#include "BatchProcessor.h"
#include "BatchProgressDialog.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QThread>
#include <QDebug>
#include <QMetaObject>

#include <cstdio>

static void cliMessageHandler(QtMsgType type,
                               const QMessageLogContext& /*context*/,
                               const QString& msg)
{
    const QByteArray text = msg.toLocal8Bit();
    switch (type)
    {
        case QtDebugMsg:
        fprintf(stdout, "[DEBUG]    %s\n", text.constData()); fflush(stdout); break;
        case QtInfoMsg:
        fprintf(stdout, "[INFO]     %s\n", text.constData()); fflush(stdout); break;
        case QtWarningMsg:
        fprintf(stderr, "[WARNING]  %s\n", text.constData()); fflush(stderr); break;
        case QtCriticalMsg:
        fprintf(stderr, "[ERROR]    %s\n", text.constData()); fflush(stderr); break;
        case QtFatalMsg:
        fprintf(stderr, "[FATAL]    %s\n", text.constData()); fflush(stderr); break;
    }
}

struct CliOptions
{
    QString inputDir;
    QString outputDir;
    bool showProgress = false;
    bool showHelp = false;
    bool showVersion = false;
};

static bool parseCli(QCoreApplication& app,
                     CliOptions& opts,
                     QString& errorMessage,
                     QString& helpText)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("CTAnalyzerX Batch Processing Tool");
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption inputDirOption(
        QStringList() << "i" << "input-dir",
        "Directory containing JSON sidecar files.",
        "input_directory");
    parser.addOption(inputDirOption);

    const QCommandLineOption outputDirOption(
        QStringList() << "o" << "output-dir",
        "Optional directory to save exported NIfTI images and processed JSON sidecar files.",
        "output_directory");
    parser.addOption(outputDirOption);

    const QCommandLineOption progressOption(
        QStringList() << "p" << "log-progress",
        "Show a progress dialog while processing and write a CSV run report.");
    parser.addOption(progressOption);

    if (!parser.parse(app.arguments()))
    {
        errorMessage = parser.errorText();
        helpText = parser.helpText();
        return false;
    }

    helpText = parser.helpText();

    if (parser.isSet("help"))
    {
        opts.showHelp = true;
        return true;
    }

    if (parser.isSet("version"))
    {
        opts.showVersion = true;
        return true;
    }

    if (!parser.isSet(inputDirOption))
    {
        errorMessage = "Missing required option: --input-dir <input_directory>";
        return false;
    }

    opts.inputDir = parser.value(inputDirOption);
    opts.outputDir = parser.isSet(outputDirOption) ? parser.value(outputDirOption) : QString();
    opts.showProgress = parser.isSet(progressOption);

    return true;
}

// ---------------------------------------------------------------------------
// formatElapsed  —  converts milliseconds to HH:mm:ss.zzz
// ---------------------------------------------------------------------------

static QString formatElapsed(qint64 elapsedMs)
{
    const qint64 ms = elapsedMs % 1000;
    const qint64 secs = (elapsedMs / 1000) % 60;
    const qint64 mins = (elapsedMs / 60'000) % 60;
    const qint64 hrs = elapsedMs / 3'600'000;

    return QStringLiteral("%1:%2:%3.%4")
        .arg(hrs, 2, 10, QChar('0'))
        .arg(mins, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

// ---------------------------------------------------------------------------
// BatchWorker  —  runs the processing loop on a dedicated thread so that
// the progress dialog on the main thread remains responsive.
// ---------------------------------------------------------------------------

class BatchWorker : public QObject
{
    Q_OBJECT

public:
    BatchWorker(const QStringList& files,
                const QString& inputFolderPath,
                const QString& outputFolderPath,
                QObject* parent = nullptr)
        : QObject(parent)
        , m_files(files)
        , m_inputFolderPath(inputFolderPath)
        , m_outputFolderPath(outputFolderPath)
        , m_abortRequested(false)
    {
    }

    // Thread-safe: called from the main thread via Qt::DirectConnection.
    // Sets the flag that the run() loop checks after each file completes.
    void requestAbort() { m_abortRequested.store(true, std::memory_order_relaxed); }

signals:
    void fileStarted(int index, const QString& sidecarBasename);
    void cropBasenameKnown(const QString& cropBasename);
    void stageAdvanced(ProcessingStage stage);
    void fileFinished(int index, bool success);
    void allFinished(int processedCount, int errorCount,
                     QVector<ProcessingRunResult> results);

public slots:
    void run()
    {
        int processedCount = 0;
        int errorCount = 0;
        QVector<ProcessingRunResult> results;
        results.reserve(m_files.size());
        QDir inputDir(m_inputFolderPath);

        for (int i = 0; i < m_files.size(); ++i)
        {
            const QString fullPath = inputDir.filePath(m_files.at(i));

            emit fileStarted(i, QFileInfo(fullPath).baseName());

            BatchProcessor::CropLoadedCallback onCropLoaded =
                [this](const QString& cropBasename)
                { emit cropBasenameKnown(cropBasename); };

            BatchProcessor::StageProgressCallback onStageAdvanced =
                [this](ProcessingStage stage)
                { emit stageAdvanced(stage); };

            BatchProcessor processor;
            ProcessingRunResult result =
                processor.processSidecarFile(
                    fullPath, m_outputFolderPath,
                    onCropLoaded, onStageAdvanced);

            results.append(result);
            result.success ? ++processedCount : ++errorCount;
            if (!result.success)
                qCritical() << "Failed to process file:" << fullPath;

            emit fileFinished(i, result.success);

            // Check abort flag after each file so the current file always
            // completes cleanly before stopping.
            if (m_abortRequested.load(std::memory_order_relaxed))
            {
                qInfo("BatchWorker: abort requested — stopping after file %d.", i + 1);
                break;
            }
        }

        emit allFinished(processedCount, errorCount, std::move(results));
    }

private:
    QStringList         m_files;
    QString             m_inputFolderPath;
    QString             m_outputFolderPath;
    std::atomic<bool>   m_abortRequested;
};

// ---------------------------------------------------------------------------
// writeCsvReport
// ---------------------------------------------------------------------------

static void writeCsvReport(const QString& csvPath,
                           const QVector<ProcessingRunResult>& results)
{
    if (results.isEmpty())
    {
        qWarning() << "writeCsvReport: no results to write; CSV not created.";
        return;
    }

    QFile csvFile(csvPath);
    if (!csvFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    {
        qWarning() << "Could not create CSV report:" << csvPath;
        return;
    }

    QTextStream out(&csvFile);

    // Header row
    out << "InputRootDir,"
        << "SidecarBasename,"
        << "CropImageBasename,"
        << "BaselineOtsuThreshold,"
        << "FinalThreshold,"
        << "TotalIterations,"
        << "SegmentedBoneVolume_mm3,"
        << "Success\n";

    // Wrap fields that may contain commas in double-quotes.
    auto quoted = [](const QString& s) -> QString
        {
            return QStringLiteral("\"%1\"").arg(QString(s).replace('"', "\"\""));
        };

    for (const ProcessingRunResult& r : results)
    {
        out << quoted(r.inputRootDir) << ","
            << quoted(r.sidecarBasename) << ","
            << quoted(r.cropBasename) << ","
            << r.baselineOtsuThreshold << ","
            << r.finalThreshold << ","
            << r.totalIterations << ","
            << r.segmentedBoneVolumeMm3 << ","
            << (r.success ? "true" : "false") << "\n";
    }

    qInfo() << "CSV report written to:" << csvPath;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    qInstallMessageHandler(cliMessageHandler);

    // Register custom types used in cross-thread queued signal/slot connections.
    // Must be called before any QThread is started that emits these signals.
    // Q_DECLARE_METATYPE alone is insufficient — runtime registration is also required.
    qRegisterMetaType<ProcessingStage>();
    qRegisterMetaType<ProcessingRunResult>();
    qRegisterMetaType<QVector<ProcessingRunResult>>();

    // QApplication is required when -p shows a GUI dialog.
    // It is a superset of QCoreApplication and safe to use unconditionally.
    QApplication app(argc, argv);
    QApplication::setApplicationName("CTAXBatchProcessor");
    QApplication::setApplicationVersion("1.0");

    CliOptions cli;
    QString cliError;
    QString cliHelp;

    if (!parseCli(app, cli, cliError, cliHelp))
    {
        QTextStream err(stderr);
        err << "CLI error: " << cliError << "\n\n";
        QTextStream out(stdout);
        out << cliHelp;
        return 1;
    }

    if (cli.showHelp)
    {
        QTextStream out(stdout);
        out << cliHelp;
        return 0;
    }

    if (cli.showVersion)
    {
        QTextStream out(stdout);
        out << QCoreApplication::applicationName()
            << " " << QCoreApplication::applicationVersion() << "\n";
        return 0;
    }

    const QString inputFolderPath = cli.inputDir;
    const QString outputFolderPath = cli.outputDir;
    const bool verbose = cli.showProgress;

    QDir inputDir(inputFolderPath);
    if (!inputDir.exists())
    {
        qCritical() << "Error: Input directory does not exist:" << inputFolderPath;
        return 1;
    }

    if (!outputFolderPath.isEmpty())
    {
        QDir outputDir(outputFolderPath);
        if (!outputDir.exists())
        {
            qWarning() << "Output directory does not exist. Attempting to create it:"
                << outputFolderPath;
            if (!outputDir.mkpath("."))
            {
                qCritical() << "Error: Could not create output directory:" << outputFolderPath;
                return 1;
            }
        }
    }

    const QStringList sidecarFiles =
        inputDir.entryList(QStringList() << "*.json",
                           QDir::Files | QDir::NoDotAndDotDot);

    if (sidecarFiles.isEmpty())
    {
        qInfo() << "No JSON sidecar files found in input directory:" << inputFolderPath;
        return 0;
    }

    // Prepare CSV output path: beside exported images when an output directory
    // is specified, otherwise beside the input sidecar files.
    const QString csvDir = outputFolderPath.isEmpty() ? inputFolderPath : outputFolderPath;
    const QString csvPath = QDir(csvDir).filePath(
        QStringLiteral("batch_report_%1.csv")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));

    // Start wall-clock timer before any processing begins.
    QElapsedTimer runTimer;
    runTimer.start();

    // -----------------------------------------------------------------------
    // Verbose mode: run worker on a background thread; show progress dialog.
    // -----------------------------------------------------------------------
    if (verbose)
    {
        BatchProgressDialog progressDialog(sidecarFiles.size());

        auto* worker = new BatchWorker(sidecarFiles, inputFolderPath, outputFolderPath);
        auto* thread = new QThread(&app);

        worker->moveToThread(thread);

        // Wire worker signals: progress dialog slots (cross-thread, queued).
        QObject::connect(worker, &BatchWorker::fileStarted,
                         &progressDialog, &BatchProgressDialog::onFileStarted);
        QObject::connect(worker, &BatchWorker::cropBasenameKnown,
                         &progressDialog, &BatchProgressDialog::onCropBasenameKnown);
        QObject::connect(worker, &BatchWorker::stageAdvanced,
                         &progressDialog, &BatchProgressDialog::onStageAdvanced);
        QObject::connect(worker, &BatchWorker::fileFinished,
                         &progressDialog, &BatchProgressDialog::onFileFinished);

        // Abort button worker flag.  DirectConnection is safe here because
        // requestAbort() only stores to an atomic: no Qt object interaction.
        QObject::connect(&progressDialog, &BatchProgressDialog::abortRequested,
                         worker, &BatchWorker::requestAbort,
                         Qt::DirectConnection);

        int  finalProcessed = 0;
        int  finalErrors = 0;
        QVector<ProcessingRunResult> allResults;

        // Guard: block until the worker is fully done before exec() can return
        // by connecting allFinished → dialog close instead of relying on the
        // user to wait. Disable the close button until allFinished fires.

        // In the connect lambda, use Qt::BlockingQueuedConnection so the
        // worker thread waits for the lambda to complete before continuing,
        // eliminating the dangling-reference window entirely.
        QObject::connect(
            worker, &BatchWorker::allFinished,
            &app,   // use a stable QObject as context, not a stack variable
            [&progressDialog, &finalProcessed, &finalErrors, &allResults]
            (int processed, int errors, QVector<ProcessingRunResult> results)
            {
                finalProcessed = processed;
                finalErrors = errors;
                allResults = std::move(results);
                progressDialog.onAllFinished(processed, errors);
                // onAllFinished enables the Close button — user can only
                // dismiss the dialog after work is complete.
            },
            Qt::QueuedConnection);

        // Start the worker when the thread starts.
        QObject::connect(thread, &QThread::started, worker, &BatchWorker::run);

        // Signal the thread to stop as soon as all work is done.
        QObject::connect(worker, &BatchWorker::allFinished,
                         thread, &QThread::quit);

        // Delete the worker once the thread has fully wound down.
        // Do NOT connect thread->finished to thread->deleteLater here:
        // progressDialog.exec() runs its own event loop, so a deleteLater
        // queued during exec() would fire before exec() returns, leaving
        // `thread` a dangling pointer for the wait() call below.
        QObject::connect(thread, &QThread::finished,
                         worker, &QObject::deleteLater);

        thread->start();
        progressDialog.exec(); // Blocks until user closes the dialog.

        // Ensure the thread has fully stopped before writing the CSV report.
        // quit() is a no-op if the thread has already stopped.
        thread->quit();
        thread->wait();

        // Safe to delete synchronously: the thread has stopped, its event
        // loop has drained, and the worker has already been scheduled for
        // deletion via the finished/deleteLater connection above.
        delete thread;

        const QString elapsed = formatElapsed(runTimer.elapsed());

        writeCsvReport(csvPath, allResults);

        qInfo() << "Batch processing complete.";
        qInfo() << "Files processed successfully:" << finalProcessed;
        qInfo() << "Files with errors:" << finalErrors;
        qInfo() << "Total run time:              " << elapsed;

        return (finalErrors > 0) ? 1 : 0;
    }

    // -----------------------------------------------------------------------
    // Non-verbose mode: synchronous loop on the main thread.
    // -----------------------------------------------------------------------
    
    int processedCount = 0;
    int errorCount = 0;
    QVector<ProcessingRunResult> allResults;
    allResults.reserve(sidecarFiles.size());

    for (const QString& fileName : sidecarFiles)
    {
        const QString fullPath = inputDir.filePath(fileName);
        qInfo() << "Processing:" << fileName;

        BatchProcessor processor;
        ProcessingRunResult result =
            processor.processSidecarFile(fullPath, outputFolderPath);

        allResults.append(result);

        if (result.success)
            ++processedCount;
        else
        {
            ++errorCount;
            qCritical() << "Failed to process file:" << fullPath;
        }
    }

    const QString elapsed = formatElapsed(runTimer.elapsed());

    writeCsvReport(csvPath, allResults);

    qInfo() << "Batch processing complete.";
    qInfo() << "Files processed successfully:" << processedCount;
    qInfo() << "Files with errors:" << errorCount;
    qInfo() << "Total run time:              " << elapsed;

    return (errorCount > 0) ? 1 : 0;
}

#include "main.moc"
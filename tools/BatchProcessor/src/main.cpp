#include "BatchProcessor.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDir>
#include <QFileInfo>
#include <QDebug>


int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("CTAXBatchProcessor");
    QCoreApplication::setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("CTAnalyzerX Batch Processing Tool");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption inputDirOption(QStringList() << "i" << "input-dir",
                                      "Directory containing JSON sidecar files.",
                                      "input_directory");
    parser.addOption(inputDirOption);

    QCommandLineOption outputDirOption(QStringList() << "o" << "output-dir",
                                       "Optional directory to save exported nifti images and processed JSON sidecar files.",
                                       "output_directory");
    parser.addOption(outputDirOption);

    parser.process(app);

    if (!parser.isSet(inputDirOption))
    {
        parser.showHelp(1);
        return 1;
    }

    QString inputFolderPath = parser.value(inputDirOption);
    QString outputFolderPath = parser.isSet(outputDirOption) ? parser.value(outputDirOption) : QString();

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
            qWarning() << "Output directory does not exist. Attempting to create it:" << outputFolderPath;
            if (!outputDir.mkpath(".")) // Creates the directory if it doesn't exist
            {
                qCritical() << "Error: Could not create output directory:" << outputFolderPath;
                return 1;
            }
        }
    }

    BatchProcessor processor;
    QStringList sidecarFiles = inputDir.entryList(QStringList() << "*.json", QDir::Files | QDir::NoDotAndDotDot);

    if (sidecarFiles.isEmpty())
    {
        qInfo() << "No JSON sidecar files found in input directory:" << inputFolderPath;
        return 0;
    }

    int processedCount = 0;
    int errorCount = 0;

    for (const QString& fileName : sidecarFiles)
    {
        QString fullPath = inputDir.filePath(fileName);
        std::cout << "procssing " << fileName.toStdString() << std::endl;
        if (processor.processSidecarFile(fullPath, outputFolderPath))
        {
            processedCount++;
        }
        else
        {
            errorCount++;
            qCritical() << "Failed to process file:" << fullPath;
        }
    }

    qInfo() << "Batch processing complete.";
    qInfo() << "Files processed successfully:" << processedCount;
    qInfo() << "Files with errors:" << errorCount;

    return (errorCount > 0) ? 1 : 0;
}
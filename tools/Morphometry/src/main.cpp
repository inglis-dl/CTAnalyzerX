#include "BoneMorphometry.h"
#include "Logger.h"
#include "MorphometryProgressDialog.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTextStream>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <vector>

namespace
{
	struct CliOptions
	{
		bool generateSurfaces = false;

		bool hasInputFile = false;
		bool hasInputDir = false;

		bool writeCsv = false;
		bool writeJson = false;

		QString inputFile;
		QString inputDir;

		// Optional prefixes (can include directory).
		QString csvPrefix;
		QString jsonPrefix;

		bool showProgress = false;
		bool showHelp = false;
		bool showVersion = false;
	};

	struct ResultRow
	{
		QString inputFile;
		QString prefix;
		bool success = false;
		QString error;

		BoneMetrics bone;
		TopologyMetrics topology;
		std::size_t numVoids = 0;
		double totalVoidVolume = 0.0;
		double totalVoidSurfaceArea = 0.0;
		std::vector<VoidMetrics> voids;

		bool surfacesWritten = false;
		QString boneSurfaceFile;
		QString voidsSurfaceFile;
	};

	static QString csvQuote(const QString& s)
	{
		QString out = s;
		out.replace('"', "\"\"");
		return QStringLiteral("\"%1\"").arg(out);
	}

	static QString derivePrefix(const QString& filePath)
	{
		const QString fileName = QFileInfo(filePath).fileName();
		const QString suffix = QStringLiteral("_export_mask.nii");
		if (fileName.endsWith(suffix, Qt::CaseInsensitive))
		{
			return fileName.left(fileName.size() - suffix.size());
		}
		return QFileInfo(filePath).completeBaseName();
	}

	static void writeConsole(FILE* stream, const QString& text)
	{
		const QByteArray utf8 = text.toUtf8();
		if (!utf8.isEmpty())
			fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stream);
		fflush(stream);
	}

	static bool parseCli(QCoreApplication& app,
						 CliOptions& opts,
						 QString& errorMessage,
						 QString& helpText)
	{
		QCommandLineParser parser;
		parser.setApplicationDescription(QStringLiteral("CTAnalyzerX Morphometry Tool"));
		parser.addHelpOption();
		parser.addVersionOption();

		const QCommandLineOption inputFileOpt(
			QStringList() << "i" << "input",
			QStringLiteral("Single input NIfTI mask file."),
			QStringLiteral("file"));

		const QCommandLineOption inputDirOpt(
			QStringList() << "d" << "input-dir",
			QStringLiteral("Input folder; recursively process *_export_mask.nii files."),
			QStringLiteral("folder"));

		const QCommandLineOption surfacesOpt(
			QStringList() << "s" << "surfaces",
			QStringLiteral("Generate surfaces: <prefix>_bone_surface.vtp and <prefix>_voids_surface.vtp."));

		const QCommandLineOption csvOpt(
			QStringList() << "c" << "csv",
			QStringLiteral("Write CSV reports (summary + void details)."));

		const QCommandLineOption csvPrefixOpt(
			QStringList() << "csv-prefix",
			QStringLiteral("CSV output prefix (optional). Produces <prefix>.csv and <prefix>_void_details.csv."),
			QStringLiteral("prefix"));

		const QCommandLineOption jsonOpt(
			QStringList() << "j" << "json",
			QStringLiteral("Write JSON report."));

		const QCommandLineOption jsonPrefixOpt(
			QStringList() << "json-prefix",
			QStringLiteral("JSON output prefix (optional). Produces <prefix>.json."),
			QStringLiteral("prefix"));

		const QCommandLineOption progressOpt(
			QStringList() << "p" << "progress",
			QStringLiteral("Show single progress gauge dialog."));

		parser.addOption(inputFileOpt);
		parser.addOption(inputDirOpt);
		parser.addOption(surfacesOpt);
		parser.addOption(csvOpt);
		parser.addOption(csvPrefixOpt);
		parser.addOption(jsonOpt);
		parser.addOption(jsonPrefixOpt);
		parser.addOption(progressOpt);

		parser.addPositionalArgument(
			QStringLiteral("file"),
			QStringLiteral("Legacy mode: single NIfTI file path."));

		if (!parser.parse(app.arguments()))
		{
			errorMessage = parser.errorText();
			helpText = parser.helpText();
			return false;
		}

		helpText = parser.helpText();

		// Do not call showHelp()/showVersion() here; let main print to stdout/stderr.
		opts.showHelp = parser.isSet(QStringLiteral("help"));
		opts.showVersion = parser.isSet(QStringLiteral("version"));
		if (opts.showHelp || opts.showVersion)
			return true;

		opts.generateSurfaces = parser.isSet(surfacesOpt);
		opts.showProgress = parser.isSet(progressOpt);

		opts.writeCsv = parser.isSet(csvOpt) || parser.isSet(csvPrefixOpt);
		opts.writeJson = parser.isSet(jsonOpt) || parser.isSet(jsonPrefixOpt);

		opts.csvPrefix = parser.value(csvPrefixOpt).trimmed();
		opts.jsonPrefix = parser.value(jsonPrefixOpt).trimmed();

		if (parser.isSet(inputFileOpt))
		{
			opts.hasInputFile = true;
			opts.inputFile = parser.value(inputFileOpt);
		}

		if (parser.isSet(inputDirOpt))
		{
			opts.hasInputDir = true;
			opts.inputDir = parser.value(inputDirOpt);
		}

		const QStringList positional = parser.positionalArguments();
		if (!opts.hasInputFile && !opts.hasInputDir && positional.size() == 1)
		{
			opts.hasInputFile = true;
			opts.inputFile = positional.first();
		}

		if (opts.hasInputFile == opts.hasInputDir)
		{
			errorMessage = QStringLiteral("Specify exactly one of --input <file> or --input-dir <folder>.");
			return false;
		}

		return true;
	}

	static std::vector<QString> collectInputs(const CliOptions& opts)
	{
		std::vector<QString> files;
		if (opts.hasInputFile)
		{
			files.push_back(QFileInfo(opts.inputFile).absoluteFilePath());
			return files;
		}

		QDirIterator it(
			opts.inputDir,
			QStringList() << QStringLiteral("*_export_mask.nii"),
			QDir::Files,
			QDirIterator::Subdirectories);

		while (it.hasNext())
		{
			files.push_back(QFileInfo(it.next()).absoluteFilePath());
		}

		std::sort(files.begin(), files.end());
		return files;
	}

	static bool writeCsvReport(const QString& csvPath, const std::vector<ResultRow>& rows)
	{
		QFile f(csvPath);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		{
			std::cerr << "Error: could not write CSV: " << csvPath.toStdString() << "\n";
			return false;
		}

		QTextStream out(&f);
		out.setRealNumberNotation(QTextStream::FixedNotation);
		out.setRealNumberPrecision(10);

		out << "InputFile,Prefix,Success,Error,"
			<< "BVTV,VoxelBSBV,SMI,TbTh,TbN,TbSp,"
			<< "BoneVoxelVolume,BoneMeshVolume,BoneMeshSurfaceArea,BoneMeshBSBV,BoneMeshEuler,"
			<< "TopologyEuler,TopologyCavities,TopologyGenus,"
			<< "NumVoids,TotalVoidVolume,TotalVoidSurfaceArea,"
			<< "SurfacesWritten,BoneSurfaceFile,VoidsSurfaceFile\n";

		for (const auto& r : rows)
		{
			out << csvQuote(r.inputFile) << ","
				<< csvQuote(r.prefix) << ","
				<< (r.success ? "true" : "false") << ","
				<< csvQuote(r.error) << ","
				<< r.bone.VoxelBVTV << ","
				<< r.bone.VoxelBSBV << ","
				<< r.bone.StructureModelIndex << ","
				<< r.bone.VoxelTrabecularThickness << ","
				<< r.bone.VoxelTrabecularNumber << ","
				<< r.bone.VoxelTrabecularSpacing << ","
				<< r.bone.VoxelVolume << ","
				<< r.bone.MeshVolume << ","
				<< r.bone.MeshSurfaceArea << ","
				<< r.bone.MeshBSBV << ","
				<< r.bone.MeshEulerNumber << ","
				<< r.topology.EulerNumber << ","
				<< r.topology.NumCavities << ","
				<< r.topology.Genus << ","
				<< static_cast<qulonglong>(r.numVoids) << ","
				<< r.totalVoidVolume << ","
				<< r.totalVoidSurfaceArea << ","
				<< (r.surfacesWritten ? "true" : "false") << ","
				<< csvQuote(r.boneSurfaceFile) << ","
				<< csvQuote(r.voidsSurfaceFile)
				<< "\n";
		}

		return true;
	}

	static QString deriveVoidDetailsCsvPath(const QString& csvPath)
	{
		const QFileInfo info(csvPath);
		return QDir(info.absolutePath()).filePath(info.completeBaseName() + QStringLiteral("_void_details.csv"));
	}

	static bool writeVoidDetailsCsvReport(const QString& csvPath, const std::vector<ResultRow>& rows)
	{
		QFile f(csvPath);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		{
			std::cerr << "Error: could not write void details CSV: " << csvPath.toStdString() << "\n";
			return false;
		}

		QTextStream out(&f);
		out.setRealNumberNotation(QTextStream::FixedNotation);
		out.setRealNumberPrecision(10);

		out << "InputFile,Prefix,VoidIndex,"
			<< "VoxelVolume,VoxelElongation,VoxelFlatness,VoxelRoundness,"
			<< "MeshSurfaceArea,VoxelEquivalentDiameter,MeshSphericity\n";

		for (const auto& r : rows)
		{
			for (std::size_t i = 0; i < r.voids.size(); ++i)
			{
				const auto& v = r.voids[i];
				out << csvQuote(r.inputFile) << ","
					<< csvQuote(r.prefix) << ","
					<< static_cast<qulonglong>(i + 1) << ","
					<< v.VoxelVolume << ","
					<< v.VoxelElongation << ","
					<< v.VoxelFlatness << ","
					<< v.VoxelRoundness << ","
					<< v.MeshSurfaceArea << ","
					<< v.VoxelEquivalentDiameter << ","
					<< v.MeshSphericity
					<< "\n";
			}
		}

		return true;
	}

	static bool writeJsonReport(const QString& jsonPath, const std::vector<ResultRow>& rows)
	{
		QJsonArray resultsArray;

		for (const auto& r : rows)
		{
			QJsonObject boneObj;
			boneObj["bvtv"] = r.bone.VoxelBVTV;
			boneObj["voxelBSBV"] = r.bone.VoxelBSBV;
			boneObj["smi"] = r.bone.StructureModelIndex;
			boneObj["tbTh"] = r.bone.VoxelTrabecularThickness;
			boneObj["tbN"] = r.bone.VoxelTrabecularNumber;
			boneObj["tbSp"] = r.bone.VoxelTrabecularSpacing;
			boneObj["voxelVolume"] = r.bone.VoxelVolume;
			boneObj["meshVolume"] = r.bone.MeshVolume;
			boneObj["meshSurfaceArea"] = r.bone.MeshSurfaceArea;
			boneObj["meshBSBV"] = r.bone.MeshBSBV;
			boneObj["meshEulerNumber"] = r.bone.MeshEulerNumber;

			QJsonObject topoObj;
			topoObj["euler"] = r.topology.EulerNumber;
			topoObj["cavities"] = r.topology.NumCavities;
			topoObj["genus"] = r.topology.Genus;

			QJsonObject voidObj;
			voidObj["count"] = static_cast<qint64>(r.numVoids);
			voidObj["totalVolume"] = r.totalVoidVolume;
			voidObj["totalSurfaceArea"] = r.totalVoidSurfaceArea;

			QJsonArray voidDetailsArray;
			for (std::size_t i = 0; i < r.voids.size(); ++i)
			{
				const auto& v = r.voids[i];
				QJsonObject voidDetailObj;
				voidDetailObj["voidIndex"] = static_cast<qint64>(i + 1);
				voidDetailObj["voxelVolume"] = v.VoxelVolume;
				voidDetailObj["voxelElongation"] = v.VoxelElongation;
				voidDetailObj["voxelFlatness"] = v.VoxelFlatness;
				voidDetailObj["voxelRoundness"] = v.VoxelRoundness;
				voidDetailObj["meshSurfaceArea"] = v.MeshSurfaceArea;
				voidDetailObj["voxelEquivalentDiameter"] = v.VoxelEquivalentDiameter;
				voidDetailObj["meshSphericity"] = v.MeshSphericity;
				voidDetailsArray.push_back(voidDetailObj);
			}

			QJsonObject surfObj;
			surfObj["written"] = r.surfacesWritten;
			surfObj["boneFile"] = r.boneSurfaceFile;
			surfObj["voidsFile"] = r.voidsSurfaceFile;

			QJsonObject rowObj;
			rowObj["inputFile"] = r.inputFile;
			rowObj["prefix"] = r.prefix;
			rowObj["success"] = r.success;
			rowObj["error"] = r.error;
			rowObj["bone"] = boneObj;
			rowObj["topology"] = topoObj;
			rowObj["voids"] = voidObj;
			rowObj["voidDetails"] = voidDetailsArray;
			rowObj["surfaces"] = surfObj;

			resultsArray.push_back(rowObj);
		}

		QJsonObject root;
		root["generatedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
		root["results"] = resultsArray;

		QFile f(jsonPath);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			std::cerr << "Error: could not write JSON: " << jsonPath.toStdString() << "\n";
			return false;
		}

		const QJsonDocument doc(root);
		f.write(doc.toJson(QJsonDocument::Indented));
		return true;
	}

	static QString stageLabelFromId(int stageId)
	{
		switch (stageId)
		{
			case 0: return QStringLiteral("Load");
			case 1: return QStringLiteral("Bone metrics");
			case 2: return QStringLiteral("Topology");
			case 3: return QStringLiteral("Void metrics");
			case 4: return QStringLiteral("Surfaces");
			case 5: return QStringLiteral("Complete");
			default: return QStringLiteral("Working");
		}
	}

	static QString defaultOutputDir(const CliOptions& opts, const std::vector<QString>& inputs)
	{
		if (opts.hasInputFile)
		{
			return QFileInfo(opts.inputFile).absolutePath();
		}
		if (opts.hasInputDir)
		{
			return QDir(opts.inputDir).absolutePath();
		}
		if (!inputs.empty())
		{
			return QFileInfo(inputs.front()).absolutePath();
		}
		return QDir::currentPath();
	}

	static QString resolveReportPath(
		const QString& prefix,
		const QString& fallbackDir,
		const QString& defaultBaseName,
		const QString& extension)
	{
		QString outDir = fallbackDir;
		QString baseName = defaultBaseName;

		if (!prefix.isEmpty())
		{
			const QFileInfo pInfo(prefix);
			const QString pPath = pInfo.path();

			if (!pPath.isEmpty() && pPath != QStringLiteral("."))
			{
				outDir = QDir::isAbsolutePath(pPath)
					? QDir(pPath).absolutePath()
					: QDir(fallbackDir).absoluteFilePath(pPath);
			}

			baseName = pInfo.fileName();
			if (baseName.isEmpty())
			{
				baseName = defaultBaseName;
			}
		}

		const QString dotExt = QStringLiteral(".") + extension;
		if (baseName.endsWith(dotExt, Qt::CaseInsensitive))
		{
			baseName.chop(dotExt.size());
		}

		return QDir(outDir).filePath(baseName + dotExt);
	}
}

class MorphometryWorker : public QObject
{
	Q_OBJECT

public:
	MorphometryWorker(std::vector<QString> files, bool generateSurfaces, QObject* parent = nullptr)
		: QObject(parent)
		, m_files(std::move(files))
		, m_generateSurfaces(generateSurfaces)
	{
	}

	void requestAbort()
	{
		m_abortRequested.store(true, std::memory_order_relaxed);
	}

	const std::vector<ResultRow>& results() const
	{
		return m_rows;
	}

	int successCount() const
	{
		return m_successCount;
	}

	int errorCount() const
	{
		return m_errorCount;
	}

signals:
	void fileStarted(int index, int total, const QString& filePath);
	void fileStageChanged(int index, int total, const QString& filePath, int stageId);
	void fileFinished(int index, int total, bool success, const QString& message);
	void allFinished(int successCount, int errorCount);

public slots:
	void run()
	{
		m_rows.clear();
		m_rows.reserve(m_files.size());
		m_successCount = 0;
		m_errorCount = 0;

		const int total = static_cast<int>(m_files.size());

		for (int i = 0; i < total; ++i)
		{
			if (m_abortRequested.load(std::memory_order_relaxed))
			{
				break;
			}

			const QString filePath = m_files[static_cast<std::size_t>(i)];
			emit fileStarted(i, total, filePath);

			ResultRow row;
			row.inputFile = filePath;
			row.prefix = derivePrefix(filePath);

			emit fileStageChanged(i, total, filePath, 0); // Load
			auto image = BoneMorphometry::LoadImage(filePath.toStdString());
			if (!image)
			{
				row.success = false;
				row.error = QStringLiteral("Failed to load image.");
				m_rows.push_back(row);
				++m_errorCount;
				emit fileFinished(i, total, false, row.error);
				continue;
			}

			emit fileStageChanged(i, total, filePath, 1); // Bone metrics
			if (!BoneMorphometry::ComputeBoneMetrics(image, row.bone))
			{
				row.success = false;
				row.error = QStringLiteral("Failed to compute bone metrics.");
				m_rows.push_back(row);
				++m_errorCount;
				emit fileFinished(i, total, false, row.error);
				continue;
			}

			emit fileStageChanged(i, total, filePath, 2); // Topology
			row.topology = BoneMorphometry::ComputeTopology(image);

			emit fileStageChanged(i, total, filePath, 3); // Void metrics
			std::vector<VoidMetrics> voids;
			if (!BoneMorphometry::ComputeVoidMetrics(image, voids, row.totalVoidVolume, row.totalVoidSurfaceArea))
			{
				row.success = false;
				row.error = QStringLiteral("Failed to compute void metrics.");
				m_rows.push_back(row);
				++m_errorCount;
				emit fileFinished(i, total, false, row.error);
				continue;
			}

			row.numVoids = voids.size();
			row.voids = std::move(voids);

			if (m_generateSurfaces)
			{
				emit fileStageChanged(i, total, filePath, 4); // Surfaces

				const QString dir = QFileInfo(filePath).absolutePath();
				row.boneSurfaceFile = QDir(dir).filePath(row.prefix + QStringLiteral("_bone_surface.vtp"));
				row.voidsSurfaceFile = QDir(dir).filePath(row.prefix + QStringLiteral("_voids_surface.vtp"));

				row.surfacesWritten = BoneMorphometry::WriteSurfaces(
					image,
					row.boneSurfaceFile.toStdString(),
					row.voidsSurfaceFile.toStdString());

				if (!row.surfacesWritten)
				{
					row.success = false;
					row.error = QStringLiteral("Surface generation failed.");
					m_rows.push_back(row);
					++m_errorCount;
					emit fileFinished(i, total, false, row.error);
					continue;
				}
			}

			emit fileStageChanged(i, total, filePath, 5); // Complete

			row.success = true;
			m_rows.push_back(row);
			++m_successCount;
			emit fileFinished(i, total, true, QString());
		}

		emit allFinished(m_successCount, m_errorCount);
	}

private:
	std::vector<QString> m_files;
	bool m_generateSurfaces = false;
	std::atomic<bool> m_abortRequested{ false };

	std::vector<ResultRow> m_rows;
	int m_successCount = 0;
	int m_errorCount = 0;
};

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("CTAnalyzerX"));
	QCoreApplication::setApplicationName(QStringLiteral("CTAXMorphometry"));

	// Parse CLI before installing Logger so --help/--version output remains
	// standard console behavior (stdout/stderr) and exits cleanly.
	CliOptions opts;
	QString cliError;
	QString cliHelp;

	if (!parseCli(app, opts, cliError, cliHelp))
	{
		if (!cliError.isEmpty())
			writeConsole(stderr, QStringLiteral("CLI error: ") + cliError + QStringLiteral("\n\n"));
		if (!cliHelp.isEmpty())
			writeConsole(stdout, cliHelp);
		return EXIT_FAILURE;
	}

	if (opts.showHelp)
	{
		writeConsole(stdout, cliHelp);
		return EXIT_SUCCESS;
	}

	if (opts.showVersion)
	{
		writeConsole(stdout,
			QCoreApplication::applicationName()
			+ QStringLiteral(" ")
			+ QCoreApplication::applicationVersion()
			+ QStringLiteral("\n"));
		return EXIT_SUCCESS;
	}

	// Install logger only for actual processing flow.
	Logger::setChannel(QStringLiteral("Morphometry"));
	Logger::setSingleSharedFile(true); // or false
	Logger::install();
	QObject::connect(&app, &QCoreApplication::aboutToQuit, []() { Logger::uninstall(); });

	const std::vector<QString> inputs = collectInputs(opts);

	const QString reportBaseName = QStringLiteral("morphometry_report_%1")
		.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
	const QString reportDir = defaultOutputDir(opts, inputs);

	const QString summaryCsvPath = opts.writeCsv
		? resolveReportPath(opts.csvPrefix, reportDir, reportBaseName, QStringLiteral("csv"))
		: QString();

	const QString voidDetailsCsvPath = opts.writeCsv
		? deriveVoidDetailsCsvPath(summaryCsvPath)
		: QString();

	const QString jsonReportPath = opts.writeJson
		? resolveReportPath(opts.jsonPrefix, reportDir, reportBaseName, QStringLiteral("json"))
		: QString();

	MorphometryProgressDialog* progressDialog = nullptr;
	if (opts.showProgress)
	{
		progressDialog = new MorphometryProgressDialog(static_cast<int>(inputs.size()));
		progressDialog->show();
	}

	auto* worker = new MorphometryWorker(inputs, opts.generateSurfaces);
	auto* workerThread = new QThread(&app);
	worker->moveToThread(workerThread);

	QEventLoop doneLoop;
	int finalSuccessCount = 0;
	int finalErrorCount = 0;

	QObject::connect(workerThread, &QThread::started, worker, &MorphometryWorker::run);

	if (progressDialog)
	{
		QObject::connect(worker, &MorphometryWorker::fileStarted,
			progressDialog, &MorphometryProgressDialog::onFileStarted);

		QObject::connect(worker, &MorphometryWorker::fileStageChanged,
			progressDialog, &MorphometryProgressDialog::onFileStageChanged);

		QObject::connect(worker, &MorphometryWorker::fileFinished,
			progressDialog, &MorphometryProgressDialog::onFileFinished);

		QObject::connect(worker, &MorphometryWorker::allFinished,
			progressDialog, &MorphometryProgressDialog::onAllFinished);

		QObject::connect(progressDialog, &MorphometryProgressDialog::abortRequested,
			worker, &MorphometryWorker::requestAbort,
			Qt::QueuedConnection);
	}

	QObject::connect(worker, &MorphometryWorker::allFinished, &app,
		[&](int successCount, int errorCount)
		{
			finalSuccessCount = successCount;
			finalErrorCount = errorCount;
			doneLoop.quit();
		});

	workerThread->start();
	doneLoop.exec();

	workerThread->quit();
	workerThread->wait();

	std::vector<ResultRow> rows = worker->results();

	delete worker;
	delete workerThread;

	if (progressDialog)
	{
		progressDialog->close();
		delete progressDialog;
	}

	const bool hasResultsToWrite = !rows.empty();

	if (opts.writeCsv)
	{
		if (hasResultsToWrite)
		{
			writeCsvReport(summaryCsvPath, rows);
			writeVoidDetailsCsvReport(voidDetailsCsvPath, rows);
		}
		else
		{
			qWarning().noquote() << "Skipping CSV export: no results to write.";
		}
	}

	if (opts.writeJson)
	{
		if (hasResultsToWrite)
		{
			writeJsonReport(jsonReportPath, rows);
		}
		else
		{
			qWarning().noquote() << "Skipping JSON export: no results to write.";
		}
	}

	qInfo().noquote() << "Completed. Success: " << finalSuccessCount
		<< ", Failed: " << finalErrorCount;

	return (finalSuccessCount > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "main.moc"
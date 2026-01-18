#include "Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QDebug>
#include <QMessageLogContext>
#include <QTextStream>
#include <QFileInfo>
#include <QDateTime>
#include <algorithm>

#include <vtkOutputWindow.h>
#include <vtkObjectFactory.h>

// -------------------- VTK OutputWindow redirect --------------------
// Small vtkOutputWindow subclass that forwards messages into Logger.
// Placed here so implementation is local to the project.
class LocalVTKOutputWindow : public vtkOutputWindow
{
public:
	static LocalVTKOutputWindow* New();
	vtkTypeMacro(LocalVTKOutputWindow, vtkOutputWindow);

	void DisplayText(const char* txt) override
	{
		if (txt) Logger::writeLine(QString::fromUtf8(txt));
	}
	void DisplayWarningText(const char* txt) override
	{
		if (txt) Logger::writeLine(QStringLiteral("VTK WARNING: %1").arg(QString::fromUtf8(txt)));
	}
	void DisplayErrorText(const char* txt) override
	{
		if (txt) Logger::writeLine(QStringLiteral("VTK ERROR: %1").arg(QString::fromUtf8(txt)));
	}
};
vtkStandardNewMacro(LocalVTKOutputWindow);

// -------------------- Logger implementation --------------------
Logger* Logger::s_instance = nullptr;

// Default runtime configuration
bool Logger::s_rotateEnabled = true;
int Logger::s_maxBackupFiles = 5;
int Logger::s_maxFileSizeMB = 5;

// Qt message handler that forwards to Logger
static void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
	Q_UNUSED(context);
	const QString t = qFormatLogMessage(type, context, msg);
	Logger::writeLine(t);
}

Logger::Logger()
	: m_stream(&m_file)
{
	// replace VTK output window (prevents VTK modal popups)
	m_vtkOutputWindow = vtkSmartPointer<LocalVTKOutputWindow>::New();
	openLog();
}

Logger::~Logger()
{
	// restore std::cerr if redirected
	if (m_oldCerr) {
		std::cerr.rdbuf(m_oldCerr);
		m_oldCerr = nullptr;
	}
	closeLog();
}

void Logger::install()
{
	if (s_instance) return;
	// create singleton
	s_instance = new Logger();

	// install Qt handler
	qInstallMessageHandler(qtMessageHandler);

	// replace VTK output window (prevents VTK modal popups)
	vtkOutputWindow::SetInstance(s_instance->GetVTKOutputWindow());
}

LocalVTKOutputWindow* Logger::GetVTKOutputWindow() const
{
	return m_vtkOutputWindow;
}

void Logger::uninstall()
{
	if (!s_instance) return;

	// Restore VTK output window to default by passing nullptr (VTK will recreate default on demand)
	vtkOutputWindow::SetInstance(nullptr);

	// uninstall Qt handler: install a no-op handler (nullptr not permitted), install fallback that writes to stderr
	qInstallMessageHandler([](QtMsgType t, const QMessageLogContext& c, const QString& m) {
		QByteArray ba = qFormatLogMessage(t, c, m).toLocal8Bit();
		fwrite(ba.constData(), 1, ba.size(), stderr);
		fputc('\n', stderr);
	});

	delete s_instance;
	s_instance = nullptr;
}

// Runtime configuration API
void Logger::setRotateEnabled(bool on) { s_rotateEnabled = on; }
bool Logger::rotateEnabled() { return s_rotateEnabled; }

void Logger::setMaxBackupFiles(int n) { s_maxBackupFiles = (n > 0 ? n : 1); }
int Logger::maxBackupFiles() { return s_maxBackupFiles; }

void Logger::setMaxFileSizeMB(int mb) { s_maxFileSizeMB = (mb > 0 ? mb : 1); }
int Logger::maxFileSizeMB() { return s_maxFileSizeMB; }

void Logger::openLog()
{
	QMutexLocker locker(&m_mutex);

	QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (dir.isEmpty())
		dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	QDir d(dir);
	if (!d.exists())
		d.mkpath(QStringLiteral("."));

	QString path = d.filePath(QStringLiteral("CTAnalyzerX.log"));

	m_file.setFileName(path);
	if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
		// fallback to temp
		QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
		path = QDir(tmp).filePath(QStringLiteral("CTAnalyzerX.log"));
		m_file.setFileName(path);
		m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
	}
	m_stream.setDevice(&m_file);

	// header
	m_stream << "---- CTAnalyzerX log started: " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ----\n";
	m_stream.flush();

	// redirect std::cerr to same file to capture libraries that write to cerr
	if (!m_ofs) {
		m_ofs = std::make_unique<std::ofstream>(m_file.fileName().toLocal8Bit().constData(), std::ios::app);
		if (m_ofs && m_ofs->good()) {
			m_oldCerr = std::cerr.rdbuf();
			std::cerr.rdbuf(m_ofs->rdbuf());
		}
		else {
			m_ofs.reset();
		}
	}
}

void Logger::closeLog()
{
	QMutexLocker locker(&m_mutex);
	if (m_stream.device()) m_stream.flush();
	if (m_file.isOpen()) m_file.close();
}

void Logger::rotateLogsIfNeeded()
{
	// must be called with mutex locked
	if (!s_rotateEnabled) return;

	const QString base = m_file.fileName();
	if (base.isEmpty()) return;

	qint64 size = 0;
	if (m_file.isOpen()) {
		size = m_file.size();
	}
	else {
		QFileInfo fi(base);
		if (fi.exists()) size = fi.size();
	}

	const qint64 threshold = static_cast<qint64>(s_maxFileSizeMB) * 1024 * 1024;
	if (threshold <= 0) return;
	if (size < threshold) return;

	// perform rotation (close, rename to timestamped backup, prune, reopen)
	// flush and close current file device
	if (m_stream.device()) m_stream.flush();
	if (m_file.isOpen()) m_file.close();

	// restore stderr redirection so rename operations won't fail on Windows
	if (m_ofs) {
		if (m_oldCerr) {
			std::cerr.rdbuf(m_oldCerr);
			m_oldCerr = nullptr;
		}
		m_ofs.reset();
	}

	// create timestamped backup name: CTAnalyzerX_YYYYMMDD_HHmm.log
	QFileInfo baseInfo(base);
	QDir dir = baseInfo.absoluteDir();
	const QString prefix = QStringLiteral("CTAnalyzerX_");
	QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmm"));
	QString backupName = QStringLiteral("%1%2.log").arg(prefix, timestamp);
	QString backupPath = dir.filePath(backupName);

	// if a collision occurs (same minute), try adding seconds, then a numeric suffix
	if (QFile::exists(backupPath)) {
		timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
		backupName = QStringLiteral("%1%2.log").arg(prefix, timestamp);
		backupPath = dir.filePath(backupName);
		int idx = 1;
		while (QFile::exists(backupPath)) {
			backupName = QStringLiteral("%1%2_%3.log").arg(prefix, timestamp).arg(idx);
			backupPath = dir.filePath(backupName);
			++idx;
		}
	}

	// move current log to timestamped backup
	if (QFile::exists(base)) {
		QFile::rename(base, backupPath);
	}

	// prune older timestamped backups if we keep a limited number
	if (s_maxBackupFiles > 0) {
		// list CTAnalyzerX_*.log files
		QStringList nameFilters;
		nameFilters << QStringLiteral("CTAnalyzerX_*.log");
		QFileInfoList infos = dir.entryInfoList(nameFilters, QDir::Files | QDir::NoSymLinks, QDir::Time); // newest first

		// sort by lastModified ascending (oldest first) so we remove oldest entries
		std::sort(infos.begin(), infos.end(), [](const QFileInfo& a, const QFileInfo& b) {
			return a.lastModified() < b.lastModified();
		});

		// remove oldest until only s_maxBackupFiles remain
		int removeCount = static_cast<int>(infos.size()) - s_maxBackupFiles;
		for (int i = 0; i < removeCount; ++i) {
			const QFileInfo& fi = infos.at(i);
			QFile::remove(fi.absoluteFilePath());
		}
	}

	// reopen a fresh log (this will recreate m_ofs and redirect stderr)
	openLog();
}

void Logger::writeInternal(const QString& line)
{
	QMutexLocker locker(&m_mutex);
	if (!m_stream.device()) {
		// attempt reopen
		openLog();
	}

	// Check file size and rotate strictly based on size before writing
	rotateLogsIfNeeded();

	const QString out = QStringLiteral("[%1] %2\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate), line);
	m_stream << out;
	m_stream.flush();
}

void Logger::writeLine(const QString& line)
{
	if (s_instance) {
		s_instance->writeInternal(line);
	}
	else {
		// fallback to stderr
		QByteArray ba = line.toLocal8Bit();
		fwrite(ba.constData(), 1, ba.size(), stderr);
		fputc('\n', stderr);
		fflush(stderr);
	}
}
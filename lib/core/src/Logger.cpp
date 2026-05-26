#include "Logger.h"
#include "VtkQtOutputWindow.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QMessageLogContext>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <cstdio>

#include <vtkObjectFactory.h>
#include <vtkOutputWindow.h>

// -------------------- Logger implementation --------------------
Logger* Logger::s_instance = nullptr;

// Default runtime configuration
bool Logger::s_rotateEnabled = true;
int Logger::s_maxBackupFiles = 5;
int Logger::s_maxFileSizeMB = 5;

QString Logger::s_umbrellaLogRoot;
QString Logger::s_channel = QStringLiteral("Unknown");
bool Logger::s_singleSharedFile = false;

void Logger::setUmbrellaLogRoot(const QString& path) { s_umbrellaLogRoot = path; }
void Logger::setChannel(const QString& channel) { s_channel = channel; }
void Logger::setSingleSharedFile(bool on) { s_singleSharedFile = on; }

// Keep previous Qt handler so uninstall restores original behavior.
static QtMessageHandler s_previousQtMessageHandler = nullptr;

// Qt message handler that forwards to Logger
static void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
	const QString t = qFormatLogMessage(type, context, msg);
	Logger::writeLine(t);
}

Logger::Logger()
	: m_stream(&m_file)
{
	openLog();
}

Logger::~Logger()
{
	// restore std::cerr if redirected
	if (m_oldCerr)
	{
		std::cerr.rdbuf(m_oldCerr);
		m_oldCerr = nullptr;
	}
	closeLog();
}

void Logger::install()
{
	if (s_instance)
		return;

	// create singleton (constructor opens log)
	s_instance = new Logger();

	// Save current VTK output window so uninstall can restore it
	s_instance->m_previousVtkOutputWindow = vtkOutputWindow::GetInstance();

	// Route VTK -> Qt logging (qDebug/qWarning/qCritical)
	s_instance->m_vtkOutputWindow = vtkSmartPointer<VtkQtOutputWindow>::New();
	vtkOutputWindow::SetInstance(s_instance->m_vtkOutputWindow);

	// install Qt handler and keep previous
	s_previousQtMessageHandler = qInstallMessageHandler(qtMessageHandler);
}

VtkQtOutputWindow* Logger::GetVTKOutputWindow() const
{
	return m_vtkOutputWindow;
}

void Logger::uninstall()
{
	if (!s_instance)
		return;

	// Restore prior VTK output sink
	if (s_instance->m_previousVtkOutputWindow)
		vtkOutputWindow::SetInstance(s_instance->m_previousVtkOutputWindow);
	else
		vtkOutputWindow::SetInstance(nullptr);

	s_instance->m_vtkOutputWindow = nullptr;
	s_instance->m_previousVtkOutputWindow = nullptr;

	// Restore previous Qt message handler
	qInstallMessageHandler(s_previousQtMessageHandler);
	s_previousQtMessageHandler = nullptr;

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
	openLogUnlocked();
}

void Logger::openLogUnlocked()
{
	QString root = s_umbrellaLogRoot;
	if (root.isEmpty())
	{
		root = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
			.filePath(QStringLiteral("CTAnalyzerX/logs"));
	}

	QDir d(root);
	if (!d.exists())
		d.mkpath(QStringLiteral("."));

	QString fileName;
	if (s_singleSharedFile)
	{
		fileName = QStringLiteral("CTAnalyzerX.shared.log");
	}
	else
	{
		const qint64 pid = QCoreApplication::applicationPid();
		fileName = QStringLiteral("%1_%2.log").arg(s_channel).arg(pid);
	}

	m_file.setFileName(d.filePath(fileName));
	if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
	{
		// fallback to temp
		const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
		m_file.setFileName(QDir(tmp).filePath(QStringLiteral("CTAnalyzerX.log")));
		m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
	}

	m_stream.setDevice(&m_file);

	// header
	m_stream << "---- CTAnalyzerX log started: "
		<< QDateTime::currentDateTime().toString(Qt::ISODate)
		<< " ----\n";
	m_stream.flush();

	// redirect std::cerr to same file to capture libraries that write to cerr
	if (!m_ofs)
	{
		m_ofs = std::make_unique<std::ofstream>(m_file.fileName().toLocal8Bit().constData(), std::ios::app);
		if (m_ofs && m_ofs->good())
		{
			m_oldCerr = std::cerr.rdbuf();
			std::cerr.rdbuf(m_ofs->rdbuf());
		}
		else
		{
			m_ofs.reset();
		}
	}
}

void Logger::closeLog()
{
	QMutexLocker locker(&m_mutex);
	closeLogUnlocked();
}

void Logger::closeLogUnlocked()
{
	if (m_stream.device())
		m_stream.flush();

	if (m_file.isOpen())
		m_file.close();
}

void Logger::rotateLogsIfNeeded()
{
	// must be called with mutex locked
	if (!s_rotateEnabled)
		return;

	const QString base = m_file.fileName();
	if (base.isEmpty())
		return;

	qint64 size = 0;
	if (m_file.isOpen())
		size = m_file.size();
	else
	{
		QFileInfo fi(base);
		if (fi.exists())
			size = fi.size();
	}

	const qint64 threshold = static_cast<qint64>(s_maxFileSizeMB) * 1024 * 1024;
	if (threshold <= 0 || size < threshold)
		return;

	// flush and close current file device
	if (m_stream.device())
		m_stream.flush();
	if (m_file.isOpen())
		m_file.close();

	// restore stderr redirection so rename operations won't fail on Windows
	if (m_ofs)
	{
		if (m_oldCerr)
		{
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

	// collision handling
	if (QFile::exists(backupPath))
	{
		timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
		backupName = QStringLiteral("%1%2.log").arg(prefix, timestamp);
		backupPath = dir.filePath(backupName);
		int idx = 1;
		while (QFile::exists(backupPath))
		{
			backupName = QStringLiteral("%1%2_%3.log").arg(prefix, timestamp).arg(idx);
			backupPath = dir.filePath(backupName);
			++idx;
		}
	}

	if (QFile::exists(base))
		QFile::rename(base, backupPath);

	if (s_maxBackupFiles > 0)
	{
		QStringList nameFilters;
		nameFilters << QStringLiteral("CTAnalyzerX_*.log");
		QFileInfoList infos = dir.entryInfoList(nameFilters, QDir::Files | QDir::NoSymLinks, QDir::Time);

		std::sort(infos.begin(), infos.end(), [](const QFileInfo& a, const QFileInfo& b) {
			return a.lastModified() < b.lastModified();
		});

		const int removeCount = static_cast<int>(infos.size()) - s_maxBackupFiles;
		for (int i = 0; i < removeCount; ++i)
			QFile::remove(infos.at(i).absoluteFilePath());
	}

	// reopen fresh log without re-locking mutex
	openLogUnlocked();
}

void Logger::writeInternal(const QString& line)
{
	// Inter-process lock only needed for single shared file mode.
	if (s_singleSharedFile)
	{
		static QSystemSemaphore ipcLock(QStringLiteral("CTAnalyzerX.Logger.SharedFile"), 1);
		ipcLock.acquire();

		{
			QMutexLocker locker(&m_mutex);
			if (!m_stream.device())
				openLogUnlocked();
			rotateLogsIfNeeded();

			const QString out = QStringLiteral("[%1][%2][pid:%3] %4\n")
				.arg(QDateTime::currentDateTime().toString(Qt::ISODate))
				.arg(s_channel)
				.arg(QCoreApplication::applicationPid())
				.arg(line);
			m_stream << out;
			m_stream.flush();
		}

		ipcLock.release();
		return;
	}

	QMutexLocker locker(&m_mutex);
	if (!m_stream.device())
		openLogUnlocked();
	rotateLogsIfNeeded();

	const QString out = QStringLiteral("[%1][%2][pid:%3] %4\n")
		.arg(QDateTime::currentDateTime().toString(Qt::ISODate))
		.arg(s_channel)
		.arg(QCoreApplication::applicationPid())
		.arg(line);
	m_stream << out;
	m_stream.flush();
}

void Logger::writeLine(const QString& line)
{
	if (s_instance)
	{
		s_instance->writeInternal(line);
	}
	else
	{
		// fallback to stderr
		const QByteArray ba = line.toLocal8Bit();
		fwrite(ba.constData(), 1, ba.size(), stderr);
		fputc('\n', stderr);
		fflush(stderr);
	}
}
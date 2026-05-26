#pragma once

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QSystemSemaphore>
#include <QTextStream>

#include <memory>
#include <ostream>
#include <fstream>

#include <vtkSmartPointer.h>

class vtkOutputWindow;
class VtkQtOutputWindow;

class Logger
{
public:
	// Install the logger (call after QApplication construction)
	static void install();

	// Optional: uninstall (restores stderr and vtk output window)
	static void uninstall();

	// Write a line to the log (thread-safe)
	static void writeLine(const QString& line);

	// Runtime config (set before install)
	static void setUmbrellaLogRoot(const QString& path);   // e.g. %LOCALAPPDATA%/CTAnalyzerX/logs
	static void setChannel(const QString& channel);        // App/Prototype/Viewer/Morphometry
	static void setSingleSharedFile(bool on);              // true => CTAnalyzerX.shared.log


	VtkQtOutputWindow* GetVTKOutputWindow() const;

	// Runtime configuration (call before Logger::install to affect behavior)
	static void setRotateEnabled(bool on);
	static bool rotateEnabled();

	static void setMaxBackupFiles(int n);
	static int maxBackupFiles();

	static void setMaxFileSizeMB(int mb);
	static int maxFileSizeMB();

private:
	Logger();
	~Logger();

	void openLog();
	void closeLog();
	void writeInternal(const QString& line);

	// assumes m_mutex is already held
	void openLogUnlocked();
	void closeLogUnlocked();

	// rotation helpers (size-based rotation happens from writeInternal)
	void rotateLogsIfNeeded();

	QFile m_file;
	QTextStream m_stream;
	QMutex m_mutex;
	static QString s_umbrellaLogRoot;
	static QString s_channel;
	static bool s_singleSharedFile;

	// rotation config (static runtime-configurable)
	static bool s_rotateEnabled;
	static int s_maxBackupFiles;
	static int s_maxFileSizeMB;

	// redirect std::cerr
	std::unique_ptr<std::ofstream> m_ofs;
	std::streambuf* m_oldCerr = nullptr;

	vtkSmartPointer<VtkQtOutputWindow> m_vtkOutputWindow;
	vtkSmartPointer<vtkOutputWindow> m_previousVtkOutputWindow;

	static Logger* s_instance;
};
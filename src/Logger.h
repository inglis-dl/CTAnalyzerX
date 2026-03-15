#pragma once

#include <QFile>
#include <QMutex>
#include <QTextStream>
#include <QStandardPaths>
#include <QDateTime>
#include <memory>
#include <ostream>

#include <vtkSmartPointer.h>

// Forward declarations for VTK
class vtkOutputWindow;
class LocalVTKOutputWindow;

class Logger
{
public:
	// Install the logger (call after QApplication construction)
	static void install();

	// Optional: uninstall (restores stderr and vtk output window)
	static void uninstall();

	// Write a line to the log (thread-safe)
	static void writeLine(const QString& line);

	LocalVTKOutputWindow* GetVTKOutputWindow() const;

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

	// rotation helpers (size-based rotation happens from writeInternal)
	void rotateLogsIfNeeded();
	void rotateLogs();

	QFile m_file;
	QTextStream m_stream;
	QMutex m_mutex;

	// rotation config (static runtime-configurable)
	static bool s_rotateEnabled;
	static int s_maxBackupFiles;
	static int s_maxFileSizeMB;

	// redirect std::cerr
	std::unique_ptr<std::ofstream> m_ofs;
	std::streambuf* m_oldCerr = nullptr;

	vtkSmartPointer<LocalVTKOutputWindow> m_vtkOutputWindow;

	static Logger* s_instance;
};

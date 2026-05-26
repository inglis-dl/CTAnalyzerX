#pragma once

#include <vtkOutputWindow.h>
#include <QDebug>
#include <QString>

/// Redirects all VTK text output to Qt's logging system.
/// Install via vtkOutputWindow::SetInstance() before any VTK objects are created.
class VtkQtOutputWindow : public vtkOutputWindow
{
public:
	static VtkQtOutputWindow* New();
	vtkTypeMacro(VtkQtOutputWindow, vtkOutputWindow);

	void DisplayText(const char* msg) override
	{
		qDebug().noquote() << "[VTK]" << QString::fromUtf8(msg ? msg : "").trimmed();
	}

	void DisplayErrorText(const char* msg) override
	{
		qCritical().noquote() << "[VTK ERROR]" << QString::fromUtf8(msg ? msg : "").trimmed();
	}

	void DisplayWarningText(const char* msg) override
	{
		qWarning().noquote() << "[VTK WARN]" << QString::fromUtf8(msg ? msg : "").trimmed();
	}

	void DisplayGenericWarningText(const char* msg) override
	{
		qWarning().noquote() << "[VTK]" << QString::fromUtf8(msg ? msg : "").trimmed();
	}

	void DisplayDebugText(const char* msg) override
	{
		qDebug().noquote() << "[VTK DBG]" << QString::fromUtf8(msg ? msg : "").trimmed();
	}

protected:
	VtkQtOutputWindow() = default;
	~VtkQtOutputWindow() override = default;

private:
	VtkQtOutputWindow(const VtkQtOutputWindow&) = delete;
	void operator=(const VtkQtOutputWindow&) = delete;
};

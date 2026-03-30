#ifndef Q_VTK_OUTPUT_WINDOW_H
#define Q_VTK_OUTPUT_WINDOW_H

#include <vtkOutputWindow.h>
#include <QDebug>

/// Redirects all VTK text output to Qt's logging system.
/// Install via vtkOutputWindow::SetInstance() before any VTK objects are created.
class VtkQtOutputWindow : public vtkOutputWindow
{
public:
    static VtkQtOutputWindow* New();
    vtkTypeMacro(VtkQtOutputWindow, vtkOutputWindow);

    void DisplayText(const char* msg) override
    {
        qDebug().noquote() << "[VTK]" << msg;
    }

    void DisplayErrorText(const char* msg) override
    {
        qCritical().noquote() << "[VTK ERROR]" << msg;
    }

    void DisplayWarningText(const char* msg) override
    {
        qWarning().noquote() << "[VTK WARN]" << msg;
    }

    void DisplayGenericWarningText(const char* msg) override
    {
        qWarning().noquote() << "[VTK]" << msg;
    }

    void DisplayDebugText(const char* msg) override
    {
        qDebug().noquote() << "[VTK DBG]" << msg;
    }

protected:
    VtkQtOutputWindow() = default;
    ~VtkQtOutputWindow() override = default;

private:
    VtkQtOutputWindow(const VtkQtOutputWindow&) = delete;
    void operator=(const VtkQtOutputWindow&) = delete;
};

#endif // Q_VTK_OUTPUT_WINDOW_H
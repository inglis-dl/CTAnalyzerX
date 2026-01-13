#pragma once

#include "ui_WindowLevelController.h"

#include <vtkSmartPointer.h>

class vtkImageData;
class ScalarOpacityFunctionWidget;

class WindowLevelController : public QWidget
{
	Q_OBJECT

public:
	explicit WindowLevelController(QWidget* parent = nullptr);

	ScalarOpacityFunctionWidget* scalarOpacityFunctionWidget() const;

	// Map between UI and external callers
public slots:
	void setWindow(double w);
	void setLevel(double l);

	// Adjust debounce interval used for interactive emissions (ms)
	void setDebounceInterval(int ms);

	// Accept image data (kept for future histogram/analysis; currently stored only)
	void setImageData(vtkImageData* image);

	// Persist/load settings to the application JSON settings file
	void writeSettings();
	void readSettings();

signals:
	// interactive (fires while user adjusts when InteractiveApply is enabled)
	void windowLevelChanged(double window, double level);
	// committed (user finished editing / pressed Enter / clicked apply)
	void windowLevelCommitted(double window, double level);
	// request to reset window/level to baseline across views
	void requestResetWindowLevel();

private:
	Ui::WindowLevelController ui;
	QTimer* m_debounce = nullptr;

	// store the most-recent image (kept for future histogram use)
	vtkSmartPointer<vtkImageData> m_image = nullptr;
};

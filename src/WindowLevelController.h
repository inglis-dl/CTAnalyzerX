#pragma once
#include <QWidget>

#include "ui_WindowLevelController.h"
#include <vtkSmartPointer.h>

class QTimer;
class vtkImageData;
class vtkImageHistogram;
class QMenu;
class QActionGroup;
class QAction;

class WindowLevelController : public QWidget
{
	Q_OBJECT
		Q_PROPERTY(int histogramScale READ histogramScale WRITE setHistogramScale NOTIFY histogramScaleChanged)

public:
	explicit WindowLevelController(QWidget* parent = nullptr);

public Q_SLOTS:
	// Set UI values (can be connected to view signals)
	void setWindow(double w);
	void setLevel(double l);

	// Adjust debounce interval used for interactive emissions (ms)
	void setDebounceInterval(int ms);

	void setImageData(vtkImageData* image);

	// Persist/load settings to the application JSON settings file
	void writeSettings();
	void readSettings();

	// Histogram scale accessor
	int histogramScale() const;
	void setHistogramScale(int s);

Q_SIGNALS:
	// interactive (fires while user adjusts when InteractiveApply is enabled)
	void windowLevelChanged(double window, double level);
	// committed (user finished editing / pressed Enter / clicked apply)
	void windowLevelCommitted(double window, double level);
	// request to reset window/level to baseline across views
	void requestResetWindowLevel();

	void histogramScaleChanged(int newScale);

private:
	Ui::WindowLevelController ui;
	QTimer* m_debounce = nullptr;

	vtkSmartPointer<vtkImageHistogram> m_histo;
	vtkSmartPointer<vtkImageData> m_lastImage;

	// context menu for the histogram view
	QMenu* m_viewMenu = nullptr;
	QActionGroup* m_viewMenuGroup = nullptr;
};

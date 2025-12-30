#pragma once

#include <QWidget>
#include <QTimer>
#include <QPainterPath>

#include "ui_WindowLevelController.h"
#include <vtkSmartPointer.h>

QT_FORWARD_DECLARE_CLASS(QMenu)
QT_FORWARD_DECLARE_CLASS(QActionGroup)
QT_FORWARD_DECLARE_CLASS(QAction)

namespace QtCharts {
	class QChartView;
	class QChart;
	class QBarSeries;
	class QBarSet;
	class QValueAxis;
}

class vtkImageData;
class vtkImageHistogram;

class WindowLevelController : public QWidget
{
	Q_OBJECT
		Q_PROPERTY(int histogramScale READ histogramScale WRITE setHistogramScale NOTIFY histogramScaleChanged)
		Q_PROPERTY(bool filterPeak READ filterPeak WRITE setFilterPeak)

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

	// Filter peak accessor
	bool filterPeak() const;
	void setFilterPeak(bool v);

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

	// Qt Charts members (replace QGraphics path approach)
	QtCharts::QChartView* m_chartView = nullptr;
	QtCharts::QChart* m_chart = nullptr;
	QtCharts::QBarSeries* m_barSeries = nullptr;
	QtCharts::QBarSet* m_barSet = nullptr;
	QtCharts::QValueAxis* m_axisX = nullptr;
	QtCharts::QValueAxis* m_axisY = nullptr;

	// context menu for the histogram view
	QMenu* m_viewMenu = nullptr;
	QActionGroup* m_viewMenuGroup = nullptr;
	// action for the new "Filter peak" toggle
	QAction* m_actFilterPeak = nullptr;
	// store the filter state
	bool m_filterPeak = false;

	// Draw/redraw histogram using current m_histo input + scale
	void redrawHistogram();

	// Ensure plot area fills view (implemented in cpp)
	void adjustChartPlotArea();

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
};

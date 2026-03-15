#pragma once

#include <QWidget>
#include "ui_ScalarOpacityFunctionWidget.h"
#include <vtkSmartPointer.h>

QT_FORWARD_DECLARE_CLASS(QGraphicsEllipseItem)
QT_FORWARD_DECLARE_CLASS(QGraphicsPathItem)
class vtkPiecewiseFunction;
class vtkImageData;
class vtkObject; // forward-declare so slots using vtkObject* compile
class ScalarOpacityFunctionWidgetPrivate;

class ScalarOpacityFunctionWidget : public QWidget
{
	Q_OBJECT
		// Threshold indicator properties (persisted in settings.json)
		Q_PROPERTY(bool showThresholdIndicator READ showThresholdIndicator WRITE setShowThresholdIndicator NOTIFY showThresholdIndicatorChanged)
		Q_PROPERTY(QString thresholdIndicatorColor READ thresholdIndicatorColor WRITE setThresholdIndicatorColor NOTIFY thresholdIndicatorColorChanged)
		Q_PROPERTY(double histogramThreshold READ histogramThreshold WRITE setHistogramThreshold NOTIFY histogramThresholdChanged)


public:
	explicit ScalarOpacityFunctionWidget(QWidget* parent = nullptr);
	~ScalarOpacityFunctionWidget();

	// Attach the vtkPiecewiseFunction that represents the "master" scalar opacity.
	// The widget will create and maintain an internal "slave" vtkPiecewiseFunction that it displays and edits.
	void setFunction(vtkPiecewiseFunction* func);

	// Provide concrete image data so the widget can compute and draw a histogram.
	// Ownership: caller retains ownership; widget will shallow-copy/inspect.
	void setImageData(vtkImageData* image);

	// Histogram controls (exposed so other code can update programmatically).
	int histogramScale() const;
	void setHistogramScale(int s);
	bool filterPeak() const;
	void setFilterPeak(bool v);

	// Threshold indicator API
	bool showThresholdIndicator() const;
	void setShowThresholdIndicator(bool v);

	QString thresholdIndicatorColor() const;
	void setThresholdIndicatorColor(const QString& color);

	double histogramThreshold() const;
	void setHistogramThreshold(double t);

	// Load/save widget-specific settings (called by parent controllers)
	void readSettings();
	void writeSettings();

	// Owned slave vtkPiecewiseFunction used by the widget for display/edits (may be null)
	vtkSmartPointer<vtkPiecewiseFunction> m_function;

public slots:
	// Force a visual refresh from the current function state.
	void updateFunction();

	// Set the scene X range (world coordinates). Scene Y remains [0..1].
	// This lets external controllers set the world X bounds (e.g. using image scalar range).
	void setSceneXRange(double xmin, double xmax);

signals:
	void functionChanged();
	void showThresholdIndicatorChanged(bool);
	void thresholdIndicatorColorChanged(const QString&);
	void histogramThresholdChanged(double);

protected:
	// UI produced by uic
	Ui::ScalarOpacityFunctionWidget ui;

	// Keep track of child widget resize events so embedded chart can follow viewport size.
	bool eventFilter(QObject* watched, QEvent* event) override;

	// PIMPL
	QScopedPointer<ScalarOpacityFunctionWidgetPrivate> d_ptr;

private:
	Q_DECLARE_PRIVATE(ScalarOpacityFunctionWidget)
		Q_DISABLE_COPY(ScalarOpacityFunctionWidget)
};

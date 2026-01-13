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

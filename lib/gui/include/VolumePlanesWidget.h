#pragma once

#include "ui_VolumePlanesWidget.h"

#include <QWidget>

class VolumePlanesWidget : public QWidget
{
	Q_OBJECT
public:
	explicit VolumePlanesWidget(QWidget* parent = nullptr);
	~VolumePlanesWidget() override;

	// Accessors for the range sliders
	RangeSlider* xRangeSlider() const { return ui.xRangeSlider; }
	RangeSlider* yRangeSlider() const { return ui.yRangeSlider; }
	RangeSlider* zRangeSlider() const { return ui.zRangeSlider; }

	// Accessors for min/max labels
	QLabel* xMinLabel() const { return ui.xMinLabel; }
	QLabel* xMaxLabel() const { return ui.xMaxLabel; }
	QLabel* yMinLabel() const { return ui.yMinLabel; }
	QLabel* yMaxLabel() const { return ui.yMaxLabel; }
	QLabel* zMinLabel() const { return ui.zMinLabel; }
	QLabel* zMaxLabel() const { return ui.zMaxLabel; }

	void setSaveEnabled(bool on);

public slots:
	// Configure the sliders' ranges and labels (x,y,z order to match UI)
	void setRangeSliders(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);

	// Called by external owners (e.g., MainWindow / state machine) to enable/disable crop UI
	// when cropping is toggled externally (keeps define button in sync)
	void onExternalCroppingChanged(bool enabled);

signals:
	// emit full ranges for each axis (min, max) in order X, Y, Z
	void croppingRegionChanged(int xMin, int xMax,
							  int yMin, int yMax,
							  int zMin, int zMax);

	// request views toggle outline visibility (connected to SliceView::setOutlineVisible)
	void requestOutlineVisibility(bool visible);

	// emitted when user presses Reset (sliders return to full extents)
	void resetCropRequested();

private slots:
	void on_resetButton_clicked();

	// Follow RangeSlider usage: valuesChanged(int min, int max)
	void on_xRangeSlider_valuesChanged(int min, int max);
	void on_yRangeSlider_valuesChanged(int min, int max);
	void on_zRangeSlider_valuesChanged(int min, int max);

private:
	Ui::VolumePlanesWidget ui;
	void setSiblingControlsEnabled(bool on);
	void updateLabels();
};
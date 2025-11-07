#pragma once

#include <QFrame>

#include "ui_VolumeControlsWidget.h"

class VolumeControlsWidget : public QFrame
{
	Q_OBJECT
public:
	explicit VolumeControlsWidget(QWidget* parent = nullptr);

	// Accessors for the range sliders
	RangeSlider* YZViewRangeSlider() const { return ui.YZViewRangeSlider; }
	RangeSlider* XZViewRangeSlider() const { return ui.XZViewRangeSlider; }
	RangeSlider* XYViewRangeSlider() const { return ui.XYViewRangeSlider; }

	// Accessors for min/max labels
	QLabel* YZViewMinLabel() const { return ui.YZViewMinLabel; }
	QLabel* YZViewMaxLabel() const { return ui.YZViewMaxLabel; }
	QLabel* XZViewMinLabel() const { return ui.XZViewMinLabel; }
	QLabel* XZViewMaxLabel() const { return ui.XZViewMaxLabel; }
	QLabel* XYViewMinLabel() const { return ui.XYViewMinLabel; }
	QLabel* XYViewMaxLabel() const { return ui.XYViewMaxLabel; }

	// Accessors for other controls
	QCheckBox* slicePlaneCheckBox() const { return ui.slicePlaneCheckBox; }
	QPushButton* resetButton() const { return ui.btnReset; }

public slots:
	void setRangeSliders(int yzMin, int yzMax, int xzMin, int xzMax, int xyMin, int xyMax);
	// Called from external owner (VolumeView/MainWindow) to synchronize cropping enabled state.
	void onExternalCroppingChanged(bool enabled);

signals:
	void croppingRegionChanged(int yzMin, int yzMax,
							  int xzMin, int xzMax,
							  int xyMin, int xyMax);
	void slicePlaneToggle(bool visible);

private slots:
	void updateYZLabel(int min, int max);
	void updateXZLabel(int min, int max);
	void updateXYLabel(int min, int max);
	// Enable/disable cropping controls when the cropping checkbox toggles
	void onCroppingToggled(bool checked);

public:
	// Insert an external WindowLevel controller into the dedicated group box.
	// Controller is NOT owned by VolumeControlsWidget (it will have a parent set here).
	// This method adjusts size policies so the groupbox fits the controller's fixed height.
	void insertWindowLevelController(QWidget* controller);

private:
	Ui::VolumeControlsWidget ui;
};

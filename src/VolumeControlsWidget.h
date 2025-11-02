#ifndef VOLUMECONTROLSWIDGET_H
#define VOLUMECONTROLSWIDGET_H

#include <QFrame>
#include <QCheckBox>
#include <QPushButton>
#include "ui_VolumeControlsWidget.h"

class RangeSlider;

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
	QPushButton* presetReset() const { return ui.presetReset; }

public slots:
	void setRangeSliders(int yzMin, int yzMax, int xzMin, int xzMax, int xyMin, int xyMax);

signals:
	void croppingRegionChanged(int yzMin, int yzMax,
							  int xzMin, int xzMax,
							  int xyMin, int xyMax);
	void slicePlaneToggle(bool visible);

private slots:
	void updateYZLabel(int min, int max);
	void updateXZLabel(int min, int max);
	void updateXYLabel(int min, int max);

private:
	Ui::VolumeControlsWidget ui;
};

#endif // VOLUMECONTROLSWIDGET_H

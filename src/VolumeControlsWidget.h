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

	// Add public methods/signals as needed for interaction
	// e.g. getters for sliders, signals for preset buttons, etc.

	// Accessors for the range sliders
	RangeSlider* axialRangeSlider() const { return ui.axialRangeSlider; }
	RangeSlider* sagitalRangeSlider() const { return ui.saggitalRangeSlider; }
	RangeSlider* coronalRangeSlider() const { return ui.coronalRangeSlider; }

	// Accessors for other controls if needed
	QCheckBox* slicePlaneCheclBox() const { return ui.slicePlaneCheckBox; }
	QPushButton* presetFullVolume() const { return ui.presetFullVolume; }
	QPushButton* presetCenterROI() const { return ui.presetCenterROI; }
	QPushButton* presetReset() const { return ui.presetReset; }

	// Set up the range sliders for a new image
public slots:
	void setRangeSliders(int axialMin, int axialMax, int sagittalMin, int sagittalMax, int coronalMin, int coronalMax);

signals:
	void croppingRegionChanged(int axialMin, int axialMax,
							  int sagittalMin, int sagittalMax,
							  int coronalMin, int coronalMax);
	void slicePlaneToggle(bool visible);

private slots:
	void updateAxialLabel(int min, int max);
	void updateSaggitalLabel(int min, int max);
	void updateCoronalLabel(int min, int max);

private:
	Ui::VolumeControlsWidget ui;
};

#endif // VOLUMECONTROLSWIDGET_H

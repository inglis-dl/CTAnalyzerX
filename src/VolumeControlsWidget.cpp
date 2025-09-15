#include "VolumeControlsWidget.h"
#include <QDebug>

VolumeControlsWidget::VolumeControlsWidget(QWidget* parent)
	: QFrame(parent)
{
	ui.setupUi(this);

	// Connect range sliders to emit croppingRegionChanged
	auto emitCropping = [this]() {
		emit croppingRegionChanged(
			ui.axialRangeSlider->minimumValue(), ui.axialRangeSlider->maximumValue(),
			ui.saggitalRangeSlider->minimumValue(), ui.saggitalRangeSlider->maximumValue(),
			ui.coronalRangeSlider->minimumValue(), ui.coronalRangeSlider->maximumValue()
		);
		};

	connect(ui.axialRangeSlider, &RangeSlider::valuesChanged, this, [this, emitCropping](int min, int max) {
		updateAxialLabel(min, max);
		emitCropping();
	});
	connect(ui.saggitalRangeSlider, &RangeSlider::valuesChanged, this, [this, emitCropping](int min, int max) {
		updateSaggitalLabel(min, max);
		emitCropping();
	});
	connect(ui.coronalRangeSlider, &RangeSlider::valuesChanged, this, [this, emitCropping](int min, int max) {
		updateCoronalLabel(min, max);
		emitCropping();
	});

	// Also update labels when the range changes (e.g., after setRangeSliders)
	connect(ui.axialRangeSlider, &RangeSlider::rangeChanged, this, [this](int min, int max) {
		updateAxialLabel(min, max);
	});
	connect(ui.saggitalRangeSlider, &RangeSlider::rangeChanged, this, [this](int min, int max) {
		updateSaggitalLabel(min, max);
	});
	connect(ui.coronalRangeSlider, &RangeSlider::rangeChanged, this, [this](int min, int max) {
		updateCoronalLabel(min, max);
	});

	// Preset buttons (unchanged)
	connect(ui.presetFullVolume, &QPushButton::clicked, this, [this, emitCropping]() {
		ui.axialRangeSlider->setValues(ui.axialRangeSlider->minimum(), ui.axialRangeSlider->maximum());
		ui.saggitalRangeSlider->setValues(ui.saggitalRangeSlider->minimum(), ui.saggitalRangeSlider->maximum());
		ui.coronalRangeSlider->setValues(ui.coronalRangeSlider->minimum(), ui.coronalRangeSlider->maximum());
		emitCropping();
	});
	connect(ui.presetCenterROI, &QPushButton::clicked, this, [this, emitCropping]() {
		// Example: center 40% ROI
		int aMin = ui.axialRangeSlider->minimum();
		int aMax = ui.axialRangeSlider->maximum();
		int sMin = ui.saggitalRangeSlider->minimum();
		int sMax = ui.saggitalRangeSlider->maximum();
		int cMin = ui.coronalRangeSlider->minimum();
		int cMax = ui.coronalRangeSlider->maximum();
		int aMid = (aMin + aMax) / 2;
		int sMid = (sMin + sMax) / 2;
		int cMid = (cMin + cMax) / 2;
		int aRange = (aMax - aMin) / 5;
		int sRange = (sMax - sMin) / 5;
		int cRange = (cMax - cMin) / 5;
		ui.axialRangeSlider->setValues(aMid - aRange, aMid + aRange);
		ui.saggitalRangeSlider->setValues(sMid - sRange, sMid + sRange);
		ui.coronalRangeSlider->setValues(cMid - cRange, cMid + cRange);
		emitCropping();
	});
	connect(ui.presetReset, &QPushButton::clicked, this, [this, emitCropping]() {
		ui.axialRangeSlider->setValues(ui.axialRangeSlider->minimum(), ui.axialRangeSlider->maximum());
		ui.saggitalRangeSlider->setValues(ui.saggitalRangeSlider->minimum(), ui.saggitalRangeSlider->maximum());
		ui.coronalRangeSlider->setValues(ui.coronalRangeSlider->minimum(), ui.coronalRangeSlider->maximum());
		emitCropping();
	});

	connect(ui.slicePlaneCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
		emit slicePlaneToggle(checked);
	});
}

void VolumeControlsWidget::setRangeSliders(int axialMin, int axialMax, int sagittalMin, int sagittalMax, int coronalMin, int coronalMax)
{
	ui.axialRangeSlider->setMinimum(axialMin);
	ui.axialRangeSlider->setMaximum(axialMax);
	ui.axialRangeSlider->setValues(axialMin, axialMax);
	updateAxialLabel(axialMin, axialMax);

	ui.saggitalRangeSlider->setMinimum(sagittalMin);
	ui.saggitalRangeSlider->setMaximum(sagittalMax);
	ui.saggitalRangeSlider->setValues(sagittalMin, sagittalMax);
	updateSaggitalLabel(sagittalMin, sagittalMax);

	ui.coronalRangeSlider->setMinimum(coronalMin);
	ui.coronalRangeSlider->setMaximum(coronalMax);
	ui.coronalRangeSlider->setValues(coronalMin, coronalMax);
	updateCoronalLabel(coronalMin, coronalMax);
}

void VolumeControlsWidget::updateAxialLabel(int min, int max)
{
	ui.axialMinLabel->setText(QString("%1").arg(min));
	ui.axialMaxLabel->setText(QString("%1").arg(max));
}

void VolumeControlsWidget::updateSaggitalLabel(int min, int max)
{
	ui.saggitalMinLabel->setText(QString("%1").arg(min));
	ui.saggitalMaxLabel->setText(QString("%1").arg(max));
}

void VolumeControlsWidget::updateCoronalLabel(int min, int max)
{
	ui.coronalMinLabel->setText(QString("%1").arg(min));
	ui.coronalMaxLabel->setText(QString("%1").arg(max));
}
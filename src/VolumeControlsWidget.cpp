#include "VolumeControlsWidget.h"

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

	connect(ui.axialRangeSlider, &RangeSlider::valuesChanged, this, [emitCropping](int, int) { emitCropping(); });
	connect(ui.saggitalRangeSlider, &RangeSlider::valuesChanged, this, [emitCropping](int, int) { emitCropping(); });
	connect(ui.coronalRangeSlider, &RangeSlider::valuesChanged, this, [emitCropping](int, int) { emitCropping(); });

	// Preset buttons
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

	// Example: connect slice plane toggle
	connect(ui.slicePlaneCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
		emit slicePlaneToggle(checked);
	});
}

void VolumeControlsWidget::setRangeSliders(int axialMin, int axialMax, int sagittalMin, int sagittalMax, int coronalMin, int coronalMax)
{
	ui.axialRangeSlider->setMinimum(axialMin);
	ui.axialRangeSlider->setMaximum(axialMax);
	ui.axialRangeSlider->setValues(axialMin, axialMax);

	ui.saggitalRangeSlider->setMinimum(sagittalMin);
	ui.saggitalRangeSlider->setMaximum(sagittalMax);
	ui.saggitalRangeSlider->setValues(sagittalMin, sagittalMax);

	ui.coronalRangeSlider->setMinimum(coronalMin);
	ui.coronalRangeSlider->setMaximum(coronalMax);
	ui.coronalRangeSlider->setValues(coronalMin, coronalMax);
}
#include "VolumeControlsWidget.h"
#include <QDebug>

VolumeControlsWidget::VolumeControlsWidget(QWidget* parent)
	: QFrame(parent)
{
	ui.setupUi(this);

	ui.YZViewRangeSlider->setOrientation(Qt::Horizontal);
	ui.XZViewRangeSlider->setOrientation(Qt::Horizontal);
	ui.XYViewRangeSlider->setOrientation(Qt::Horizontal);

	// Connect range sliders to emit croppingRegionChanged
	auto emitCropping = [this]() {
		emit croppingRegionChanged(
			ui.YZViewRangeSlider->minimumValue(), ui.YZViewRangeSlider->maximumValue(),
			ui.XZViewRangeSlider->minimumValue(), ui.XZViewRangeSlider->maximumValue(),
			ui.XYViewRangeSlider->minimumValue(), ui.XYViewRangeSlider->maximumValue()
		);
		};

	connect(ui.YZViewRangeSlider, &RangeSlider::valuesChanged, this, [this, emitCropping](int min, int max) {
		updateYZLabel(min, max);
		emitCropping();
	});
	connect(ui.XZViewRangeSlider, &RangeSlider::valuesChanged, this, [this, emitCropping](int min, int max) {
		updateXZLabel(min, max);
		emitCropping();
	});
	connect(ui.XYViewRangeSlider, &RangeSlider::valuesChanged, this, [this, emitCropping](int min, int max) {
		updateXYLabel(min, max);
		emitCropping();
	});

	// Also update labels when the range changes (e.g., after setRangeSliders)
	connect(ui.YZViewRangeSlider, &RangeSlider::rangeChanged, this, [this](int min, int max) {
		updateYZLabel(min, max);
	});
	connect(ui.XZViewRangeSlider, &RangeSlider::rangeChanged, this, [this](int min, int max) {
		updateXZLabel(min, max);
	});
	connect(ui.XYViewRangeSlider, &RangeSlider::rangeChanged, this, [this](int min, int max) {
		updateXYLabel(min, max);
	});

	connect(ui.presetReset, &QPushButton::clicked, this, [this, emitCropping]() {
		ui.YZViewRangeSlider->setValues(ui.YZViewRangeSlider->minimum(), ui.YZViewRangeSlider->maximum());
		ui.XZViewRangeSlider->setValues(ui.XZViewRangeSlider->minimum(), ui.XZViewRangeSlider->maximum());
		ui.XYViewRangeSlider->setValues(ui.XYViewRangeSlider->minimum(), ui.XYViewRangeSlider->maximum());
		emitCropping();
	});

	connect(ui.slicePlaneCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
		emit slicePlaneToggle(checked);
	});
}

void VolumeControlsWidget::setRangeSliders(int yzMin, int yzMax, int xzMin, int xzMax, int xyMin, int xyMax)
{
	ui.YZViewRangeSlider->setMinimum(yzMin);
	ui.YZViewRangeSlider->setMaximum(yzMax);
	ui.YZViewRangeSlider->setValues(yzMin, yzMax);
	updateYZLabel(yzMin, yzMax);

	ui.XZViewRangeSlider->setMinimum(xzMin);
	ui.XZViewRangeSlider->setMaximum(xzMax);
	ui.XZViewRangeSlider->setValues(xzMin, xzMax);
	updateXZLabel(xzMin, xzMax);

	ui.XYViewRangeSlider->setMinimum(xyMin);
	ui.XYViewRangeSlider->setMaximum(xyMax);
	ui.XYViewRangeSlider->setValues(xyMin, xyMax);
	updateXYLabel(xyMin, xyMax);
}

void VolumeControlsWidget::updateYZLabel(int min, int max)
{
	ui.YZViewMinLabel->setText(QString("%1").arg(min));
	ui.YZViewMaxLabel->setText(QString("%1").arg(max));
}

void VolumeControlsWidget::updateXZLabel(int min, int max)
{
	ui.XZViewMinLabel->setText(QString("%1").arg(min));
	ui.XZViewMaxLabel->setText(QString("%1").arg(max));
}

void VolumeControlsWidget::updateXYLabel(int min, int max)
{
	ui.XYViewMinLabel->setText(QString("%1").arg(min));
	ui.XYViewMaxLabel->setText(QString("%1").arg(max));
}
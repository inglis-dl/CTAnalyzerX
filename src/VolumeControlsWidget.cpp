#include "VolumeControlsWidget.h"
#include <QDebug>
#include <QSizePolicy>
#include <QLayout>
#include <algorithm>

VolumeControlsWidget::VolumeControlsWidget(QWidget* parent)
	: QFrame(parent)
{
	ui.setupUi(this);

	ui.YZViewRangeSlider->setOrientation(Qt::Horizontal);
	ui.XZViewRangeSlider->setOrientation(Qt::Horizontal);
	ui.XYViewRangeSlider->setOrientation(Qt::Horizontal);

	// Connect range sliders to emit croppingRegionChanged
	// Emit only when cropping is actually enabled.
	auto emitCropping = [this]() {
		if (!ui.croppingCheckBox || !ui.croppingCheckBox->isChecked()) {
			return;
		}
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

	connect(ui.btnReset, &QPushButton::clicked, this, [this, emitCropping]() {
		ui.YZViewRangeSlider->setValues(ui.YZViewRangeSlider->minimum(), ui.YZViewRangeSlider->maximum());
		ui.XZViewRangeSlider->setValues(ui.XZViewRangeSlider->minimum(), ui.XZViewRangeSlider->maximum());
		ui.XYViewRangeSlider->setValues(ui.XYViewRangeSlider->minimum(), ui.XYViewRangeSlider->maximum());
		emitCropping();
	});

	connect(ui.slicePlaneCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
		emit slicePlaneToggle(checked);
	});

	// wire cropping checkbox -> enable/disable controls
	connect(ui.croppingCheckBox, &QCheckBox::toggled,
			this, &VolumeControlsWidget::onCroppingToggled);

	// initialize controls to reflect the current checkbox state
	onCroppingToggled(ui.croppingCheckBox->isChecked());
}

void VolumeControlsWidget::setRangeSliders(int yzMin, int yzMax, int xzMin, int xzMax, int xyMin, int xyMax)
{
	// Prevent intermediate slider signals from emitting cropping updates while we initialize.
	bool yzBlocked = ui.YZViewRangeSlider->blockSignals(true);
	bool xzBlocked = ui.XZViewRangeSlider->blockSignals(true);
	bool xyBlocked = ui.XYViewRangeSlider->blockSignals(true);

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

	// restore previous blockSignals states
	ui.YZViewRangeSlider->blockSignals(yzBlocked);
	ui.XZViewRangeSlider->blockSignals(xzBlocked);
	ui.XYViewRangeSlider->blockSignals(xyBlocked);

	// Emit a single croppingRegionChanged only if cropping is enabled.
	if (ui.croppingCheckBox && ui.croppingCheckBox->isChecked()) {
		emit croppingRegionChanged(
			ui.YZViewRangeSlider->minimumValue(), ui.YZViewRangeSlider->maximumValue(),
			ui.XZViewRangeSlider->minimumValue(), ui.XZViewRangeSlider->maximumValue(),
			ui.XYViewRangeSlider->minimumValue(), ui.XYViewRangeSlider->maximumValue()
		);
	}
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

void VolumeControlsWidget::insertWindowLevelController(QWidget* controller)
{
	if (!controller) return;

	// Prefer to add the controller into the dedicated group box layout (created by the UI)
	QWidget* group = ui.groupBoxWindowLevel;
	QLayout* layout = nullptr;
	if (group)
		layout = group->layout();

	// Ensure controller has a reasonable vertical size policy so group box can adopt its height
	// If the controller advertises a sizeHint, use that height as fixed height so group box fits tightly.
	QSize hint = controller->sizeHint();
	if (hint.height() > 0) {
		// Make controller a fixed-height widget to avoid unwanted vertical stretching
		controller->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		controller->setFixedHeight(hint.height());
	}
	else {
		// Fall back to a sensible policy
		controller->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	}

	if (layout) {
		layout->addWidget(controller);
		controller->setParent(group);
	}
	else {
		// If no layout on the group for some reason, add directly to group's layout object name fallback
		// try to append to windowLevelLayout inside the generated UI object
		if (ui.windowLevelLayout) {
			ui.windowLevelLayout->addWidget(controller);
			controller->setParent(group);
		}
		else {
			// last resort: place under this widget's layout
			if (this->layout()) {
				this->layout()->addWidget(controller);
				controller->setParent(this);
			}
			else {
				// set parent but don't attempt to reparent into layout
				controller->setParent(group ? group : this);
			}
		}
	}

	// Make the group box adopt the fixed height of its contents
	if (group) {
		group->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		group->adjustSize();
	}

	// Update geometry so parent layouts recompute sizes
	this->updateGeometry();
	this->adjustSize();
}

void VolumeControlsWidget::onCroppingToggled(bool checked)
{
	// Buttons
	if (ui.btnCrop) ui.btnCrop->setEnabled(checked);
	if (ui.btnReset) ui.btnReset->setEnabled(checked);

	// Range sliders
	if (ui.YZViewRangeSlider) ui.YZViewRangeSlider->setEnabled(checked);
	if (ui.XZViewRangeSlider) ui.XZViewRangeSlider->setEnabled(checked);
	if (ui.XYViewRangeSlider) ui.XYViewRangeSlider->setEnabled(checked);
}

void VolumeControlsWidget::onExternalCroppingChanged(bool enabled)
{
	// Avoid feedback loops: block checkbox signals while we update it from the view
	if (!ui.croppingCheckBox) return;
	const bool prevBlocked = ui.croppingCheckBox->blockSignals(true);
	ui.croppingCheckBox->setChecked(enabled);
	ui.croppingCheckBox->blockSignals(prevBlocked);

	// Update dependent controls to match the new state
	onCroppingToggled(enabled);
}
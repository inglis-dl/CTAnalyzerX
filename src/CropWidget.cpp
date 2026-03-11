#include "CropWidget.h"

#include <QDebug>

CropWidget::CropWidget(QWidget* parent)
	: QWidget(parent)
{
	ui.setupUi(this);

	// initial state: define OFF, keep children disabled until user enables define
	setSiblingControlsEnabled(false);

	ui.xRangeSlider->setOrientation(Qt::Horizontal);
	ui.yRangeSlider->setOrientation(Qt::Horizontal);
	ui.zRangeSlider->setOrientation(Qt::Horizontal);

	// connect UI
	connect(ui.defineButton, &QPushButton::toggled, this, &CropWidget::on_defineButton_toggled);
	connect(ui.resetButton, &QPushButton::clicked, this, &CropWidget::on_resetButton_clicked);
	connect(ui.saveButton, &QPushButton::clicked, this, &CropWidget::on_saveButton_clicked);

	// RangeSlider provides valuesChanged(min,max)
	connect(ui.xRangeSlider, &RangeSlider::valuesChanged, this, &CropWidget::on_xRangeSlider_valuesChanged);
	connect(ui.yRangeSlider, &RangeSlider::valuesChanged, this, &CropWidget::on_yRangeSlider_valuesChanged);
	connect(ui.zRangeSlider, &RangeSlider::valuesChanged, this, &CropWidget::on_zRangeSlider_valuesChanged);

	// sensible defaults for labels (in case caller doesn't call setRangeSliders)
	setRangeSliders(0, 1000, 0, 1000, 0, 1000);

	// Ensure Save is correctly initialized according to slider defaults
	// (it will be disabled because the initial region matches the full image)
	updateSaveButtonState();
}

CropWidget::~CropWidget()
{
}

void CropWidget::setSaveEnabled(bool on)
{
	ui.saveButton->setEnabled(on);
}

void CropWidget::setRangeSliders(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax)
{
	// Prevent intermediate signals while initializing
	bool xb = ui.xRangeSlider->blockSignals(true);
	bool yb = ui.yRangeSlider->blockSignals(true);
	bool zb = ui.zRangeSlider->blockSignals(true);

	// X
	ui.xRangeSlider->setMinimum(xMin);
	ui.xRangeSlider->setMaximum(xMax);
	ui.xRangeSlider->setValues(xMin, xMax);
	ui.xMinLabel->setText(QString::number(xMin));
	ui.xMaxLabel->setText(QString::number(xMax));

	// Y
	ui.yRangeSlider->setMinimum(yMin);
	ui.yRangeSlider->setMaximum(yMax);
	ui.yRangeSlider->setValues(yMin, yMax);
	ui.yMinLabel->setText(QString::number(yMin));
	ui.yMaxLabel->setText(QString::number(yMax));

	// Z
	ui.zRangeSlider->setMinimum(zMin);
	ui.zRangeSlider->setMaximum(zMax);
	ui.zRangeSlider->setValues(zMin, zMax);
	ui.zMinLabel->setText(QString::number(zMin));
	ui.zMaxLabel->setText(QString::number(zMax));

	updateLabels();

	// restore previous blockSignals states
	ui.xRangeSlider->blockSignals(xb);
	ui.yRangeSlider->blockSignals(yb);
	ui.zRangeSlider->blockSignals(zb);

	// Recompute Save-enabled state after establishing full ranges
	updateSaveButtonState();
}

void CropWidget::onExternalCroppingChanged(bool enabled)
{
	// Keep define button in sync with external state and enable/disable siblings.
	// Avoid re-emitting the same user-intent signal when programmatically changing the button.
	QSignalBlocker guard(ui.defineButton);
	ui.defineButton->setChecked(enabled);
	setSiblingControlsEnabled(enabled);

	// Keep outlines in sync with external cropping state
	emit requestOutlineVisibility(enabled);
}

void CropWidget::on_defineButton_toggled(bool checked)
{
	setSiblingControlsEnabled(checked);
	emit defineCropToggled(checked);

	// Let views show/hide outline actors when define mode changes.
	emit requestOutlineVisibility(checked);
}

void CropWidget::on_resetButton_clicked()
{
	// Reset sliders to full ranges (min, max)
	ui.xRangeSlider->setValues(ui.xRangeSlider->minimum(), ui.xRangeSlider->maximum());
	ui.yRangeSlider->setValues(ui.yRangeSlider->minimum(), ui.yRangeSlider->maximum());
	ui.zRangeSlider->setValues(ui.zRangeSlider->minimum(), ui.zRangeSlider->maximum());
	updateLabels();
	// emit a region change so consumers can respond (full extents)
	emit croppingRegionChanged(
		ui.xRangeSlider->minimumValue(), ui.xRangeSlider->maximumValue(),
		ui.yRangeSlider->minimumValue(), ui.yRangeSlider->maximumValue(),
		ui.zRangeSlider->minimumValue(), ui.zRangeSlider->maximumValue()
	);

	// Update Save button state after reset (now same as input, so Save should be disabled)
	updateSaveButtonState();

	// emit the explicit reset signal so consumers can respond (e.g., discard pending changes, update outline actors, etc.)
	emit resetCropRequested();
}

void CropWidget::on_saveButton_clicked()
{
	// Turn outlines off before committing the save so views won't request out-of-range data
	// while the new image is written/loaded.
	emit requestOutlineVisibility(false);

	// Save is the single commit action now.
	emit saveCroppedRequested();
}

void CropWidget::on_xRangeSlider_valuesChanged(int min, int max)
{
	ui.xMinLabel->setText(QString::number(min));
	ui.xMaxLabel->setText(QString::number(max));
	emit croppingRegionChanged(
		min, max,
		ui.yRangeSlider->minimumValue(), ui.yRangeSlider->maximumValue(),
		ui.zRangeSlider->minimumValue(), ui.zRangeSlider->maximumValue()
	);
	updateSaveButtonState();
}

void CropWidget::on_yRangeSlider_valuesChanged(int min, int max)
{
	ui.yMinLabel->setText(QString::number(min));
	ui.yMaxLabel->setText(QString::number(max));
	emit croppingRegionChanged(
		ui.xRangeSlider->minimumValue(), ui.xRangeSlider->maximumValue(),
		min, max,
		ui.zRangeSlider->minimumValue(), ui.zRangeSlider->maximumValue()
	);
	updateSaveButtonState();
}

void CropWidget::on_zRangeSlider_valuesChanged(int min, int max)
{
	ui.zMinLabel->setText(QString::number(min));
	ui.zMaxLabel->setText(QString::number(max));
	emit croppingRegionChanged(
		ui.xRangeSlider->minimumValue(), ui.xRangeSlider->maximumValue(),
		ui.yRangeSlider->minimumValue(), ui.yRangeSlider->maximumValue(),
		min, max
	);
	updateSaveButtonState();
}

void CropWidget::setSiblingControlsEnabled(bool on)
{
	ui.resetButton->setEnabled(on);
	// Save should only be enabled when defining AND when extents produce a valid, smaller-than-input volume.
	// updateSaveButtonState() will take care of that.
	updateSaveButtonState();

	ui.xRangeSlider->setEnabled(on);
	ui.yRangeSlider->setEnabled(on);
	ui.zRangeSlider->setEnabled(on);
}

void CropWidget::updateLabels()
{
	// Keep min/max labels reflecting current slider values.
	ui.xMinLabel->setText(QString::number(ui.xRangeSlider->minimumValue()));
	ui.yMinLabel->setText(QString::number(ui.yRangeSlider->minimumValue()));
	ui.zMinLabel->setText(QString::number(ui.zRangeSlider->minimumValue()));

	ui.xMaxLabel->setText(QString::number(ui.xRangeSlider->maximumValue()));
	ui.yMaxLabel->setText(QString::number(ui.yRangeSlider->maximumValue()));
	ui.zMaxLabel->setText(QString::number(ui.zRangeSlider->maximumValue()));
}

// enable Save only when:
// - Define mode is active (ui.defineButton checked),
// - the selected extents produce positive dimensions on each axis,
// - AND the selected extents are strictly smaller than the full image extents on at least one axis.
void CropWidget::updateSaveButtonState()
{
	// If no UI elements available bail out defensively.
	if (!ui.saveButton || !ui.xRangeSlider || !ui.yRangeSlider || !ui.zRangeSlider || !ui.defineButton) return;

	// Must be in define mode to allow save
	if (!ui.defineButton->isChecked()) {
		ui.saveButton->setEnabled(false);
		return;
	}

	int xmin = ui.xRangeSlider->minimumValue();
	int xmax = ui.xRangeSlider->maximumValue();
	int ymin = ui.yRangeSlider->minimumValue();
	int ymax = ui.yRangeSlider->maximumValue();
	int zmin = ui.zRangeSlider->minimumValue();
	int zmax = ui.zRangeSlider->maximumValue();

	// positive dimensions
	bool positive = (xmax - xmin + 1) > 0 && (ymax - ymin + 1) > 0 && (zmax - zmin + 1) > 0;

	// different from full extents?
	bool smaller = (xmin != ui.xRangeSlider->minimum()) || (xmax != ui.xRangeSlider->maximum()) ||
		(ymin != ui.yRangeSlider->minimum()) || (ymax != ui.yRangeSlider->maximum()) ||
		(zmin != ui.zRangeSlider->minimum()) || (zmax != ui.zRangeSlider->maximum());

	ui.saveButton->setEnabled(positive && smaller);
}
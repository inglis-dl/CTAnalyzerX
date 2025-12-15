#include "CropController.h"

#include <QDebug>

CropController::CropController(QWidget* parent)
	: QWidget(parent)
{
	ui.setupUi(this);

	// initial state: define OFF, keep children disabled until user enables define
	setSiblingControlsEnabled(false);

	ui.xRangeSlider->setOrientation(Qt::Horizontal);
	ui.yRangeSlider->setOrientation(Qt::Horizontal);
	ui.zRangeSlider->setOrientation(Qt::Horizontal);

	// connect UI
	connect(ui.defineButton, &QPushButton::toggled, this, &CropController::on_defineButton_toggled);
	connect(ui.applyButton, &QPushButton::clicked, this, &CropController::on_applyButton_clicked);
	connect(ui.resetButton, &QPushButton::clicked, this, &CropController::on_resetButton_clicked);
	connect(ui.saveButton, &QPushButton::clicked, this, &CropController::on_saveButton_clicked);

	// RangeSlider provides valuesChanged(min,max)
	connect(ui.xRangeSlider, &RangeSlider::valuesChanged, this, &CropController::on_xRangeSlider_valuesChanged);
	connect(ui.yRangeSlider, &RangeSlider::valuesChanged, this, &CropController::on_yRangeSlider_valuesChanged);
	connect(ui.zRangeSlider, &RangeSlider::valuesChanged, this, &CropController::on_zRangeSlider_valuesChanged);

	// sensible defaults for labels (in case caller doesn't call setRangeSliders)
	setRangeSliders(0, 1000, 0, 1000, 0, 1000);
}

CropController::~CropController()
{
}

void CropController::setRangeSliders(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax)
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
}

void CropController::onExternalCroppingChanged(bool enabled)
{
	// Keep define button in sync with external state and enable/disable siblings.
	// Avoid re-emitting the same user-intent signal when programmatically changing the button.
	QSignalBlocker guard(ui.defineButton);
	ui.defineButton->setChecked(enabled);
	setSiblingControlsEnabled(enabled);
}

void CropController::on_defineButton_toggled(bool checked)
{
	setSiblingControlsEnabled(checked);
	emit defineCropToggled(checked);
}

void CropController::on_applyButton_clicked()
{
	emit applyCropRequested();
}

void CropController::on_resetButton_clicked()
{
	// Reset sliders to full ranges (min, max) to match VolumeControlsWidget behavior
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
}

void CropController::on_saveButton_clicked()
{
	emit saveCroppedRequested();
}

void CropController::on_xRangeSlider_valuesChanged(int min, int max)
{
	ui.xMinLabel->setText(QString::number(min));
	ui.xMaxLabel->setText(QString::number(max));
	emit croppingRegionChanged(
		min, max,
		ui.yRangeSlider->minimumValue(), ui.yRangeSlider->maximumValue(),
		ui.zRangeSlider->minimumValue(), ui.zRangeSlider->maximumValue()
	);
}

void CropController::on_yRangeSlider_valuesChanged(int min, int max)
{
	ui.yMinLabel->setText(QString::number(min));
	ui.yMaxLabel->setText(QString::number(max));
	emit croppingRegionChanged(
		ui.xRangeSlider->minimumValue(), ui.xRangeSlider->maximumValue(),
		min, max,
		ui.zRangeSlider->minimumValue(), ui.zRangeSlider->maximumValue()
	);
}

void CropController::on_zRangeSlider_valuesChanged(int min, int max)
{
	ui.zMinLabel->setText(QString::number(min));
	ui.zMaxLabel->setText(QString::number(max));
	emit croppingRegionChanged(
		ui.xRangeSlider->minimumValue(), ui.xRangeSlider->maximumValue(),
		ui.yRangeSlider->minimumValue(), ui.yRangeSlider->maximumValue(),
		min, max
	);
}

void CropController::setSiblingControlsEnabled(bool on)
{
	// The Define button is allowed to remain enabled; everything else is toggled.
	// Keep Apply / Reset / Save buttons and sliders in disabled state when not defining.
	ui.applyButton->setEnabled(on);
	ui.resetButton->setEnabled(on);
	ui.saveButton->setEnabled(on);

	ui.xRangeSlider->setEnabled(on);
	ui.yRangeSlider->setEnabled(on);
	ui.zRangeSlider->setEnabled(on);
}

void CropController::updateLabels()
{
	// Keep min/max labels reflecting current slider values.
	ui.xMinLabel->setText(QString::number(ui.xRangeSlider->minimumValue()));
	ui.yMinLabel->setText(QString::number(ui.yRangeSlider->minimumValue()));
	ui.zMinLabel->setText(QString::number(ui.zRangeSlider->minimumValue()));

	ui.xMaxLabel->setText(QString::number(ui.xRangeSlider->maximumValue()));
	ui.yMaxLabel->setText(QString::number(ui.yRangeSlider->maximumValue()));
	ui.zMaxLabel->setText(QString::number(ui.zRangeSlider->maximumValue()));
}
#include "VolumeRotationWidget.h"
#include <QHBoxLayout>
#include <QFormLayout>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QComboBox>

#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkTransform.h>

VolumeRotationWidget::VolumeRotationWidget(QWidget* parent)
	: QWidget(parent)
{
	auto* form = new QFormLayout();

	m_yaw = new QDoubleSpinBox(this);
	m_yaw->setRange(-360.0, 360.0);
	m_yaw->setSingleStep(1.0);
	m_yaw->setDecimals(2);
	form->addRow(tr("Yaw (Z)"), m_yaw);

	m_pitch = new QDoubleSpinBox(this);
	m_pitch->setRange(-360.0, 360.0);
	m_pitch->setSingleStep(1.0);
	m_pitch->setDecimals(2);
	form->addRow(tr("Pitch (X)"), m_pitch);

	m_roll = new QDoubleSpinBox(this);
	m_roll->setRange(-360.0, 360.0);
	m_roll->setSingleStep(1.0);
	m_roll->setDecimals(2);
	form->addRow(tr("Roll (Y)"), m_roll);

	// Downsample combo + apply
	m_downsampleCombo = new QComboBox(this);
	m_downsampleCombo->addItem(tr("1x (none)"), 1);
	m_downsampleCombo->addItem(tr("2x"), 2);
	m_downsampleCombo->addItem(tr("4x"), 4);
	m_downsampleCombo->setCurrentIndex(0);
	form->addRow(tr("Downsample"), m_downsampleCombo);

	m_applyDownsample = new QPushButton(tr("Apply"), this);
	form->addRow(QString(), m_applyDownsample);

	m_live = new QCheckBox(tr("Live"), this);
	m_live->setChecked(true);

	m_reset = new QPushButton(tr("Reset"), this);

	auto* h = new QHBoxLayout();
	h->addLayout(form);
	h->addWidget(m_live);
	h->addWidget(m_reset);
	setLayout(h);

	connect(m_yaw, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &VolumeRotationWidget::onValueChanged);
	connect(m_pitch, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &VolumeRotationWidget::onValueChanged);
	connect(m_roll, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &VolumeRotationWidget::onValueChanged);

	connect(m_yaw, &QDoubleSpinBox::editingFinished, this, &VolumeRotationWidget::onEditingFinished);
	connect(m_pitch, &QDoubleSpinBox::editingFinished, this, &VolumeRotationWidget::onEditingFinished);
	connect(m_roll, &QDoubleSpinBox::editingFinished, this, &VolumeRotationWidget::onEditingFinished);

	connect(m_reset, &QPushButton::clicked, this, &VolumeRotationWidget::onResetClicked);
	connect(m_applyDownsample, &QPushButton::clicked, this, &VolumeRotationWidget::onApplyDownsampleClicked);

	// create internal reslice helper
	m_resliceHelper = std::make_unique<ImageResliceHelper>();
}

double VolumeRotationWidget::yaw() const { return m_yaw ? m_yaw->value() : 0.0; }
double VolumeRotationWidget::pitch() const { return m_pitch ? m_pitch->value() : 0.0; }
double VolumeRotationWidget::roll() const { return m_roll ? m_roll->value() : 0.0; }
bool VolumeRotationWidget::liveUpdate() const { return m_live ? m_live->isChecked() : true; }

int VolumeRotationWidget::downsampleFactor() const { return m_downsampleFactor; }

void VolumeRotationWidget::setDownsampleFactor(int factor)
{
	if (factor != 1 && factor != 2 && factor != 4) return;
	if (m_downsampleFactor == factor) return;
	m_downsampleFactor = factor;
	// Update combo selection to match
	if (m_downsampleCombo) {
		int idx = m_downsampleCombo->findData(factor);
		if (idx >= 0) m_downsampleCombo->setCurrentIndex(idx);
	}
	emit downsampleFactorChanged(m_downsampleFactor);
}

void VolumeRotationWidget::SetInputData(vtkImageData* img)
{
	if (!m_resliceHelper) m_resliceHelper = std::make_unique<ImageResliceHelper>();
	m_resliceHelper->SetInputData(img);
	// Apply current downsample factor to helper but don't force update until Apply pressed, unless live
	m_resliceHelper->SetDownsampleFactor(m_downsampleFactor);
	// Immediately produce an initial resliced image based on current rotation settings
	if (m_resliceHelper) {
		vtkTransform* t = m_resliceHelper->GetTransform();
		if (t) {
			t->Identity();
			t->RotateZ(yaw());
			t->RotateX(pitch());
			t->RotateY(roll());
		}
		if (m_live && m_live->isChecked()) {
			m_resliceHelper->Update();
			vtkImageData* outImg = vtkImageData::SafeDownCast(m_resliceHelper->GetReslice()->GetOutput());
			if (outImg) emit resliceReady(outImg);
			emit reslicePortReady(m_resliceHelper->GetOutputPort());
		}
	}
}

void VolumeRotationWidget::SetInputConnection(vtkAlgorithmOutput* port)
{
	if (!m_resliceHelper) m_resliceHelper = std::make_unique<ImageResliceHelper>();
	m_resliceHelper->SetInputConnection(port);
	m_resliceHelper->SetDownsampleFactor(m_downsampleFactor);
	// Immediately update and emit port (no image available in non-pipeline use)
	if (m_resliceHelper) {
		if (m_live && m_live->isChecked()) {
			m_resliceHelper->Update();
		}
		emit reslicePortReady(m_resliceHelper->GetOutputPort());
		vtkImageData* outImg = vtkImageData::SafeDownCast(m_resliceHelper->GetReslice()->GetOutput());
		if (outImg) emit resliceReady(outImg);
	}
}

void VolumeRotationWidget::onValueChanged(double)
{
	if (m_inProgrammaticUpdate) return;
	const bool finished = false;
	emit rotationChanged(yaw(), pitch(), roll(), finished);
	if (m_resliceHelper && m_live && m_live->isChecked()) {
		updateReslice(false);
	}
}

void VolumeRotationWidget::onEditingFinished()
{
	if (m_inProgrammaticUpdate) return;
	emit rotationChanged(yaw(), pitch(), roll(), true);
	if (m_resliceHelper) updateReslice(true);
}

void VolumeRotationWidget::onResetClicked()
{
	m_inProgrammaticUpdate = true;
	if (m_yaw) m_yaw->setValue(0.0);
	if (m_pitch) m_pitch->setValue(0.0);
	if (m_roll) m_roll->setValue(0.0);
	m_inProgrammaticUpdate = false;
	emit rotationChanged(0.0, 0.0, 0.0, true);
	if (m_resliceHelper) updateReslice(true);
}

void VolumeRotationWidget::onApplyDownsampleClicked()
{
	int factor = 1;
	if (m_downsampleCombo) factor = m_downsampleCombo->currentData().toInt();
	setDownsampleFactor(factor);
	if (!m_resliceHelper) return;
	m_resliceHelper->SetDownsampleFactor(m_downsampleFactor);
	// Only update now (deferred until user clicks Apply)
	m_resliceHelper->Update();
	vtkImageData* outImg = vtkImageData::SafeDownCast(m_resliceHelper->GetReslice()->GetOutput());
	if (outImg) emit resliceReady(outImg);
	emit reslicePortReady(m_resliceHelper->GetOutputPort());
	// Notify listeners that an explicit Apply completed and downstream widgets may need to refresh their internal state
	emit resliceApplied();
}

void VolumeRotationWidget::updateReslice(bool finished)
{
	if (!m_resliceHelper) return;
	vtkTransform* t = m_resliceHelper->GetTransform();
	if (!t) return;
	// Reset then apply: yaw(Z), pitch(X), roll(Y)
	t->Identity();
	t->RotateZ(yaw());
	t->RotateX(pitch());
	t->RotateY(roll());

	// If live update is enabled, ensure the helper uses the current downsample selection
	m_resliceHelper->SetDownsampleFactor(m_downsampleFactor);

	m_resliceHelper->Update();
	vtkImageData* outImg = vtkImageData::SafeDownCast(m_resliceHelper->GetReslice()->GetOutput());
	if (outImg) {
		emit resliceReady(outImg);
	}
	// also emit port for pipeline usage
	emit reslicePortReady(m_resliceHelper->GetOutputPort());
}

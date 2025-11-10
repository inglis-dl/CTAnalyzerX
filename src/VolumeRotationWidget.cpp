#include "VolumeRotationWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>

#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkTransform.h>
#include <vtkSmartPointer.h>
#include <vtkImageChangeInformation.h>
#include <vtkAlgorithmOutput.h>

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

	// Flattened: create internal reslice pipeline pieces previously owned by ImageResliceHelper.
	m_reslice = vtkSmartPointer<vtkImageReslice>::New();
	m_resliceTransform = vtkSmartPointer<vtkTransform>::New();
	m_changeInfo = vtkSmartPointer<vtkImageChangeInformation>::New();

	// wire: changeInfo -> reslice
	m_reslice->SetInputConnection(m_changeInfo->GetOutputPort());
	m_reslice->SetResliceTransform(m_resliceTransform);
	m_reslice->SetInterpolationModeToCubic();
	m_reslice->AutoCropOutputOn();

	setOperational(false);
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
	// keep internal reslice downsample in sync
	m_resliceDownsample = m_downsampleFactor;
}

void VolumeRotationWidget::setInputData(vtkImageData* img)
{
	// Set raw input into the pipeline (via change info) and update downsample.
	if (!m_operational || !m_changeInfo) return;
	m_changeInfo->SetInputData(img);
	// ensure changeInfo can provide metadata for immediate grid centering
	m_changeInfo->Update();
	m_resliceDownsample = m_downsampleFactor;

	// Compute and cache input center (physical) so Apply can compose transforms quickly.
	vtkImageData* in = vtkImageData::SafeDownCast(m_changeInfo->GetOutput());
	if (in) {
		double origin[3]; in->GetOrigin(origin);
		double spacing[3]; in->GetSpacing(spacing);
		int extent[6]; in->GetExtent(extent);
		double centerIndex[3];
		centerIndex[0] = 0.5 * (extent[0] + extent[1]);
		centerIndex[1] = 0.5 * (extent[2] + extent[3]);
		centerIndex[2] = 0.5 * (extent[4] + extent[5]);
		m_inputCenter[0] = origin[0] + centerIndex[0] * spacing[0];
		m_inputCenter[1] = origin[1] + centerIndex[1] * spacing[1];
		m_inputCenter[2] = origin[2] + centerIndex[2] * spacing[2];
		m_hasInputCenter = true;
	}
	else {
		m_hasInputCenter = false;
	}

	// Recompute output grid (spacing/origin/extent) immediately so downstream mappers have a centered grid.
	computeOutputGridFromInput();
	// Do NOT call m_reslice->Update() here: we only update pipeline when user clicks Apply.
}

void VolumeRotationWidget::setInputConnection(vtkAlgorithmOutput* port)
{
	if (!m_operational || !m_changeInfo) return;
	m_changeInfo->SetInputConnection(port);
	// Ensure we can read metadata to center grid immediately
	m_changeInfo->Update();
	m_resliceDownsample = m_downsampleFactor;

	// Cache center and recompute output grid so downstream consumers have a centered grid immediately.
	vtkImageData* in = vtkImageData::SafeDownCast(m_changeInfo->GetOutput());
	if (in) {
		double origin[3]; in->GetOrigin(origin);
		double spacing[3]; in->GetSpacing(spacing);
		int extent[6]; in->GetExtent(extent);
		double centerIndex[3];
		centerIndex[0] = 0.5 * (extent[0] + extent[1]);
		centerIndex[1] = 0.5 * (extent[2] + extent[3]);
		centerIndex[2] = 0.5 * (extent[4] + extent[5]);
		m_inputCenter[0] = origin[0] + centerIndex[0] * spacing[0];
		m_inputCenter[1] = origin[1] + centerIndex[1] * spacing[1];
		m_inputCenter[2] = origin[2] + centerIndex[2] * spacing[2];
		m_hasInputCenter = true;
	}
	else {
		m_hasInputCenter = false;
	}

	computeOutputGridFromInput();
	// Do not Update() m_reslice here; wait for user Apply.
}

void VolumeRotationWidget::onValueChanged(double)
{
	if (m_inProgrammaticUpdate) return;
	// Store pending values; do not touch the pipeline until Apply is clicked.
	m_pendingYaw = yaw();
	m_pendingPitch = pitch();
	m_pendingRoll = roll();
	const bool finished = false;
	emit rotationChanged(m_pendingYaw, m_pendingPitch, m_pendingRoll, finished);
}

void VolumeRotationWidget::onEditingFinished()
{
	if (m_inProgrammaticUpdate) return;
	m_pendingYaw = yaw();
	m_pendingPitch = pitch();
	m_pendingRoll = roll();
	emit rotationChanged(m_pendingYaw, m_pendingPitch, m_pendingRoll, true);
	// do not update pipeline here
}

void VolumeRotationWidget::onResetClicked()
{
	// Temporarily inhibit signal feedback from programmatic UI changes
	m_inProgrammaticUpdate = true;
	if (m_yaw) m_yaw->setValue(0.0);
	if (m_pitch) m_pitch->setValue(0.0);
	if (m_roll) m_roll->setValue(0.0);
	m_inProgrammaticUpdate = false;

	// Reset pending rotation state (do not change downsample)
	m_pendingYaw = m_pendingPitch = m_pendingRoll = 0.0;

	// Notify listeners about the UI-level reset
	emit rotationChanged(0.0, 0.0, 0.0, true);

	// If not operational or no pipeline pieces, don't attempt to update VTK pipeline
	if (!m_operational || !m_changeInfo || !m_reslice || !m_resliceTransform) {
		return;
	}

	// Ensure we have current metadata from the input so grid computations are correct
	m_changeInfo->Update();

	// Recompute the reslice output grid to be centered on the input image (preserves current downsample)
	computeOutputGridFromInput();

	// Reset the reslice transform to identity so there is no rotation applied
	m_resliceTransform->Identity();
	m_reslice->SetResliceTransform(m_resliceTransform);

	// Run the reslice so downstream consumers see the reset image immediately
	m_reslice->Update();

	// Emit updated results so both code paths (image consumers and pipeline consumers) are notified
	vtkImageData* outImg = vtkImageData::SafeDownCast(m_reslice->GetOutput());
	if (outImg) emit resliceReady(outImg);
}

void VolumeRotationWidget::onApplyDownsampleClicked()
{
	int factor = 1;
	if (m_downsampleCombo) factor = m_downsampleCombo->currentData().toInt();
	setDownsampleFactor(factor);
	// Apply to internal reslice state and update
	m_resliceDownsample = m_downsampleFactor;
	// Compose and apply pending rotations + downsample to the reslice and run the pipeline now.
	applyPendingChanges();

	// Notify listeners that an explicit Apply completed and downstream widgets may need to refresh their internal state
	emit resliceApplied();
}

void VolumeRotationWidget::updateReslice(bool finished)
{
	// Deprecated: use applyPendingChanges() to make a single coherent Apply action.
	applyPendingChanges();
}

vtkAlgorithmOutput* VolumeRotationWidget::getOutputPort()
{
	if (!m_reslice) return nullptr;
	return m_reslice->GetOutputPort();
}

// Flattened helper implementations (mirroring ImageResliceHelper behavior)
void VolumeRotationWidget::computeOutputGridFromInput()
{
	// use changeInfo output as the input to reslice
	vtkImageData* in = vtkImageData::SafeDownCast(m_changeInfo->GetOutput());
	if (!in) return;

	double inSpacing[3]; in->GetSpacing(inSpacing);
	double inOrigin[3]; in->GetOrigin(inOrigin);
	int inExt[6]; in->GetExtent(inExt);

	// Compute reduced spacing based on integer downsample factor
	double outSpacing[3] = { inSpacing[0] * m_resliceDownsample, inSpacing[1] * m_resliceDownsample, inSpacing[2] * m_resliceDownsample };

	int outExt[6];
	double outOrigin[3];

	for (int i = 0; i < 3; ++i) {
		int inCount = inExt[2 * i + 1] - inExt[2 * i] + 1;
		if (inCount < 1) inCount = 1;
		// compute output count rounding up so we cover full physical extent
		int outCount = (inCount + m_resliceDownsample - 1) / m_resliceDownsample;
		if (outCount < 1) outCount = 1;

		// compute input min and max in physical coordinates
		double inMinPhys = inOrigin[i] + inExt[2 * i] * inSpacing[i];
		double inMaxPhys = inOrigin[i] + inExt[2 * i + 1] * inSpacing[i];
		double inCenterPhys = 0.5 * (inMinPhys + inMaxPhys);

		// compute origin so that the output grid is centered at inCenterPhys
		double halfExtentPhys = 0.5 * (outCount - 1) * outSpacing[i];
		outOrigin[i] = inCenterPhys - halfExtentPhys;

		outExt[2 * i] = 0;
		outExt[2 * i + 1] = outCount - 1;
	}

	m_reslice->SetOutputSpacing(outSpacing);
	m_reslice->SetOutputOrigin(outOrigin);
	m_reslice->SetOutputExtent(outExt);
}

void VolumeRotationWidget::Update()
{
	if (!m_changeInfo || !m_reslice) return;
	// Ensure changeInfo is updated so its output is available to reslice
	m_changeInfo->Update();

	// Recompute output grid before updating reslice.
	computeOutputGridFromInput();

	// Compose reslice transform: translate to center, apply user transform, translate back
	double center[3];
	vtkImageData* in = vtkImageData::SafeDownCast(m_changeInfo->GetOutput());
	/*
	if (in && m_resliceTransform) {
		// compute center
		double origin[3]; in->GetOrigin(origin);
		double spacing[3]; in->GetSpacing(spacing);
		int extent[6]; in->GetExtent(extent);
		double centerIndex[3];
		centerIndex[0] = 0.5 * (extent[0] + extent[1]);
		centerIndex[1] = 0.5 * (extent[2] + extent[3]);
		centerIndex[2] = 0.5 * (extent[4] + extent[5]);
		center[0] = origin[0] + centerIndex[0] * spacing[0];
		center[1] = origin[1] + centerIndex[1] * spacing[1];
		center[2] = origin[2] + centerIndex[2] * spacing[2];

		m_resliceTransform->Identity();
		m_resliceTransform->Translate(center[0], center[1], center[2]);
		m_resliceTransform->Translate(-center[0], -center[1], -center[2]);
		m_reslice->SetResliceTransform(m_resliceTransform);
	}
	*/

	m_reslice->Update();
}

vtkImageReslice* VolumeRotationWidget::GetReslice()
{
	return m_reslice.Get();
}

// Toggle operational mode. When turned OFF:
//  - UI controls are disabled (visual/interaction)
//  - upstream input connection / data is detached from m_changeInfo
//  - internal processing (Update()/updateReslice()) will early-return.
// When turned ON the caller is responsible for re-attaching the input port
// (call setInputConnection(...) or setInputData(...)).
void VolumeRotationWidget::setOperational(bool on)
{
	if (m_operational == on) return;
	m_operational = on;

	// Visual / UI enable/disable
	if (m_yaw) m_yaw->setEnabled(on);
	if (m_pitch) m_pitch->setEnabled(on);
	if (m_roll) m_roll->setEnabled(on);
	if (m_live) m_live->setEnabled(on);
	if (m_reset) m_reset->setEnabled(on);
	if (m_downsampleCombo) m_downsampleCombo->setEnabled(on);
	if (m_applyDownsample) m_applyDownsample->setEnabled(on);

	// When disabling, detach any upstream VTK input so pipeline stops producing.
	// Also clear any input data that may keep the pipeline alive.
	if (!on) {
		if (m_changeInfo) {
			// Remove connection and any raw data pointer. This prevents further Update() work.
			// Use SetInputConnection(nullptr) first (VTK will ignore a null), then SetInputData(nullptr).
			m_changeInfo->SetInputConnection(nullptr);
			m_changeInfo->SetInputData(nullptr);
		}
		// Reset transforms/state to safe defaults
		m_resliceDownsample = 1;
	}
	// If enabling we do not auto-reattach any prior input: caller must rewire explicitly.
}

// Compose pending rotations and downsample into the reslice transform + run the pipeline.
void VolumeRotationWidget::applyPendingChanges()
{
	if (!m_operational || !m_reslice || !m_changeInfo) return;

	// ensure we have up-to-date input metadata (center)
	m_changeInfo->Update();
	vtkImageData* in = vtkImageData::SafeDownCast(m_changeInfo->GetOutput());
	if (in) {
		double origin[3]; in->GetOrigin(origin);
		double spacing[3]; in->GetSpacing(spacing);
		int extent[6]; in->GetExtent(extent);
		double centerIndex[3];
		centerIndex[0] = 0.5 * (extent[0] + extent[1]);
		centerIndex[1] = 0.5 * (extent[2] + extent[3]);
		centerIndex[2] = 0.5 * (extent[4] + extent[5]);
		m_inputCenter[0] = origin[0] + centerIndex[0] * spacing[0];
		m_inputCenter[1] = origin[1] + centerIndex[1] * spacing[1];
		m_inputCenter[2] = origin[2] + centerIndex[2] * spacing[2];
		m_hasInputCenter = true;
	}
	else {
		m_hasInputCenter = false;
	}

	// Update output grid to reflect current downsample
	computeOutputGridFromInput();

	// If a pending center offset was requested, adjust changeInfo's output origin so that
	// the reslice sees a shifted grid (this effectively moves the rotation center).
	// We compute the new origin as the current changeInfo output origin + pending offset.
	if (m_hasInputCenter) {
		// Read the current changeInfo output origin (use input 'in' which was updated above)
		double currentOutOrigin[3] = { 0.0, 0.0, 0.0 };
		in->GetOrigin(currentOutOrigin); // origin of the changeInfo output

		// Apply pending center offset to the changeInfo output origin so that subsequent
		// center computation is shifted by the same physical vector.
		double newOutOrigin[3] = {
			currentOutOrigin[0] + m_pendingCenterOffset[0],
			currentOutOrigin[1] + m_pendingCenterOffset[1],
			currentOutOrigin[2] + m_pendingCenterOffset[2]
		};

		m_changeInfo->SetOutputOrigin(newOutOrigin);
		// force an update so the reslice and center computation below use the new origin
		m_changeInfo->Update();

		// recompute the input center after we changed the changeInfo output origin
		vtkImageData* in2 = vtkImageData::SafeDownCast(m_changeInfo->GetOutput());
		if (in2) {
			double origin2[3]; in2->GetOrigin(origin2);
			double spacing2[3]; in2->GetSpacing(spacing2);
			int extent2[6]; in2->GetExtent(extent2);
			double centerIndex2[3];
			centerIndex2[0] = 0.5 * (extent2[0] + extent2[1]);
			centerIndex2[1] = 0.5 * (extent2[2] + extent2[3]);
			centerIndex2[2] = 0.5 * (extent2[4] + extent2[5]);
			m_inputCenter[0] = origin2[0] + centerIndex2[0] * spacing2[0];
			m_inputCenter[1] = origin2[1] + centerIndex2[1] * spacing2[1];
			m_inputCenter[2] = origin2[2] + centerIndex2[2] * spacing2[2];
			m_hasInputCenter = true;
		}
	}

	// Compose transform directly into m_resliceTransform using cached (possibly shifted) center
	if (m_hasInputCenter && m_resliceTransform) {
		m_resliceTransform->Identity();
		// translate to center
		m_resliceTransform->Translate(m_inputCenter[0], m_inputCenter[1], m_inputCenter[2]);
		// apply rotations in widget order: yaw(Z), pitch(X), roll(Y)
		m_resliceTransform->RotateZ(m_pendingYaw);
		m_resliceTransform->RotateX(m_pendingPitch);
		m_resliceTransform->RotateY(m_pendingRoll);
		// translate back
		m_resliceTransform->Translate(-m_inputCenter[0], -m_inputCenter[1], -m_inputCenter[2]);
		m_reslice->SetResliceTransform(m_resliceTransform);
	}

	// Ensure downsample is applied
	m_resliceDownsample = m_downsampleFactor;

	// Run reslice now that transform + grid are set
	m_reslice->Update();

	// Notify listeners: provide concrete image and port for downstream pipeline or setImageData users
	vtkImageData* outImg = vtkImageData::SafeDownCast(m_reslice->GetOutput());
	if (outImg) emit resliceReady(outImg);
}

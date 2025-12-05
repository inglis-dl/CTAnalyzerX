#include "WorkflowPanelWidget.h"
#include "CollapsibleGroupBox.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QScrollArea>

WorkflowPanelWidget::WorkflowPanelWidget(QWidget* parent)
	: QWidget(parent)
{
	// Scrollable vertical layout to fit many groups on left panel
	m_scrollArea = new QScrollArea(this);
	m_scrollArea->setWidgetResizable(true);
	m_scrollArea->setFrameStyle(QFrame::NoFrame);

	m_scrollContent = new QWidget(m_scrollArea);
	m_rootLayout = new QVBoxLayout(m_scrollContent);
	m_rootLayout->setContentsMargins(6, 6, 6, 6);
	m_rootLayout->setSpacing(8);
	m_scrollContent->setLayout(m_rootLayout);
	m_scrollArea->setWidget(m_scrollContent);

	auto* outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);
	outer->addWidget(m_scrollArea);

	// --- Image load group ---
	m_grpLoad = makeGroup(tr("Image"));
	m_btnLoadImage = new QPushButton(tr("Load Image..."), m_grpLoad);
	m_loadContainer = new QWidget(m_grpLoad);
	{
		auto* lay = new QVBoxLayout(m_loadContainer);
		lay->setContentsMargins(0, 0, 0, 0);
		lay->addWidget(makePlaceholderLabel(tr("Image loader / source selector will appear here.")));
		lay->addWidget(m_btnLoadImage);
	}
	m_grpLoad->setContentWidget(m_loadContainer);
	m_rootLayout->addWidget(m_grpLoad);
	connect(m_btnLoadImage, &QPushButton::clicked, this, &WorkflowPanelWidget::onLoadImageClicked);

	// --- Cropping group (steps 2,3,4) ---
	m_grpCropping = makeGroup(tr("Cropping"));
	m_croppingContainer = new QWidget(m_grpCropping);
	{
		auto* cl = new QVBoxLayout(m_croppingContainer);
		cl->setContentsMargins(0, 0, 0, 0);
		cl->setSpacing(6);
		cl->addWidget(makePlaceholderLabel(tr("VolumeCroppingWidget placeholder (Range sliders + 3D box widget)")));
		m_btnDefineCrop = new QPushButton(tr("Define Crop (Enable box)"), m_croppingContainer);
		m_btnApplyCrop = new QPushButton(tr("Apply Crop & Save Temp"), m_croppingContainer);
		m_btnSaveCropped = new QPushButton(tr("Save Cropped Volume..."), m_croppingContainer); // new
		m_btnLoadCropped = new QPushButton(tr("Load Cropped Volume"), m_croppingContainer);
		cl->addWidget(m_btnDefineCrop);
		cl->addWidget(m_btnApplyCrop);
		cl->addWidget(m_btnSaveCropped);
		cl->addWidget(m_btnLoadCropped);
	}
	m_grpCropping->setContentWidget(m_croppingContainer);
	m_rootLayout->addWidget(m_grpCropping);
	connect(m_btnDefineCrop, &QPushButton::clicked, this, &WorkflowPanelWidget::onDefineCropClicked);
	connect(m_btnApplyCrop, &QPushButton::clicked, this, &WorkflowPanelWidget::onApplyCropClicked);
	connect(m_btnLoadCropped, &QPushButton::clicked, this, &WorkflowPanelWidget::onLoadCroppedClicked);
	connect(m_btnSaveCropped, &QPushButton::clicked, this, &WorkflowPanelWidget::onSaveCroppedClicked); // new

	// --- Fiducials / Axes group (step 5) ---
	m_grpFiducials = makeGroup(tr("Fiducials & Axes"));
	m_fiducialsContainer = new QWidget(m_grpFiducials);
	{
		auto* fl = new QVBoxLayout(m_fiducialsContainer);
		fl->setContentsMargins(0, 0, 0, 0);
		fl->addWidget(makePlaceholderLabel(tr("Fiducial placement controls placeholder (define plane & rotation axes)")));
		m_btnPlaceFiducials = new QPushButton(tr("Place Fiducials"), m_fiducialsContainer);
		fl->addWidget(m_btnPlaceFiducials);
	}
	m_grpFiducials->setContentWidget(m_fiducialsContainer);
	m_rootLayout->addWidget(m_grpFiducials);
	connect(m_btnPlaceFiducials, &QPushButton::clicked, this, &WorkflowPanelWidget::onPlaceFiducialsClicked);

	// --- Rotation group (steps 6,7) ---
	m_grpRotation = makeGroup(tr("Rotation"));
	m_rotationContainer = new QWidget(m_grpRotation);
	{
		auto* rl = new QVBoxLayout(m_rotationContainer);
		rl->setContentsMargins(0, 0, 0, 0);
		rl->addWidget(makePlaceholderLabel(tr("VolumeRotationWidget placeholder (axes widget + spinboxes)")));
		m_btnStartInteractiveRotation = new QPushButton(tr("Start Interactive Rotation"), m_rotationContainer);
		m_btnApplyRotation = new QPushButton(tr("Apply Rotation & Save"), m_rotationContainer);
		rl->addWidget(m_btnStartInteractiveRotation);
		rl->addWidget(m_btnApplyRotation);
	}
	m_grpRotation->setContentWidget(m_rotationContainer);
	m_rootLayout->addWidget(m_grpRotation);
	connect(m_btnStartInteractiveRotation, &QPushButton::clicked, this, &WorkflowPanelWidget::onStartInteractiveRotationClicked);
	connect(m_btnApplyRotation, &QPushButton::clicked, this, &WorkflowPanelWidget::onApplyRotationClicked);

	// --- Segmentation group (steps 8,9,10) ---
	m_grpSegmentation = makeGroup(tr("Segmentation"));
	m_segmentationContainer = new QWidget(m_grpSegmentation);
	{
		auto* sl = new QVBoxLayout(m_segmentationContainer);
		sl->setContentsMargins(0, 0, 0, 0);
		sl->addWidget(makePlaceholderLabel(tr("Thresholding / region growing UI placeholder (histogram, preview)")));
		m_btnComputeThreshold = new QPushButton(tr("Compute Threshold (Otsu)"), m_segmentationContainer);
		m_btnPreviewThreshold = new QPushButton(tr("Preview Threshold"), m_segmentationContainer);
		m_btnRunSegmentation = new QPushButton(tr("Run Region Growing"), m_segmentationContainer);
		m_btnSaveSegment = new QPushButton(tr("Save Segmented Volume"), m_segmentationContainer);
		sl->addWidget(m_btnComputeThreshold);
		sl->addWidget(m_btnPreviewThreshold);
		sl->addWidget(m_btnRunSegmentation);
		sl->addWidget(m_btnSaveSegment);
	}
	m_grpSegmentation->setContentWidget(m_segmentationContainer);
	m_rootLayout->addWidget(m_grpSegmentation);
	connect(m_btnComputeThreshold, &QPushButton::clicked, this, &WorkflowPanelWidget::onComputeThresholdClicked);
	connect(m_btnPreviewThreshold, &QPushButton::clicked, this, &WorkflowPanelWidget::onPreviewThresholdClicked);
	connect(m_btnRunSegmentation, &QPushButton::clicked, this, &WorkflowPanelWidget::onRunSegmentationClicked);
	connect(m_btnSaveSegment, &QPushButton::clicked, this, &WorkflowPanelWidget::onSaveSegmentClicked);

	// --- Appearance group (independent) ---
	m_grpAppearance = makeGroup(tr("Appearance"));
	m_appearanceContainer = new QWidget(m_grpAppearance);
	{
		auto* al = new QVBoxLayout(m_appearanceContainer);
		al->setContentsMargins(0, 0, 0, 0);
		al->addWidget(makePlaceholderLabel(tr("Window/Level controller and non-processing appearance controls")));
		// real WindowLevelController can be inserted via insertAppearanceWidget()
	}
	m_grpAppearance->setContentWidget(m_appearanceContainer);
	m_rootLayout->addWidget(m_grpAppearance);

	// spacer to push groups up
	m_rootLayout->addStretch(1);
}

CollapsibleGroupBox* WorkflowPanelWidget::makeGroup(const QString& title)
{
	auto* g = new CollapsibleGroupBox(title, this);
	g->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
	return g;
}

QLabel* WorkflowPanelWidget::makePlaceholderLabel(const QString& text)
{
	auto* l = new QLabel(text, this);
	l->setWordWrap(true);
	l->setFrameStyle(QFrame::StyledPanel | QFrame::Plain);
	l->setContentsMargins(4, 4, 4, 4);
	return l;
}

// Insert real widgets into placeholders (reparent & add to layout)
void WorkflowPanelWidget::insertVolumeCroppingWidget(QWidget* widget)
{
	if (!widget) return;
	// clear existing content
	if (m_customCroppingWidget) {
		m_customCroppingWidget->setParent(nullptr);
	}
	widget->setParent(m_croppingContainer);
	auto* lay = qobject_cast<QVBoxLayout*>(m_croppingContainer->layout());
	if (lay) {
		lay->insertWidget(0, widget);
	}
	m_customCroppingWidget = widget;
}

void WorkflowPanelWidget::insertFiducialsWidget(QWidget* widget)
{
	if (!widget) return;
	if (m_customFiducialsWidget) m_customFiducialsWidget->setParent(nullptr);
	widget->setParent(m_fiducialsContainer);
	auto* lay = qobject_cast<QVBoxLayout*>(m_fiducialsContainer->layout());
	if (lay) lay->insertWidget(0, widget);
	m_customFiducialsWidget = widget;
}

void WorkflowPanelWidget::insertVolumeRotationWidget(QWidget* widget)
{
	if (!widget) return;
	if (m_customRotationWidget) m_customRotationWidget->setParent(nullptr);
	widget->setParent(m_rotationContainer);
	auto* lay = qobject_cast<QVBoxLayout*>(m_rotationContainer->layout());
	if (lay) lay->insertWidget(0, widget);
	m_customRotationWidget = widget;
}

void WorkflowPanelWidget::insertSegmentationWidget(QWidget* widget)
{
	if (!widget) return;
	if (m_customSegmentationWidget) m_customSegmentationWidget->setParent(nullptr);
	widget->setParent(m_segmentationContainer);
	auto* lay = qobject_cast<QVBoxLayout*>(m_segmentationContainer->layout());
	if (lay) lay->insertWidget(0, widget);
	m_customSegmentationWidget = widget;
}

void WorkflowPanelWidget::insertAppearanceWidget(QWidget* widget)
{
	if (!widget) return;
	if (m_customAppearanceWidget) m_customAppearanceWidget->setParent(nullptr);
	widget->setParent(m_appearanceContainer);
	auto* lay = qobject_cast<QVBoxLayout*>(m_appearanceContainer->layout());
	if (lay) lay->insertWidget(0, widget);
	m_customAppearanceWidget = widget;
}

// --- Group enable/disable helpers (controller-friendly) ---
void WorkflowPanelWidget::setCroppingEnabled(bool on)
{
	if (m_grpCropping) m_grpCropping->setEnabled(on);
	if (m_btnDefineCrop) m_btnDefineCrop->setEnabled(on);
	if (m_btnApplyCrop) m_btnApplyCrop->setEnabled(on);
	if (m_btnLoadCropped) m_btnLoadCropped->setEnabled(on);
	if (m_btnSaveCropped) m_btnSaveCropped->setEnabled(on);
}

bool WorkflowPanelWidget::isCroppingEnabled() const
{
	return m_grpCropping ? m_grpCropping->isEnabled() : false;
}

void WorkflowPanelWidget::setRotationEnabled(bool on)
{
	if (m_grpRotation) m_grpRotation->setEnabled(on);
	if (m_btnStartInteractiveRotation) m_btnStartInteractiveRotation->setEnabled(on);
	if (m_btnApplyRotation) m_btnApplyRotation->setEnabled(on);
}

bool WorkflowPanelWidget::isRotationEnabled() const
{
	return m_grpRotation ? m_grpRotation->isEnabled() : false;
}

void WorkflowPanelWidget::setSegmentationEnabled(bool on)
{
	if (m_grpSegmentation) m_grpSegmentation->setEnabled(on);
	if (m_btnComputeThreshold) m_btnComputeThreshold->setEnabled(on);
	if (m_btnPreviewThreshold) m_btnPreviewThreshold->setEnabled(on);
	if (m_btnRunSegmentation) m_btnRunSegmentation->setEnabled(on);
	if (m_btnSaveSegment) m_btnSaveSegment->setEnabled(on);
}

bool WorkflowPanelWidget::isSegmentationEnabled() const
{
	return m_grpSegmentation ? m_grpSegmentation->isEnabled() : false;
}

void WorkflowPanelWidget::setFiducialsEnabled(bool on)
{
	if (m_grpFiducials) m_grpFiducials->setEnabled(on);
	if (m_btnPlaceFiducials) m_btnPlaceFiducials->setEnabled(on);
}

bool WorkflowPanelWidget::isFiducialsEnabled() const
{
	return m_grpFiducials ? m_grpFiducials->isEnabled() : false;
}

void WorkflowPanelWidget::setAppearanceEnabled(bool on)
{
	if (m_grpAppearance) m_grpAppearance->setEnabled(on);
	// appearance container may contain multiple widgets; rely on group enable to propagate
}

bool WorkflowPanelWidget::isAppearanceEnabled() const
{
	return m_grpAppearance ? m_grpAppearance->isEnabled() : false;
}

// -----------------------------------------------------------------------------
// Missing slot definitions - emit the corresponding signals
// -----------------------------------------------------------------------------
void WorkflowPanelWidget::onLoadImageClicked()
{
	emit loadImageRequested();
}

void WorkflowPanelWidget::onDefineCropClicked()
{
	emit defineCropRequested();
}

void WorkflowPanelWidget::onApplyCropClicked()
{
	emit applyCropRequested();
}

void WorkflowPanelWidget::onLoadCroppedClicked()
{
	emit loadCroppedRequested();
}

void WorkflowPanelWidget::onPlaceFiducialsClicked()
{
	emit placeFiducialsRequested();
}

void WorkflowPanelWidget::onStartInteractiveRotationClicked()
{
	emit startInteractiveRotationRequested();
}

void WorkflowPanelWidget::onApplyRotationClicked()
{
	emit applyRotationRequested();
}

void WorkflowPanelWidget::onComputeThresholdClicked()
{
	emit computeThresholdRequested();
}

void WorkflowPanelWidget::onPreviewThresholdClicked()
{
	emit previewThresholdRequested();
}

void WorkflowPanelWidget::onRunSegmentationClicked()
{
	emit runSegmentationRequested();
}

void WorkflowPanelWidget::onSaveSegmentClicked()
{
	emit saveSegmentRequested();
}

// new: save cropped slot
void WorkflowPanelWidget::onSaveCroppedClicked()
{
	emit saveCroppedRequested();
}

// new fine-grained control implementations
void WorkflowPanelWidget::setDefineCropEnabled(bool on)
{
	if (m_btnDefineCrop) m_btnDefineCrop->setEnabled(on);
}

void WorkflowPanelWidget::setApplyCropEnabled(bool on)
{
	if (m_btnApplyCrop) m_btnApplyCrop->setEnabled(on);
}

void WorkflowPanelWidget::setSaveCroppedEnabled(bool on)
{
	if (m_btnSaveCropped) m_btnSaveCropped->setEnabled(on);
}

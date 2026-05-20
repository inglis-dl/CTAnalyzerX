#include "WorkflowPanelWidget.h"
#include "ui_WorkflowPanelWidget.h"

#include "CropWidget.h"
#include "ImageInfoWidget.h"
#include "LightboxWidget.h"
#include "WindowLevelWidget.h"

#include <QDebug>
#include <QDoubleValidator>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSizePolicy>


WorkflowPanelWidget::WorkflowPanelWidget(QWidget* parent)
	: QWidget(parent)
	, ui(new Ui::WorkflowPanelWidget)
{
	ui->setupUi(this);
	init();
}

WorkflowPanelWidget::~WorkflowPanelWidget()
{
	delete ui;
}

void WorkflowPanelWidget::init()
{
	// Adopt UI widgets into member variables so rest of the class can
	// continue using the same names and logic.
	m_scrollArea = ui->scrollArea;
	m_scrollContent = ui->scrollContent;
	m_rootLayout = ui->rootLayout;

	// Group boxes
	m_grpImageInfo = ui->grpImageInfo;
	m_grpCrop = ui->grpCrop;
	m_grpWindowLevel = ui->grpWindowLevel;
	m_grpThreshold = ui->grpThreshold;

	// Wire CropWidget signals to panel-level signals
	if (auto* widget = ui->cropWidget) {
		connect(widget, &CropWidget::defineCropToggled,
			this, [this](bool) { emit defineCropRequested(); });

		connect(widget, &CropWidget::saveCroppedRequested,
			this, &WorkflowPanelWidget::saveCroppedRequested);

		connect(widget, &CropWidget::croppingRegionChanged,
			this, &WorkflowPanelWidget::croppingRegionChanged,
			Qt::UniqueConnection);

		// Reset forwarder
		connect(widget, &CropWidget::resetCropRequested,
			this, &WorkflowPanelWidget::resetCropRequested,
			Qt::UniqueConnection);
	}

	// Window/Level group: .ui embeds WindowLevelWidget named "windowLevelWidget"
	if (auto* widget = ui->windowLevelWidget) {
		connect(widget, &WindowLevelWidget::windowLevelCommitted,
			this, &WorkflowPanelWidget::windowLevelAdjusted,
			Qt::UniqueConnection);
	}

	if (ui->editThreshold) {
		auto* validator = new QDoubleValidator(ui->editThreshold);
		validator->setNotation(QDoubleValidator::StandardNotation);
		ui->editThreshold->setValidator(validator);
	}

	if (ui->btnThresholdOtsu) {
		connect(ui->btnThresholdOtsu, &QPushButton::clicked, this, [this]() {
			emit otsuThresholdRequested();
		});
	}

	if (ui->btnThresholdSave) {
		connect(ui->btnThresholdSave, &QPushButton::clicked, this, [this]() {
			bool ok = false;
			const double value = ui->editThreshold ? ui->editThreshold->text().trimmed().toDouble(&ok) : 0.0;
			if (ok) {
				emit thresholdSaveRequested(value);
			}
		});
	}

	// Ensure groups adopt same size/policy behavior
	adjustGroupWidths();

	// Add stretch to push groups to the top
	if (m_rootLayout) {
		m_rootLayout->addStretch(1);
	}
}

void WorkflowPanelWidget::adjustGroupWidths()
{
	if (!m_scrollArea || !m_scrollContent) {
		return;
	}

	const int avail = m_scrollArea->viewport()->width();
	const int padding = 24;
	const int target = qMax(220, avail - padding);

	const QList<QWidget*> groups = {
		m_grpImageInfo,
		m_grpCrop,
		m_grpThreshold,
		m_grpWindowLevel
	};

	for (QWidget* g : groups) {
		if (!g) {
			continue;
		}
		g->setMaximumWidth(target);
		g->setMinimumWidth(0);
		g->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	}

	m_scrollContent->setMinimumWidth(0);
	m_scrollContent->setMaximumWidth(target);
}

void WorkflowPanelWidget::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);
	adjustGroupWidths();
}

void WorkflowPanelWidget::setCroppingEnabled(bool on)
{
	if (!m_grpCrop) {
		return;
	}

	if (auto* widget = ui->cropWidget) {
		widget->onExternalCroppingChanged(on);
	}

	m_grpCrop->setEnabled(on);
	m_grpCrop->setCollapsed(!on);
}

bool WorkflowPanelWidget::isCroppingEnabled() const
{
	return m_grpCrop ? m_grpCrop->isEnabled() : false;
}

void WorkflowPanelWidget::setThresholdEnabled(bool on)
{
	if (!m_grpThreshold) {
		return;
	}
	m_grpThreshold->setEnabled(on);
	m_grpThreshold->setCollapsed(!on);
}

bool WorkflowPanelWidget::isThresholdEnabled() const
{
	return m_grpThreshold ? m_grpThreshold->isEnabled() : false;
}

void WorkflowPanelWidget::setThresholdFromSidecar(bool present, double value)
{
	if (!ui->editThreshold) {
		return;
	}

	if (present) {
		ui->editThreshold->setText(QString::number(value, 'g', 12));
	}
	else {
		ui->editThreshold->clear();
	}
}

void WorkflowPanelWidget::setWindowLevellingEnabled(bool /*on*/)
{
	// Appearance controls are independent of processing pipeline.
	if (m_grpWindowLevel) {
		m_grpWindowLevel->setEnabled(true);
		m_grpWindowLevel->setCollapsed(false);
	}
}

bool WorkflowPanelWidget::isWindowLevellingEnabled() const
{
	return m_grpWindowLevel ? m_grpWindowLevel->isEnabled() : false;
}

WindowLevelWidget* WorkflowPanelWidget::windowLevelWidget() const
{
	return ui->windowLevelWidget;
}

ImageInfoWidget* WorkflowPanelWidget::imageInfoWidget() const
{
	return ui->imageInfoWidget;
}

void WorkflowPanelWidget::notifyWorkflowRestored(WorkflowStateMachine::State restoredState)
{
	CollapsibleGroupBox* targetGroup = nullptr;

	switch (restoredState) {
		case WorkflowStateMachine::DefiningCrop:
		case WorkflowStateMachine::LoadingCropped:
		targetGroup = m_grpCrop;
		break;
		case WorkflowStateMachine::ReplacingThreshold:
		targetGroup = m_grpThreshold;
		break;
		default:
		return;
	}

	if (targetGroup) {
		targetGroup->setCollapsed(false);
		qDebug() << "Restored workflow to"
			<< WorkflowStateMachine::stateToString(restoredState);
	}
}

void WorkflowPanelWidget::setSaveCroppedEnabled(bool on)
{
	if (auto* crop = ui->cropWidget) {
		crop->setSaveEnabled(on);
	}
}

void WorkflowPanelWidget::setLightboxWidget(LightboxWidget* lightbox)
{
	// Disconnect previous Lightbox
	if (m_lightbox) {
		disconnect(this, &WorkflowPanelWidget::croppingRegionChanged,
			m_lightbox, &LightboxWidget::setCroppingRegion);

		if (auto* widget = ui->cropWidget) {
			disconnect(m_lightbox, &LightboxWidget::imageExtentsChanged,
				widget, &CropWidget::setRangeSliders);
			disconnect(widget, &CropWidget::requestOutlineVisibility, nullptr, nullptr);
		}

		m_lightbox.clear();
	}

	if (!lightbox) {
		return;
	}

	m_lightbox = lightbox;

	connect(this, &WorkflowPanelWidget::croppingRegionChanged,
		m_lightbox, &LightboxWidget::setCroppingRegion,
		Qt::UniqueConnection);

	if (auto* widget = ui->cropWidget) {
		connect(m_lightbox, &LightboxWidget::imageExtentsChanged,
			widget, &CropWidget::setRangeSliders,
			Qt::UniqueConnection);

		if (auto* xy = m_lightbox->getXYView()) {
			connect(widget, &CropWidget::requestOutlineVisibility,
				xy, &SliceView::setOutlineVisible,
				Qt::UniqueConnection);
		}
		if (auto* xz = m_lightbox->getXZView()) {
			connect(widget, &CropWidget::requestOutlineVisibility,
				xz, &SliceView::setOutlineVisible,
				Qt::UniqueConnection);
		}
		if (auto* yz = m_lightbox->getYZView()) {
			connect(widget, &CropWidget::requestOutlineVisibility,
				yz, &SliceView::setOutlineVisible,
				Qt::UniqueConnection);
		}
		if (auto* vol = m_lightbox->getVolumeView()) {
			connect(widget, &CropWidget::requestOutlineVisibility,
				vol, &VolumeView::setOutlineVisible,
				Qt::UniqueConnection);
		}
	}
}

#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "ImageProcessingStateMachine.h"
#include "LightboxWidget.h"
#include "ImageLoader.h"
#include "WindowLevelController.h"
#include "WindowLevelBridge.h"
//#include "VolumeRotationWidget.h"   // removed: rotation widget handled via WorkflowPanelWidget now
#include "JsonSettings.h"
#include "WorkflowPanelWidget.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QKeyEvent>
#include <QImage>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QShowEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QUrl>
#include <QSysInfo>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QOpenGLFunctions>
#include <QDebug>
#include <QElapsedTimer>
#include <QThread>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QColor>
#include <QPalette>
#include <QTimer>

#include <vtkVersion.h>   // VTK version macros
#include <vtkEventQtSlotConnect.h>
#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <itkVersion.h>   // ITK version macros
#include <itkImage.h>
#include <itkImageSeriesReader.h>
#include <itkGDCMImageIO.h>
#include <itkGDCMSeriesFileNames.h>
#include <itkImageFileReader.h>
#include <itkImageToVTKImageFilter.h>

using ImageType = itk::Image<short, 3>;

namespace {
	QString queryOpenGLSummary()
	{
		QSurfaceFormat fmt;
		fmt.setRenderableType(QSurfaceFormat::OpenGL);
		QOffscreenSurface surface;
		surface.setFormat(fmt);
		surface.create();

		QOpenGLContext ctx;
		ctx.setFormat(fmt);
		if (!ctx.create() || !surface.isValid() || !ctx.makeCurrent(&surface))
			return QStringLiteral("unavailable");

		QOpenGLFunctions* f = ctx.functions();
		const char* vendor = reinterpret_cast<const char*>(f->glGetString(GL_VENDOR));
		const char* renderer = reinterpret_cast<const char*>(f->glGetString(GL_RENDERER));
		const char* version = reinterpret_cast<const char*>(f->glGetString(GL_VERSION));
		ctx.doneCurrent();

		const QString v = vendor ? QString::fromLatin1(vendor) : QStringLiteral("?");
		const QString r = renderer ? QString::fromLatin1(renderer) : QStringLiteral("?");
		const QString ver = version ? QString::fromLatin1(version) : QStringLiteral("?");

		return QStringLiteral("%1 | %2 | %3").arg(v, r, ver);
	}
}

MainWindow::MainWindow(QWidget* parent)
	: QMainWindow(parent), ui(new Ui::MainWindow), defaultImageLoaded(false)
{
	ui->setupUi(this);

	setAcceptDrops(true); // Enable drag and drop on the main window

	progressBar = new QProgressBar(this);
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->setVisible(false); // Hide by default

	statusBar()->addPermanentWidget(progressBar);

	// Connect menu actions to slots
	connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::onActionOpen);
	connect(ui->actionSave, &QAction::triggered, this, &MainWindow::onActionSave);
	connect(ui->actionExit, &QAction::triggered, this, &MainWindow::onActionExit);
	connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::onActionAbout);
	connect(ui->actionScreenshot, &QAction::triggered, this, &MainWindow::saveScreenshot);

	m_imageLoader = vtkSmartPointer<ImageLoader>::New();

	vtkConnections = vtkSmartPointer<vtkEventQtSlotConnect>::New();

	vtkConnections->Connect(
	m_imageLoader, vtkCommand::StartEvent,
	this, SLOT(onVtkStartEvent()));

	vtkConnections->Connect(
		m_imageLoader, vtkCommand::EndEvent,
		this, SLOT(onVtkEndEvent()));

	vtkConnections->Connect(
		m_imageLoader, vtkCommand::ProgressEvent,
		this, SLOT(onVtkProgressEvent()));

	// --- Ensure WorkflowPanelWidget is present on the left ---
	// Prefer the designer-provided widget if available (ui->workflowPanelWidget).
	// Otherwise create one and insert it into controlPanel.
	m_workflowPanelWidget = nullptr;
	if (ui->workflowPanelWidget) {
		// ui->workflowPanelWidget is created by uic if the .ui has the custom widget
		m_workflowPanelWidget = qobject_cast<WorkflowPanelWidget*>(ui->workflowPanelWidget);
	}
	if (!m_workflowPanelWidget) {
		// fallback: create and attach to controlPanel/layout
		m_workflowPanelWidget = new WorkflowPanelWidget(this);
		if (ui->controlPanelLayout) {
			// remove existing items from layout (widgets will be reparented / deleted as needed)
			QLayout* oldLayout = ui->controlPanelLayout;
			if (oldLayout) {
				QLayoutItem* item = nullptr;
				while ((item = oldLayout->takeAt(0)) != nullptr) {
					if (QWidget* w = item->widget()) {
						w->setParent(nullptr);
						delete w;
					}
					delete item;
				}
			}
			ui->controlPanelLayout->addWidget(m_workflowPanelWidget);
		}
		else if (ui->controlPanel) {
			if (QLayout* old = ui->controlPanel->layout()) {
				QLayoutItem* item = nullptr;
				while ((item = old->takeAt(0)) != nullptr) {
					if (QWidget* w = item->widget()) {
						w->setParent(nullptr);
						delete w;
					}
					delete item;
				}
				delete old;
			}
			auto* vlay = new QVBoxLayout(ui->controlPanel);
			vlay->setContentsMargins(0, 0, 0, 0);
			vlay->addWidget(m_workflowPanelWidget);
			ui->controlPanel->setLayout(vlay);
		}
		else if (this->centralWidget() && this->centralWidget()->layout()) {
			this->centralWidget()->layout()->addWidget(m_workflowPanelWidget);
		}
	}

	/*
	// VolumeRotationWidget and VolumeControlsWidget are intentionally removed from the main UI.
	// Their previous creation and wiring are commented out as the WorkflowPanelWidget now contains
	// the workflow controls and placeholders (including rotation controls).
	//
	// Example of the removed code:
	// m_volumeRotationWidget = new VolumeRotationWidget(this);
	// if (ui->volumeControlsWidget) { ... }
	// else if (m_workflowPanelWidget) { m_workflowPanelWidget->insertVolumeRotationWidget(m_volumeRotationWidget); }
	*/

	// Initialize image processing state machine
	m_processingStateMachine = new ImageProcessingStateMachine(this);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestLoadImage,
			this, &MainWindow::onProcessingRequestLoadImage);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestDefineCrop,
			this, &MainWindow::onProcessingRequestDefineCrop);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestApplyCrop,
			this, &MainWindow::onProcessingRequestApplyCrop);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestLoadCropped,
			this, &MainWindow::onProcessingRequestLoadCropped);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestPlaceFiducials,
			this, &MainWindow::onProcessingRequestPlaceFiducials);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestStartInteractiveRotation,
			this, &MainWindow::onProcessingRequestStartInteractiveRotation);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestApplyRotation,
			this, &MainWindow::onProcessingRequestApplyRotation);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestLoadRotated,
			this, &MainWindow::onProcessingRequestLoadRotated);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestComputeThreshold,
			this, &MainWindow::onProcessingRequestComputeThreshold);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestSegment,
			this, &MainWindow::onProcessingRequestSegment);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::requestSaveSegment,
			this, &MainWindow::onProcessingRequestSaveSegment);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::finished,
			this, &MainWindow::onProcessingFinished);
	connect(m_processingStateMachine, &ImageProcessingStateMachine::error,
			this, &MainWindow::onProcessingError);

	// Replace ad-hoc lambda: use the centralized updateUiForState handler.
	connect(m_processingStateMachine, &ImageProcessingStateMachine::stateChanged,
			this, &MainWindow::updateUiForState, Qt::QueuedConnection);

	// Ensure UI initially reflects the machine's starting state
	if (m_processingStateMachine) {
		updateUiForState(m_processingStateMachine->currentState());
	}

	// Note: removed local m_processingActive bookkeeping. MainWindow queries the state machine
	// via isActive()/stateChanged() if needed. Keep UI updates driven from finished/error/canceled signals.

	// Optional: respond to stateChanged for UI hints (not required, shown as example)
	connect(m_processingStateMachine, &ImageProcessingStateMachine::stateChanged, this, [this](ImageProcessingStateMachine::State s) {
		// simple UI gating: disable Open while machine is active (LoadingImage..SavingSegment)
		bool active = (s != ImageProcessingStateMachine::Idle && s != ImageProcessingStateMachine::Completed && s != ImageProcessingStateMachine::ErrorState);
		if (ui) {
			ui->actionOpen->setEnabled(!active);
			// optionally also gate other actions if desired:
			ui->actionSave->setEnabled(!active);
		}
	});

	// --- Wire workflow panel actions into state-machine-driven flow ---
	if (m_workflowPanelWidget) {
		// Load image button -> open file dialog (same as ActionOpen)
		connect(m_workflowPanelWidget, &WorkflowPanelWidget::loadImageRequested, this, &MainWindow::onActionOpen);

		// When user clicks "Define Crop" we only enable the Apply button (user must click Apply to proceed)
		connect(m_workflowPanelWidget, &WorkflowPanelWidget::defineCropRequested, this, [this]() {
			if (m_workflowPanelWidget) {
				m_workflowPanelWidget->setApplyCropEnabled(true);
				// Keep define enabled so user can re-open box if needed
			}
		});

		// When user clicks "Apply Crop" drive the state machine transition directly via QStateMachine.
		// This leverages the QState transitions instead of emitting the intermediate notify slot here.
		if (m_processingStateMachine) {
			m_processingStateMachine->addExternalTransition(
				ImageProcessingStateMachine::DefiningCrop,
				ImageProcessingStateMachine::ApplyingCrop,
				m_workflowPanelWidget,
				SIGNAL(applyCropRequested()));
		}

		// Save cropped request -> prompt for path then (TODO) start save worker
		connect(m_workflowPanelWidget, &WorkflowPanelWidget::saveCroppedRequested, this, [this]() {
			QString out = QFileDialog::getSaveFileName(this, tr("Save Cropped Volume"), "", tr("NIfTI (*.nii *.nii.gz);;All Files (*)"));
			if (out.isEmpty()) return;
			// TODO: start save worker that writes the cropped file to 'out'.
			// For now show a stub message and enable next workflow items after manual intervention.
			QMessageBox::information(this, tr("Save Cropped"), tr("Cropped volume would be saved to:\n%1\n\n(Implement save worker.)").arg(out));
			// After a real save completes, notify the state machine or directly advance:
			// m_processingStateMachine->notifyCroppedLoaded();
		});
	}

	setupPanelConnections();

	// Load settings (geometry, recent files, appearance) using JsonSettings-backed QSettings
	readSettings();

	// --- connect LightboxWidget view setting requests to MainWindow slots
	if (ui->lightboxWidget) {
		if (auto* yz = ui->lightboxWidget->getYZView())
			connect(this, &MainWindow::requestLoadViewSettings, yz, &ImageFrameWidget::readSettings, Qt::UniqueConnection);
		if (auto* xz = ui->lightboxWidget->getXZView())
			connect(this, &MainWindow::requestLoadViewSettings, xz, &ImageFrameWidget::readSettings, Qt::UniqueConnection);
		if (auto* xy = ui->lightboxWidget->getXYView())
			connect(this, &MainWindow::requestLoadViewSettings, xy, &ImageFrameWidget::readSettings, Qt::UniqueConnection);
		if (auto* vol = ui->lightboxWidget->getVolumeView())
			connect(this, &MainWindow::requestLoadViewSettings, vol, &ImageFrameWidget::readSettings, Qt::UniqueConnection);

		if (auto* yz = ui->lightboxWidget->getYZView())
			connect(this, &MainWindow::requestSaveViewSettings, yz, &ImageFrameWidget::writeSettings, Qt::UniqueConnection);
		if (auto* xz = ui->lightboxWidget->getXZView())
			connect(this, &MainWindow::requestSaveViewSettings, xz, &ImageFrameWidget::writeSettings, Qt::UniqueConnection);
		if (auto* xy = ui->lightboxWidget->getXYView())
			connect(this, &MainWindow::requestSaveViewSettings, xy, &ImageFrameWidget::writeSettings, Qt::UniqueConnection);
		if (auto* vol = ui->lightboxWidget->getVolumeView())
			connect(this, &MainWindow::requestSaveViewSettings, vol, &ImageFrameWidget::writeSettings, Qt::UniqueConnection);
	}
}

MainWindow::~MainWindow()
{
	// persist settings (geometry, recent files, appearance) before shutdown
	writeSettings();
	delete ui;
	// Note: widgets parented to 'this' or ui will be deleted automatically.
	// m_workflowPanelWidget is owned by the UI (or this) and will be deleted accordingly.
}

void MainWindow::onActionOpen()
{
	QString fileName = QFileDialog::getOpenFileName(this, tr("Open File"), "", tr("DICOM Folder (*.dcm);;ISQ Files (*.isq);;All Files (*)"));
	if (fileName.isEmpty()) return;

	openFile(fileName);
}

void MainWindow::onActionSave()
{
	QMessageBox::information(this, tr("Save"), tr("Save action triggered."));
}

void MainWindow::onActionExit()
{
	close();
}

void MainWindow::onActionAbout()
{
	// Compile-time fallbacks
#ifndef CTANALYZERX_VERSION
#define CTANALYZERX_VERSION "unknown"
#endif
#ifndef CTANALYZERX_BUILD_DATE
#define CTANALYZERX_BUILD_DATE "unknown"
#endif
#ifndef CTANALYZERX_GIT_HASH
#define CTANALYZERX_GIT_HASH "unknown"
#endif
#ifndef CTANALYZERX_BUILD_TYPE
#define CTANALYZERX_BUILD_TYPE "unknown"
#endif
#ifndef CTANALYZERX_COMPILER
#define CTANALYZERX_COMPILER "unknown"
#endif
#ifndef CTANALYZERX_VTKDICOM_VERSION
#define CTANALYZERX_VTKDICOM_VERSION "unknown"
#endif

	const QString ver = QString::fromUtf8(CTANALYZERX_VERSION).trimmed();
	const QString build = QString::fromUtf8(CTANALYZERX_BUILD_DATE).trimmed();
	const QString fullHash = QString::fromUtf8(CTANALYZERX_GIT_HASH).trimmed();
	const QString shortHash = fullHash.left(7);
	const QString buildType = QString::fromUtf8(CTANALYZERX_BUILD_TYPE).trimmed();
	const QString compiler = QString::fromUtf8(CTANALYZERX_COMPILER).trimmed();
	const QString vtkDicomVer = QString::fromUtf8(CTANALYZERX_VTKDICOM_VERSION).trimmed();

	// Platform
	const QString os = QSysInfo::prettyProductName();
	const QString arch = QSysInfo::currentCpuArchitecture();

	// Libraries
	const QString qtVer = QString::fromLatin1(QT_VERSION_STR);
	// Use vtkVersion API (same scheme as jswqAboutDialog.cxx) instead of raw macros which may differ between VTK releases
	const QString vtkVer = QString::fromLatin1(vtkVersion::GetVTKVersionFull());
	const QString itkVer = QStringLiteral("%1.%2.%3")
		.arg(QString::number(ITK_VERSION_MAJOR),
			 QString::number(ITK_VERSION_MINOR),
			 QString::number(ITK_VERSION_PATCH));

	// OpenGL summary (vendor | renderer | version)
	const QString gl = queryOpenGLSummary();

	const QString details = tr(
		"3D volume image visualization tool for DICOM and Scanco .isq files.\n\n"
		"Version:   %1\n"
		"Build:     %2\n"
		"Git:       %3\n"
		"BuildCfg:  %4\n"
		"Compiler:  %5\n"
		"OS:        %6 (%7)\n"
		"Qt:        %8\n"
		"VTK:       %9\n"
		"ITK:       %10\n"
		"VTK-DICOM: %11\n"
		"OpenGL:    %12")
		.arg(ver, build, shortHash, buildType, compiler,
			 os, arch, qtVer, vtkVer, itkVer, vtkDicomVer, gl);

	QMessageBox::about(this, tr("About CTAnalyzerX"), details);
}

void MainWindow::setupPanelConnections()
{
	// Previously this function wired VolumeControlsWidget <-> LightboxWidget signals.
	// That logic is commented out because the WorkflowPanelWidget now holds the UI
	// controls and placeholders. If needed, re-wire using m_workflowPanelWidget APIs.

	/*
	// control the volume cropping planes in the volumeview
	connect(ui->volumeControlsWidget, &VolumeControlsWidget::croppingRegionChanged,
		ui->lightboxWidget->getVolumeView(), &VolumeView::setCroppingRegion);

	// toggle volume slice planes
	connect(ui->volumeControlsWidget, &VolumeControlsWidget::slicePlaneToggle,
		ui->lightboxWidget->getVolumeView(), &VolumeView::setOrthoPlanesVisible);

	// update the range sliders when the image extents change
	connect(ui->lightboxWidget->getVolumeView(), &VolumeView::imageExtentsChanged,
		ui->volumeControlsWidget, &VolumeControlsWidget::setRangeSliders);

	// synchronize cropping enabled state when VolumeView resets it (e.g., new image)
	connect(ui->lightboxWidget->getVolumeView(), &VolumeView::croppingEnabledChanged,
		ui->volumeControlsWidget, &VolumeControlsWidget::onExternalCroppingChanged);
	*/

	// --- Window/Level controller (owned by WorkflowPanelWidget UI) ---
	if (ui->lightboxWidget) {
		WindowLevelController* wlController = nullptr;

		// Prefer the controller embedded in the WorkflowPanelWidget (uic placed one by default).
		if (m_workflowPanelWidget) {
			wlController = m_workflowPanelWidget->windowLevelController();
		}

		// Fallback: if workflow panel is not present, ask the lightbox if it created one internally.
		if (!wlController) {
			wlController = ui->lightboxWidget->windowLevelController();
		}

		// Register the controller instance with the Lightbox so it can wire propagation.
		if (wlController) {
			ui->lightboxWidget->setWindowLevelController(wlController);
		}
	}

	// Let the WorkflowPanelWidget own the live connection to the Lightbox for cropping updates.
	if (m_workflowPanelWidget && ui->lightboxWidget) {
		m_workflowPanelWidget->setLightboxWidget(ui->lightboxWidget);
	}

	// Other view-mode sync left intact where only lightbox is required
	if (ui->lightboxWidget /* && ui->volumeControlsWidget */) {
		// If you later re-add VolumeControlsWidget wiring, guard and connect here.

		// Keep WorkflowPanelWidget informed of cropping-enabled changes coming from VolumeView
		if (m_workflowPanelWidget && ui->lightboxWidget->getVolumeView()) {
			connect(ui->lightboxWidget->getVolumeView(), &VolumeView::croppingEnabledChanged,
					m_workflowPanelWidget, &WorkflowPanelWidget::setCroppingEnabled, Qt::UniqueConnection);
		}
	}
}

void MainWindow::addToRecentFiles(const QString& filePath)
{
	recentFiles.removeAll(filePath);
	recentFiles.prepend(filePath);
	while (recentFiles.size() > 10)
		recentFiles.removeLast();
	updateRecentFilesMenu();
}

void MainWindow::updateRecentFilesMenu()
{
	// Remove old recent file actions (tagged with a property)
	QList<QAction*> actions = ui->menuFile->actions();
	for (QAction* action : actions) {
		if (action->property("isRecentFile").toBool() || action->objectName() == "actionClearRecentFiles") {
			ui->menuFile->removeAction(action);
			delete action;
		}
	}

	// Find the separator after which to insert recent files (assume it's after actionScreenshot)
	QAction* insertAfter = ui->actionScreenshot;
	int insertIndex = ui->menuFile->actions().indexOf(insertAfter) + 1;

	// Insert a separator if not present
	QAction* sep = nullptr;
	if (insertIndex < ui->menuFile->actions().size() && !ui->menuFile->actions()[insertIndex]->isSeparator()) {
		sep = new QAction(this);
		sep->setSeparator(true);
		ui->menuFile->insertAction(ui->menuFile->actions().value(insertIndex), sep);
		++insertIndex;
	}
	else if (insertIndex < ui->menuFile->actions().size()) {
		sep = ui->menuFile->actions()[insertIndex];
		++insertIndex;
	}

	// Add up to 10 recent file actions, showing only the file name and tooltip for full path
	int count = 0;
	for (const QString& filePath : recentFiles) {
		if (count++ >= 10) break;
		QFileInfo info(filePath);
		QString displayName = info.fileName();
		QAction* action = new QAction(displayName, this);
		action->setProperty("isRecentFile", true);
		action->setToolTip(filePath);
		if (displayName.endsWith(".dcm", Qt::CaseInsensitive) || displayName.endsWith(".dicom", Qt::CaseInsensitive)) {
			action->setIcon(QIcon(":/icons/dicom.png")); // Provide a suitable icon resource
		}
		connect(action, &QAction::triggered, this, [this, filePath]() {
			// If no state machine is present fall back to immediate open (preserves current behavior).
			if (!m_processingStateMachine) {
				openFile(filePath);
				return;
			}

			// If the machine is idle, hand the path to it and start the workflow.
			if (!m_processingStateMachine->isActive()) {
				m_processingStateMachine->setInputFilePath(filePath);
				m_processingStateMachine->start();
				statusBar()->showMessage(tr("Queued file for processing: %1").arg(filePath), 2000);
				return;
			}

			// Machine is active — prompt user to cancel and restart (same policy as drag/drop).
			const auto resp = QMessageBox::question(this, tr("Processing in progress"),
				tr("A processing job is currently running.\n\nCancel it and start processing the selected file?\n\n%1").arg(filePath),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

			if (resp == QMessageBox::Yes) {
				// Wait for canceled() then start the new job. Use a self-disconnecting connection.
				QMetaObject::Connection* conn = new QMetaObject::Connection;
				*conn = connect(m_processingStateMachine, &ImageProcessingStateMachine::canceled, this,
					[this, filePath, conn]() {
						m_processingStateMachine->setInputFilePath(filePath);
						m_processingStateMachine->start();
						disconnect(*conn);
						delete conn;
					}, Qt::QueuedConnection);

				// Ask the machine to cancel current job; when it emits canceled() our lambda will start new job.
				m_processingStateMachine->cancel();
				statusBar()->showMessage(tr("Canceling current job and will start processing selected file..."), 2000);
				return;
			}

			// User declined — provide feedback and do nothing.
			statusBar()->showMessage(tr("Selection ignored while processing is active"), 2000);
		});
		ui->menuFile->insertAction(ui->menuFile->actions().value(insertIndex), action);
		++insertIndex;
	}

	// Add "Clear Recent Files" action if there are any recent files
	if (!recentFiles.isEmpty()) {
		QAction* clearAction = new QAction("Clear Recent Files", this);
		clearAction->setObjectName("actionClearRecentFiles");
		connect(clearAction, &QAction::triggered, this, &MainWindow::clearRecentFiles);
		ui->menuFile->insertAction(ui->menuFile->actions().value(insertIndex), clearAction);
	}
}

void MainWindow::readSettings()
{
	QSettings settings(JsonSettings::defaultSettingsPath(), JsonSettings::JsonFormat);
	if (settings.status() == QSettings::NoError) {
		// Appearance: restore geometry if valid
		settings.beginGroup("appearance");
		QRect rect;
		QRect screen = QGuiApplication::primaryScreen()->geometry();
		int xpos = settings.value("geometry_x").toInt();
		int ypos = settings.value("geometry_y").toInt();
		int w = settings.value("geometry_w").toInt();
		int h = settings.value("geometry_h").toInt();

		if (xpos <= 0 || xpos >= screen.width()) xpos = screen.x();
		if (ypos <= 0 || ypos >= screen.height()) ypos = screen.y();

		rect.setX(xpos);
		rect.setY(ypos);
		rect.setWidth(w > 0 ? w : this->width());
		rect.setHeight(h > 0 ? h : this->height());

		if (rect.isValid())
			this->setGeometry(rect);
		settings.endGroup();

		// Recent files: load list
		settings.beginGroup("recent");
		QStringList rf = settings.value("recentFiles").toStringList();
		if (!rf.isEmpty()) recentFiles = rf;
		settings.endGroup();
	}

	// Ensure UI menu reflects loaded recent files
	updateRecentFilesMenu();
}

void MainWindow::writeSettings()
{
	QSettings settings(JsonSettings::defaultSettingsPath(), JsonSettings::JsonFormat);
	if (settings.status() == QSettings::NoError) {
		// Application metadata (optional)
		settings.beginGroup("application");

		// Compile-time fallbacks (match About dialog fallbacks)
#ifndef CTANALYZERX_VERSION
#define CTANALYZERX_VERSION "unknown"
#endif
#ifndef CTANALYZERX_BUILD_DATE
#define CTANALYZERX_BUILD_DATE "unknown"
#endif
#ifndef CTANALYZERX_GIT_HASH
#define CTANALYZERX_GIT_HASH "unknown"
#endif
#ifndef CTANALYZERX_BUILD_TYPE
#define CTANALYZERX_BUILD_TYPE "unknown"
#endif
#ifndef CTANALYZERX_COMPILER
#define CTANALYZERX_COMPILER "unknown"
#endif
#ifndef CTANALYZERX_VTKDICOM_VERSION
#define CTANALYZERX_VTKDICOM_VERSION "unknown"
#endif

		const QString ver = QString::fromUtf8(CTANALYZERX_VERSION).trimmed();
		const QString build = QString::fromUtf8(CTANALYZERX_BUILD_DATE).trimmed();
		const QString fullHash = QString::fromUtf8(CTANALYZERX_GIT_HASH).trimmed();
		const QString shortHash = fullHash.left(7);
		const QString buildType = QString::fromUtf8(CTANALYZERX_BUILD_TYPE).trimmed();
		const QString compiler = QString::fromUtf8(CTANALYZERX_COMPILER).trimmed();
		const QString vtkDicomVer = QString::fromUtf8(CTANALYZERX_VTKDICOM_VERSION).trimmed();

		// Platform / libs
		const QString os = QSysInfo::prettyProductName();
		const QString arch = QSysInfo::currentCpuArchitecture();
		const QString qtVer = QString::fromLatin1(QT_VERSION_STR);
		const QString vtkVer = QString::fromLatin1(vtkVersion::GetVTKVersionFull());
		const QString itkVer = QStringLiteral("%1.%2.%3")
			.arg(QString::number(ITK_VERSION_MAJOR),
				 QString::number(ITK_VERSION_MINOR),
				 QString::number(ITK_VERSION_PATCH));

		// OpenGL summary (vendor | renderer | version)
		const QString gl = queryOpenGLSummary();

		// Persist concise about information for offline reporting
		settings.setValue("version", ver);
		settings.setValue("buildDate", build);
		settings.setValue("gitHashShort", shortHash);
		settings.setValue("buildType", buildType);
		settings.setValue("compiler", compiler);
		settings.setValue("vtkDicomVersion", vtkDicomVer);

		settings.setValue("os", os);
		settings.setValue("architecture", arch);
		settings.setValue("qtVersion", qtVer);
		settings.setValue("vtkVersion", vtkVer);
		settings.setValue("itkVersion", itkVer);
		settings.setValue("openGL", gl);

		settings.endGroup();

		// Persist recent files
		settings.beginGroup("recent");
		settings.setValue("recentFiles", recentFiles);
		settings.endGroup();

		// Persist geometry/appearance
		settings.beginGroup("appearance");
		QRect rect(this->geometry());
		settings.setValue("geometry_x", rect.x());
		settings.setValue("geometry_y", rect.y());
		settings.setValue("geometry_w", rect.width());
		settings.setValue("geometry_h", rect.height());
		settings.endGroup();

		// Ask child views to persist their own per-view groups (they use settingsGroupKey()).
		emit requestSaveViewSettings();

		settings.sync();
	}
}

void MainWindow::loadRecentFiles()
{
	// Backwards-compatible wrapper: delegate to JSON-backed settings
	readSettings();
}

void MainWindow::saveRecentFiles()
{
	// Backwards-compatible wrapper: delegate to JSON-backed settings
	writeSettings();
}

void MainWindow::clearRecentFiles()
{
	recentFiles.clear();
	updateRecentFilesMenu();
	writeSettings();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
	QMainWindow::keyPressEvent(event);
}

void MainWindow::openFile(const QString& filePath)
{
	// Use ImageLoader::CanReadFile for file type detection and existence
	if (!ImageLoader::CanReadFile(filePath)) {
		QMessageBox::warning(this, "Cannot Open File",
			QString("The selected file cannot be opened. It may not exist, is not readable, or is not a supported type (DICOM, ISQ or NIfTI).\n\nFile: %1").arg(filePath));
		return;
	}

	// Set image type based on extension (prefer explicit mapping for files)
	QString lower = filePath.toLower();
	if (lower.endsWith(".isq")) {
		m_imageLoader->SetImageType(ImageLoader::ImageType::ScancoISQ);
	}
	else if (lower.endsWith(".nii") || lower.endsWith(".nii.gz")) {
		m_imageLoader->SetImageType(ImageLoader::ImageType::NIFTI);
	}
	else if (lower.endsWith(".dcm") || lower.endsWith(".dicom")) {
		m_imageLoader->SetImageType(ImageLoader::ImageType::DICOM);
	}
	else {
		// For directories or unknown extensions, fall back to the loader's default behavior.
		QFileInfo info(filePath);
		if (info.isDir()) {
			m_imageLoader->SetImageType(ImageLoader::ImageType::DICOM);
		}
		// otherwise leave the ImageLoader::ImageType as-is (it may have been set by CanReadFile probe)
	}

	m_imageLoader->SetInputPath(filePath);

	// Try to load the image with detailed error feedback
	vtkSmartPointer<vtkImageData> vtkImage;
	try {
		m_imageLoader->Update();
		vtkImage = m_imageLoader->GetOutput();
		if (!vtkImage) {
			QMessageBox::critical(this, "Unsupported or Invalid File",
				QString("Failed to load volume. The file may be corrupted, empty, or in an unsupported format.\n\nFile: %1").arg(filePath));
			return;
		}
	}
	catch (const std::exception& ex) {
		QMessageBox::critical(this, "Error Loading File",
			QString("An error occurred while loading the file:\n%1\n\nDetails: %2")
				.arg(filePath, ex.what()));
		return;
	}
	catch (...) {
		QMessageBox::critical(this, "Unknown Error",
			QString("An unknown error occurred while loading the file:\n%1").arg(filePath));
		return;
	}

	if (vtkImage->GetDimensions()[0] <= 1 ||
		vtkImage->GetDimensions()[1] <= 1 ||
		vtkImage->GetDimensions()[2] <= 1) {
		QMessageBox::critical(this, "Invalid Volume Data",
			QString("The loaded volume has invalid dimensions and cannot be displayed.\n\nFile: %1").arg(filePath));

		ui->lightboxWidget->setDefaultImage();
		// previously: if (m_volumeRotationWidget) m_volumeRotationWidget->setOperational(false);
	}
	else {
		ui->lightboxWidget->setInputConnection(m_imageLoader->GetOutputPort(), true);

		// Update recent files list
		addToRecentFiles(filePath);
		writeSettings();
	}
}

void MainWindow::saveScreenshot()
{
	QImage screenshot = this->grab().toImage();
	QString filePath = QFileDialog::getSaveFileName(this, "Save Screenshot", "", "PNG Files (*.png);;JPEG Files (*.jpg)");
	if (!filePath.isEmpty()) {
		screenshot.save(filePath);
		QMessageBox::information(this, "Screenshot Saved", "Saved to:\n" + filePath);
	}
}

void MainWindow::onVtkStartEvent()
{
	// If we're already on the GUI thread update UI directly, otherwise queue it.
	if (QThread::currentThread() == this->thread()) {
		showLoaderStart();
		// Ensure the progress bar is painted immediately while the read is running.
		progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "showLoaderStart", Qt::QueuedConnection);
	}
}

void MainWindow::onVtkEndEvent()
{
	if (QThread::currentThread() == this->thread()) {
		showLoaderEnd();
		progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "showLoaderEnd", Qt::QueuedConnection);
	}
}

void MainWindow::onVtkProgressEvent()
{
	if (!m_imageLoader) return;
	double p = m_imageLoader->GetProgress();
	int value = static_cast<int>(std::clamp(p, 0.0, 1.0) * 100.0);

	// If caller is on GUI thread we must update directly (and pump events briefly) because the read
	// is blocking the event loop. Otherwise use a queued invocation.
	if (QThread::currentThread() == this->thread()) {
		// Throttle client-side if needed (keep it responsive), but do immediate update.
		setLoaderProgress(value);
		progressBar->update();
		// brief event pump so the widget repaints while the blocking read continues
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "setLoaderProgress", Qt::QueuedConnection, Q_ARG(int, value));
	}
}

// Accept drag if it contains a supported file
void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
	const QMimeData* mimeData = event->mimeData();
	if (mimeData->hasUrls()) {
		for (const QUrl& url : mimeData->urls()) {
			QString filePath = url.toLocalFile();
			if (ImageLoader::CanReadFile(filePath)) {
				event->acceptProposedAction();
				return;
			}
		}
	}
	event->ignore();
}

// Handle drop: hand the first supported file to the state machine.
// If a processing job is already running, prompt to cancel & restart.
void MainWindow::dropEvent(QDropEvent* event)
{
	const QMimeData* mimeData = event->mimeData();
	if (!mimeData->hasUrls()) {
		event->ignore();
		return;
	}

	QString droppedPath;
	for (const QUrl& url : mimeData->urls()) {
		QString filePath = url.toLocalFile();
		if (ImageLoader::CanReadFile(filePath)) {
			droppedPath = filePath;
			break;
		}
	}
	if (droppedPath.isEmpty()) {
		event->ignore();
		return;
	}

	// If no state machine, fall back to original behavior
	if (!m_processingStateMachine) {
		openFile(droppedPath);
		event->acceptProposedAction();
		return;
	}

	// If not active, start immediately (ask machine if it is active)
	if (!m_processingStateMachine->isActive()) {
		m_processingStateMachine->setInputFilePath(droppedPath);
		m_processingStateMachine->start();
		statusBar()->showMessage(tr("Queued file for processing: %1").arg(droppedPath), 2000);
		event->acceptProposedAction();
		return;
	}

	// Machine is active — prompt user to cancel and restart
	const auto resp = QMessageBox::question(this, tr("Processing in progress"),
		tr("A processing job is currently running.\n\nCancel it and start processing the dropped file?\n\n%1").arg(droppedPath),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

	if (resp == QMessageBox::Yes) {
		// Wait for canceled signal, then start with droppedPath.
		QMetaObject::Connection* conn = new QMetaObject::Connection;
		*conn = connect(m_processingStateMachine, &ImageProcessingStateMachine::canceled, this, [this, droppedPath, conn]() {
			// now safe to start new job
			m_processingStateMachine->setInputFilePath(droppedPath);
			m_processingStateMachine->start();
			disconnect(*conn);
			delete conn;
		}, Qt::QueuedConnection);

		// Request cancellation; machine will emit canceled() and our lambda will start new job.
		m_processingStateMachine->cancel();
		statusBar()->showMessage(tr("Canceling current job and will start processing dropped file..."), 2000);
		event->acceptProposedAction();
		return;
	}

	// User chose not to cancel — ignore or accept w/out action to indicate handled
	statusBar()->showMessage(tr("Dropped file ignored while processing is active"), 2000);
	event->acceptProposedAction();
}

void MainWindow::setLoaderProgress(int percent)
{
	progressBar->setValue(percent);
	progressBar->setVisible(true);
}

void MainWindow::showLoaderStart()
{
	progressBar->setValue(0);
	progressBar->setVisible(true);
	progressBar->setEnabled(true);
}

void MainWindow::showLoaderEnd()
{
	progressBar->setValue(100);
	progressBar->setVisible(false);
}

void MainWindow::showEvent(QShowEvent* e)
{
	QMainWindow::showEvent(e);
	emit requestLoadViewSettings();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	// Ask views to persist themselves before final write
	emit requestSaveViewSettings();
	// existing close behavior
	writeSettings();
	QMainWindow::closeEvent(event);
}


void MainWindow::onProcessingRequestLoadImage()
{
	if (!m_processingStateMachine) return;
	const QString path = m_processingStateMachine->inputFilePath();
	if (path.isEmpty()) {
		statusBar()->showMessage(tr("StateMachine: requestLoadImage (no path set)"), 3000);
		qDebug() << "ImageProcessingStateMachine requested load image but input path is empty.";

		// Ensure loader UI is hidden if no path (openFile won't be called)
		showLoaderEnd();
		return;
	}

	// Disable menu actions to avoid reentrancy while loading
	if (ui) {
		ui->actionOpen->setEnabled(false);
		ui->actionSave->setEnabled(false);
	}

	// Ensure loader UI is visible
	showLoaderStart();

	// Perform the actual load using existing helper (synchronous)
	openFile(path);

	// Inspect loader output to determine success (match checks in openFile)
	bool success = false;
	if (m_imageLoader) {
		if (auto img = m_imageLoader->GetOutput()) {
			int dims[3]; img->GetDimensions(dims);
			if (dims[0] > 1 && dims[1] > 1 && dims[2] > 1) success = true;
		}
	}

	// Stop loader UI (openFile and VTK events may already have toggled this; safe to call)
	showLoaderEnd();

	if (success) {
		// Do not manipulate WorkflowPanelWidget here — UI state is centrally driven
		// by the ImageProcessingStateMachine::stateChanged -> MainWindow::updateUiForState path.
		// Let the state machine inspect any sidecar and transition appropriately.
		if (m_processingStateMachine) {
			m_processingStateMachine->readSidecarForInput();
			m_processingStateMachine->notifyImageLoaded();
		}

		statusBar()->showMessage(tr("StateMachine: loading image '%1'").arg(path), 3000);
		qDebug() << "StateMachine: loaded image" << path;
	}
	else {
		// Failed: keep workflow UI state management in updateUiForState; report error.
		statusBar()->showMessage(tr("StateMachine: failed to load image '%1'").arg(path), 5000);
		qDebug() << "StateMachine: failed to load image" << path;
	}

	// Restore basic menu actions
	if (ui) {
		ui->actionOpen->setEnabled(true);
		ui->actionSave->setEnabled(true);
	}
}

void MainWindow::onProcessingRequestDefineCrop()
{
	// Enable/notify UI so user can define crop. Do not auto-transition here.
	statusBar()->showMessage(tr("StateMachine: please define crop (use controls on the left)"), 4000);
	qDebug() << "StateMachine requested: define crop";
	// TODO: show/raise cropping UI and wire completion to m_processingStateMachine->notifyCropDefined()
}

void MainWindow::onProcessingRequestApplyCrop()
{
	// Apply crop should start a worker that creates a cropped temporary file,
	// then call m_processingStateMachine->notifyCropApplied() or notifyFailed(...)
	statusBar()->showMessage(tr("StateMachine: applying crop (starting worker)"), 4000);
	qDebug() << "StateMachine requested: apply crop";
	// TODO: start crop worker; on completion call notifyCropApplied()/notifyFailed()
}

void MainWindow::onProcessingRequestLoadCropped()
{
	// Trigger UI/loader to load the produced cropped volume (worker should provide path)
	statusBar()->showMessage(tr("StateMachine: load cropped volume"), 3000);
	qDebug() << "StateMachine requested: load cropped";
	// TODO: load cropped file (or wait for worker to call m_processingStateMachine->notifyCroppedLoaded())
}

void MainWindow::onProcessingRequestPlaceFiducials()
{
	statusBar()->showMessage(tr("StateMachine: place fiducials"), 3000);
	qDebug() << "StateMachine requested: place fiducials";
	// TODO: enable fiducial UI and call notifyFiducialsPlaced() when done
}

void MainWindow::onProcessingRequestStartInteractiveRotation()
{
	statusBar()->showMessage(tr("StateMachine: start interactive rotation"), 3000);
	qDebug() << "StateMachine requested: start interactive rotation";
	// TODO: present rotation UI; call notifyInteractiveRotationFinished() when user completes
}

void MainWindow::onProcessingRequestApplyRotation()
{
	statusBar()->showMessage(tr("StateMachine: apply rotation (starting worker)"), 3000);
	qDebug() << "StateMachine requested: apply rotation";
	// TODO: start rotation worker; on completion call notifyRotationApplied()/notifyFailed()
}

void MainWindow::onProcessingRequestLoadRotated()
{
	statusBar()->showMessage(tr("StateMachine: load rotated image"), 3000);
	qDebug() << "StateMachine requested: load rotated";
	// TODO: load rotated file (or wait for worker to call m_processingStateMachine->notifyRotatedLoaded())
}

void MainWindow::onProcessingRequestComputeThreshold()
{
	statusBar()->showMessage(tr("StateMachine: compute threshold"), 3000);
	qDebug() << "StateMachine requested: compute threshold";
	// TODO: run threshold computation (Otsu/histogram) and call notifyThresholdComputed()
}

void MainWindow::onProcessingRequestSegment()
{
	statusBar()->showMessage(tr("StateMachine: run segmentation"), 3000);
	qDebug() << "StateMachine requested: segment";
	// TODO: run segmentation worker and call notifySegmentationDone()/notifyFailed()
}

void MainWindow::onProcessingRequestSaveSegment()
{
	statusBar()->showMessage(tr("StateMachine: save segmented volume"), 3000);
	qDebug() << "StateMachine requested: save segment";
	// TODO: prompt save dialog or start saver and call notifySaved()/notifyFailed()
}

void MainWindow::onProcessingFinished()
{
	// Stop any loader UI
	showLoaderEnd();

	// Do not directly drive WorkflowPanelWidget here. UI updates are performed by
	// updateUiForState() in response to the state machine's stateChanged() signal.
	// Keep only the generic UI cleanup (menus / status).
	if (ui) {
		ui->actionOpen->setEnabled(true);
		ui->actionSave->setEnabled(true);
	}

	statusBar()->showMessage(tr("Processing finished"), 5000);
	qDebug() << "Image processing finished.";
	// Additional UI updates after processing can be added here.
}

void MainWindow::onProcessingError(const QString& reason)
{
	// Stop loader visuals
	showLoaderEnd();

	qDebug() << "ImageProcessingStateMachine error:" << reason;
	QMessageBox::critical(this, tr("Processing Error"), reason);
	statusBar()->showMessage(tr("Processing error: %1").arg(reason), 10000);

	// Do not directly toggle WorkflowPanelWidget here. Let updateUiForState handle UI transitions.
	// Restore basic menu actions:
	if (ui) {
		ui->actionOpen->setEnabled(true);
		ui->actionSave->setEnabled(true);
	}
}

// Add this implementation near the other MainWindow helpers (below constructor for example).

void MainWindow::updateUiForState(ImageProcessingStateMachine::State s)
{
	// Helper to check if a valid image is currently loaded
	auto hasImage = [&]() -> bool {
		if (!m_imageLoader) return false;
		if (auto img = m_imageLoader->GetOutput()) {
			int d[3]; img->GetDimensions(d);
			return (d[0] > 1 && d[1] > 1 && d[2] > 1);
		}
		return false;
		};

	const bool imgPresent = hasImage();

	// Default: enable/disable top-level actions based on whether the machine is idle/completed/error
	switch (s) {
		case ImageProcessingStateMachine::Idle:
		case ImageProcessingStateMachine::Completed:
		case ImageProcessingStateMachine::ErrorState:
		if (ui) {
			ui->actionOpen->setEnabled(true);
			ui->actionSave->setEnabled(imgPresent);
		}
		// Enable/disable workflow groups depending on whether an image is present
		if (m_workflowPanelWidget) {
			m_workflowPanelWidget->applyState(s, imgPresent);
		}
		// Hide loader if idle
		showLoaderEnd();
		break;

		case ImageProcessingStateMachine::LoadingImage:
		// Block UI while loading
		if (ui) {
			ui->actionOpen->setEnabled(false);
			ui->actionSave->setEnabled(false);
		}
		if (m_workflowPanelWidget) {
			m_workflowPanelWidget->applyState(s, imgPresent);
		}
		showLoaderStart();
		break;

		case ImageProcessingStateMachine::DefiningCrop:
		// Let user interact with cropping controls only
		if (ui) {
			ui->actionOpen->setEnabled(false);
			ui->actionSave->setEnabled(false);
		}
		if (m_workflowPanelWidget) {
			m_workflowPanelWidget->applyState(s, imgPresent);
		}
		showLoaderEnd();
		break;

		case ImageProcessingStateMachine::ApplyingCrop:
		case ImageProcessingStateMachine::ApplyingRotation:
		case ImageProcessingStateMachine::ComputingThreshold:
		case ImageProcessingStateMachine::Segmenting:
		case ImageProcessingStateMachine::SavingSegment:
		// Long-running workers - disable interactive UI and show loader
		if (ui) {
			ui->actionOpen->setEnabled(false);
			ui->actionSave->setEnabled(false);
		}
		if (m_workflowPanelWidget) {
			m_workflowPanelWidget->applyState(s, imgPresent);
		}
		showLoaderStart();
		break;

		case ImageProcessingStateMachine::PlacingFiducials:
		// Enable fiducials placement UI
		if (ui) {
			ui->actionOpen->setEnabled(false);
			ui->actionSave->setEnabled(false);
		}
		if (m_workflowPanelWidget) {
			m_workflowPanelWidget->applyState(s, imgPresent);
		}
		showLoaderEnd();
		break;

		case ImageProcessingStateMachine::InteractiveRotation:
		if (ui) {
			ui->actionOpen->setEnabled(false);
			ui->actionSave->setEnabled(false);
		}
		if (m_workflowPanelWidget) {
			m_workflowPanelWidget->applyState(s, imgPresent);
		}
		showLoaderEnd();
		break;

		default:
		// conservative fallback: disable risky UI
		if (ui) {
			ui->actionOpen->setEnabled(false);
			ui->actionSave->setEnabled(false);
		}
		if (m_workflowPanelWidget) {
			m_workflowPanelWidget->applyState(s, imgPresent);
		}
		showLoaderEnd();
		break;
	}
}

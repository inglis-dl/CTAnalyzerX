#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "CropExporter.h"
#include "ImageInfoWidget.h"
#include "ImageLoader.h"
#include "JsonSettings.h"
#include "JsonUtils.h"
#include "LandmarkWidget.h"
#include "LightboxWidget.h"
#include "Logger.h"
#include "OtsuThresholdWorker.h"
#include "WindowLevelBridge.h"
#include "WindowLevelWidget.h"
#include "WorkflowPanelWidget.h"
#include "WorkflowStateMachine.h"

#include <QtConcurrent/QtConcurrent>
#include <QDateTime>
#include <QDebug>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QSurfaceFormat>
#include <QThread>

#include <vtkEventQtSlotConnect.h>
#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkVersion.h>   // VTK version macros
#include <itkVersion.h>   // ITK version macros

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
	connect(ui->actionResume, &QAction::triggered, this, &MainWindow::onActionResume);
	connect(ui->actionSave, &QAction::triggered, this, &MainWindow::onActionSave);
	connect(ui->actionExit, &QAction::triggered, this, &MainWindow::onActionExit);
	connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::onActionAbout);
	connect(ui->actionScreenshot, &QAction::triggered, this, &MainWindow::saveScreenshot);

	m_projectsMenu = ui->menuProjects;

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
	m_workflowPanelWidget = qobject_cast<WorkflowPanelWidget*>(ui->workflowPanelWidget);
	Q_ASSERT(m_workflowPanelWidget); // or qWarning() if you prefer non-fatal

	// Initialize image processing state machine
	m_workflowStateMachine = new WorkflowStateMachine(this);

	// State machine request/notification connections
	connect(m_workflowStateMachine, &WorkflowStateMachine::requestComputeThreshold,
		this, &MainWindow::onProcessingRequestComputeThreshold);
	connect(m_workflowStateMachine, &WorkflowStateMachine::requestLoadImage,
		this, &MainWindow::onProcessingRequestLoadImage);
	connect(m_workflowStateMachine, &WorkflowStateMachine::requestOpenImage,
		this, &MainWindow::onProcessingRequestOpenImage, Qt::QueuedConnection);
	connect(m_workflowStateMachine, &WorkflowStateMachine::suggestedState,
		this, &MainWindow::updateUiForState, Qt::QueuedConnection);
	connect(m_workflowStateMachine, &WorkflowStateMachine::projectLoaded,
		this, &MainWindow::onProjectLoaded, Qt::QueuedConnection);
	connect(m_workflowStateMachine, &WorkflowStateMachine::requestSaveCropped,
		this, &MainWindow::onProcessingRequestSaveCropped);
	connect(m_workflowStateMachine, &WorkflowStateMachine::requestLoadCropped,
		this, &MainWindow::onProcessingRequestLoadCropped);
	connect(m_workflowStateMachine, &WorkflowStateMachine::requestLoadLandmarks,
		this, &MainWindow::onProcessingRequestLoadLandmarks);
	connect(m_workflowStateMachine, &WorkflowStateMachine::requestSaveLandmarks,
		this, &MainWindow::onProcessingRequestSaveLandmarks);
	// High-level state change notification (for progress/status/menus only)
	connect(m_workflowStateMachine, &WorkflowStateMachine::stateChanged,
		this, &MainWindow::updateUiForState, Qt::QueuedConnection);

	// Sidecar persistence notifications
	connect(m_workflowStateMachine, &WorkflowStateMachine::sidecarWritten,
		this, [this](const QString& sidecarPath) {
			addToRecentProjects(sidecarPath);
			writeSettings();
			updateRecentProjectsMenu();
			statusBar()->showMessage(tr("Project saved: %1").arg(sidecarPath), 2000);
		}, Qt::QueuedConnection);

	connect(m_workflowStateMachine, &WorkflowStateMachine::sidecarWriteFailed,
		this, [this](const QString& imagePath, const QString& reason) {
			qWarning() << "Sidecar write failed for" << imagePath << ":" << reason;
			statusBar()->showMessage(tr("Failed to save project for %1").arg(imagePath), 4000);
		}, Qt::QueuedConnection);

	// Top-level menu action gating (Save should be disabled when machine is active)
	connect(m_workflowStateMachine, &WorkflowStateMachine::stateChanged, this,
		[this](WorkflowStateMachine::State s) {
			bool active = (s != WorkflowStateMachine::Idle &&
				s != WorkflowStateMachine::Completed &&
				s != WorkflowStateMachine::ErrorState);
			ui->actionSave->setEnabled(!active);
		});

	// Phase 4: Workflow resumption support
	connect(m_workflowStateMachine, &WorkflowStateMachine::workflowRestored,
		this, [this](WorkflowStateMachine::State restoredState) {
			statusBar()->showMessage(
				tr("Resumed workflow at: %1").arg(
					WorkflowStateMachine::stateToString(restoredState)),
				5000);

			// Notify workflow panel to highlight restored step
			if (m_workflowPanelWidget) {
				m_workflowPanelWidget->notifyWorkflowRestored(restoredState);
			}
		}, Qt::QueuedConnection);

	// Auto-save workflow state on significant transitions
	connect(m_workflowStateMachine, &WorkflowStateMachine::stateChanged,
		this, [this](WorkflowStateMachine::State newState) {
			// Save state at key workflow milestones (not during transient/loading states)
			const bool shouldPersist = (newState == WorkflowStateMachine::DefiningCrop ||
				newState == WorkflowStateMachine::DefiningLandmarks);

			if (shouldPersist && m_workflowStateMachine) {
				m_workflowStateMachine->saveWorkflowState();
			}
		}, Qt::QueuedConnection);

	// Cancel-and-restart: fired by onActionOpen when the machine is active.
	// UniqueConnection prevents duplicates; the slot reads m_pendingOpenFile directly.
	connect(m_workflowStateMachine, &WorkflowStateMachine::canceled,
		this, &MainWindow::onCancelAndStartPendingFile);

	// ========================================================================
	// Phase 3: Declarative UI property bindings
	// State machine capabilities directly drive workflow panel controls
	// ========================================================================
	if (m_workflowStateMachine && m_workflowPanelWidget) {
		// Cropping UI bindings
		connect(m_workflowStateMachine, &WorkflowStateMachine::canDefineCropChanged,
			m_workflowPanelWidget, &WorkflowPanelWidget::setCroppingEnabled, Qt::QueuedConnection);

		connect(m_workflowStateMachine, &WorkflowStateMachine::canSaveCropChanged,
			m_workflowPanelWidget, &WorkflowPanelWidget::setSaveCroppedEnabled, Qt::QueuedConnection);

		// Landmarks UI bindings
		connect(m_workflowStateMachine, &WorkflowStateMachine::canPlaceLandmarksChanged,
			m_workflowPanelWidget, &WorkflowPanelWidget::setLandmarkingEnabled, Qt::QueuedConnection);

		auto* landmarkWidget = m_workflowPanelWidget->landmarkWidget();
		if (landmarkWidget) {
			// Route save request directly to the MainWindow executor slot
			connect(landmarkWidget, &LandmarkWidget::saveLandmarksRequested,
				this, &MainWindow::onProcessingRequestSaveLandmarks, Qt::QueuedConnection);

			qDebug() << "Connected LandmarkWidget save request to state machine";
		}

		// Initialize UI to match state machine's initial capabilities
		m_workflowPanelWidget->setCroppingEnabled(m_workflowStateMachine->canDefineCrop());
		m_workflowPanelWidget->setSaveCroppedEnabled(m_workflowStateMachine->canSaveCrop());
		m_workflowPanelWidget->setLandmarkingEnabled(m_workflowStateMachine->canPlaceLandmarks());

		// Appearance group is always available (not state-machine managed)
		m_workflowPanelWidget->setWindowLevellingEnabled(true);
	}

	// Ensure UI initially reflects the machine's starting state (for progress/status only)
	updateUiForState(m_workflowStateMachine->currentState());

	// --- Wire workflow panel actions into state-machine-driven flow ---
	if (m_workflowPanelWidget) {
		// Save cropped request -> trigger export worker
		connect(m_workflowPanelWidget, &WorkflowPanelWidget::saveCroppedRequested, this, [this]() {
			if (!m_cropExporter) {
				statusBar()->showMessage(tr("No crop exporter available"), 5000);
				return;
			}
			m_cropExporter->apply();
			});
	}

	// Instantiate CropExporter (stateless w.r.t. stakeholders). MainWindow performs the wiring.
	if (!m_cropExporter) {
		m_cropExporter = new CropExporter(this);

		// Panel -> exporter: extents only (Apply removed; Save triggers export)
		if (m_workflowPanelWidget) {
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::croppingRegionChanged,
				m_cropExporter, &CropExporter::setCropRegion, Qt::UniqueConnection);
		}

		// MainWindow hooks: show loader/progress like ImageLoader
		connect(m_cropExporter, &CropExporter::writeStarted,
			this, &MainWindow::showProgressStart, Qt::QueuedConnection);
		connect(m_cropExporter, &CropExporter::writeProgress,
			this, &MainWindow::showProgressValue, Qt::QueuedConnection);
		connect(m_cropExporter, &CropExporter::writeFinished,
			this, [this](const QString& path, bool success, const QString& msg) {
				this->showProgressEnd();

				if (success && !path.isEmpty()) {
					addToRecentFiles(path);
					writeSettings();
					statusBar()->showMessage(tr("Saved cropped volume: %1").arg(path), 8000);

					// Use the proper helper so the state machine is notified correctly.
					// croppedLoaded() will be triggered inside openAndNotifyImageLoaded
					// when it detects the derived path.
					if (m_workflowStateMachine) {
						m_workflowStateMachine->cropApplied();
					}
					openAndNotifyImageLoaded(path, /*showProgress=*/false);
				}
				else {
					statusBar()->showMessage(tr("Crop write failed: %1").arg(msg), 6000);
				}
			}, Qt::QueuedConnection);

		// Forward sidecar requests to WorkflowStateMachine
		if (m_workflowStateMachine) {
			connect(m_cropExporter, &CropExporter::sidecarUpdateRequested,
				m_workflowStateMachine, &WorkflowStateMachine::writeCropSidecarForOutput,
				Qt::QueuedConnection);
		}
	}

	setupPanelConnections();

	// Connect ImageLoader's JSON metadata emitter to the ImageInfoWidget
	if (m_imageLoader && m_workflowPanelWidget && m_workflowPanelWidget->imageInfoWidget()) {
		if (auto emitter = m_imageLoader->metaEmitter()) {
			connect(emitter, &ImageLoaderMetaEmitter::metaUpdated,
				m_workflowPanelWidget->imageInfoWidget(), &ImageInfoWidget::updateFromMeta, Qt::QueuedConnection);
		}
	}

	// Connect LightboxWidget's default-image metaReady signal to ImageInfoWidget
	if (ui->lightboxWidget && m_workflowPanelWidget && m_workflowPanelWidget->imageInfoWidget()) {
		connect(ui->lightboxWidget, &LightboxWidget::metaReady,
			m_workflowPanelWidget->imageInfoWidget(), &ImageInfoWidget::updateFromMeta, Qt::QueuedConnection);
	}

	// Load settings (geometry, recent files, appearance)
	readSettings();

	// --- connect LightboxWidget view setting requests to MainWindow slots
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
	const QString fileName = QFileDialog::getOpenFileName(
		this, tr("Open File"), {},
		tr("DICOM Folder (*.dcm);;ISQ Files (*.isq);;NIfTI Files (*.nii *.nii.gz);;All Files (*)"));

	if (fileName.isEmpty())
		return;

	if (!m_workflowStateMachine)
	{
		openFile(fileName);   // no state machine: bare load
		return;
	}

	if (!m_workflowStateMachine->isWorkflowActive())
	{
		m_workflowStateMachine->setInputFilePath(fileName);
		m_workflowStateMachine->start();
		statusBar()->showMessage(tr("Opening: %1").arg(fileName), 2000);
		return;
	}

	// Machine is active: stage the file and request a cancel.
	// onCancelAndStartPendingFile (connected in ctor) will pick up m_pendingOpenFile
	// when canceled() fires and start the new workflow.
	m_pendingOpenFile = fileName;
	m_workflowStateMachine->cancel();
	statusBar()->showMessage(tr("Canceling current job…"), 2000);
}

void MainWindow::onActionResume()
{
	QString projectPath = QFileDialog::getOpenFileName(
		this,
		tr("Select Project to Resume"),
		QString(),
		tr("Project Files (*.json);;All Files (*)"));

	if (projectPath.isEmpty()) {
		return; // User canceled
	}

	resumeProjectWorkflow(projectPath);
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
	// --- Window/Level controller (owned by WorkflowPanelWidget UI) ---

	WindowLevelWidget* wlController = nullptr;

	// Prefer the controller embedded in the WorkflowPanelWidget (uic placed one by default).
	if (m_workflowPanelWidget) {
		wlController = m_workflowPanelWidget->windowLevelWidget();
	}

	// Fallback: if workflow panel is not present, ask the lightbox if it created one internally.
	if (!wlController) {
		wlController = ui->lightboxWidget->windowLevelWidget();
	}

	// Register the controller instance with the Lightbox so it can wire propagation.
	if (wlController) {
		ui->lightboxWidget->setWindowLevelWidget(wlController);

		// Ensure the controller participates in global load/save settings hooks
		connect(this, &MainWindow::requestLoadViewSettings, wlController, &WindowLevelWidget::readSettings, Qt::UniqueConnection);
		connect(this, &MainWindow::requestSaveViewSettings, wlController, &WindowLevelWidget::writeSettings, Qt::UniqueConnection);

		// Load controller settings immediately so UI reflects persisted state early
		wlController->readSettings();
	}

	if (m_workflowStateMachine && wlController) {
		if (ScalarOpacityFunctionWidget* scalarWidget = wlController->scalarOpacityFunctionWidget()) {
			connect(m_workflowStateMachine, &WorkflowStateMachine::thresholdChanged,
				this, [scalarWidget](bool present, double value) {
					// If sidecar contains a threshold, set it on the widget and show the marker.
					// Otherwise hide the marker. parseOtsuThreshold guarantees `value` is valid when `present` is true.
					if (present) {
						scalarWidget->setHistogramThreshold(value);
					}
					scalarWidget->setShowThresholdIndicator(present);
				}, Qt::QueuedConnection);
		}
	}

	// Let the WorkflowPanelWidget own the live connection to the Lightbox for cropping updates.
	if (m_workflowPanelWidget && ui->lightboxWidget) {
		m_workflowPanelWidget->setLightboxWidget(ui->lightboxWidget);

		// XY slice
		if (auto* xy = ui->lightboxWidget->getXYView()) {
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::defineCropRequested,
				xy, [xy]() { xy->setOutlineVisible(true); }, Qt::UniqueConnection);
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::saveCroppedRequested,
				xy, [xy]() { xy->setOutlineVisible(false); }, Qt::UniqueConnection);
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::croppingRegionChanged,
				xy, &SliceView::setCroppingRegion, Qt::UniqueConnection);
		}
		// XZ slice
		if (auto* xz = ui->lightboxWidget->getXZView()) {
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::defineCropRequested,
				xz, [xz]() { xz->setOutlineVisible(true); }, Qt::UniqueConnection);
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::saveCroppedRequested,
				xz, [xz]() { xz->setOutlineVisible(false); }, Qt::UniqueConnection);
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::croppingRegionChanged,
				xz, &SliceView::setCroppingRegion, Qt::UniqueConnection);
		}
		// YZ slice
		if (auto* yz = ui->lightboxWidget->getYZView()) {
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::defineCropRequested,
				yz, [yz]() { yz->setOutlineVisible(true); }, Qt::UniqueConnection);
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::saveCroppedRequested,
				yz, [yz]() { yz->setOutlineVisible(false); }, Qt::UniqueConnection);
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::croppingRegionChanged,
				yz, &SliceView::setCroppingRegion, Qt::UniqueConnection);
		}

		if (auto* vol = ui->lightboxWidget->getVolumeView()) {
			connect(vol, &VolumeView::croppingEnabledChanged,
				m_workflowPanelWidget, &WorkflowPanelWidget::setCroppingEnabled, Qt::UniqueConnection);

			// Show outline when user enters "define crop" mode (panel-level request)
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::defineCropRequested,
				vol, [vol]() { vol->setOutlineVisible(true); }, Qt::UniqueConnection);

			// Hide outline when user requests Save (crop completed / save initiated)
			connect(m_workflowPanelWidget, &WorkflowPanelWidget::saveCroppedRequested,
				vol, [vol]() { vol->setOutlineVisible(false); }, Qt::UniqueConnection);
		}
	}

	if (m_workflowPanelWidget && m_workflowStateMachine) {
		connect(m_workflowPanelWidget, &WorkflowPanelWidget::resetCropRequested,
			m_workflowStateMachine, &WorkflowStateMachine::cropReset,
			Qt::QueuedConnection);
	}
}

void MainWindow::addToRecentFiles(const QString& filePath)
{
	// Guard: skip workflow-derived cropped volumes generated by CropExporter::makeAutoOutputPath.
	// CropExporter produces names like "<base>_crop_<xdim>x<ydim>x<zdim>.nii".
	// Match on the complete base name to avoid relying on timestamps or other variants.
	QFileInfo fi(filePath);
	const QString baseName = fi.completeBaseName();

	static const QRegularExpression cropPattern(R"(_crop_\d+x\d+x\d+)", QRegularExpression::CaseInsensitiveOption);
	if (cropPattern.match(baseName).hasMatch()) {
		qDebug() << "Skipping workflow-derived cropped file for recents:" << filePath;
		return;
	}

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
			if (!m_workflowStateMachine) {
				openFile(filePath);
				return;
			}

			// If the machine is idle, hand the path to it and start the workflow.
			if (!m_workflowStateMachine->isWorkflowActive()) {
				m_workflowStateMachine->setInputFilePath(filePath);
				m_workflowStateMachine->start();
				statusBar()->showMessage(tr("Queued file for processing: %1").arg(filePath), 2000);
				return;
			}

			// Machine is active — prompt user to cancel and restart (same policy as drag/drop).
			const auto resp = QMessageBox::question(this, tr("Processing in progress"),
				tr("A processing job is currently running.\n\nCancel it and start processing the selected file?\n\n%1").arg(filePath),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

			if (resp == QMessageBox::Yes) {
				// Wait for canceled() then start the new job. Use a self-disconnecting connection.
				auto conn = QSharedPointer<QMetaObject::Connection>::create();
				*conn = connect(m_workflowStateMachine, &WorkflowStateMachine::canceled, this,
					[this, filePath, conn]() {
						m_workflowStateMachine->setInputFilePath(filePath);
						m_workflowStateMachine->start();
						disconnect(*conn);
					}, Qt::QueuedConnection);

				// Ask the machine to cancel current job; when it emits canceled() our lambda will start new job.
				m_workflowStateMachine->cancel();
				statusBar()->showMessage(tr("Canceling current job and will start processing selected file..."), 2000);
				return;
			}

			// User declined: provide feedback and do nothing.
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

		// Recent files & projects: load lists (we will verify them once on first show)
		settings.beginGroup("recent");
		QStringList rf = settings.value("recentFiles").toStringList();
		if (!rf.isEmpty()) recentFiles = rf;
		QStringList rp = settings.value("recentProjects").toStringList();
		if (!rp.isEmpty()) recentProjects = rp;
		settings.endGroup();

		// Logging preferences (optional group)
		settings.beginGroup("logging");
		const bool rotateEnabled = settings.value("rotateEnabled", Logger::rotateEnabled()).toBool();
		const int maxBackups = settings.value("maxBackupFiles", Logger::maxBackupFiles()).toInt();
		const int maxFileSizeMB = settings.value("maxFileSizeMB", Logger::maxFileSizeMB()).toInt();
		settings.endGroup();

		// Apply to Logger runtime config (should be done before Logger::install/openLog if possible)
		Logger::setRotateEnabled(rotateEnabled);
		Logger::setMaxBackupFiles(maxBackups);
		Logger::setMaxFileSizeMB(maxFileSizeMB);
	}

	// Ensure UI menu reflects loaded recent files
	updateRecentFilesMenu();
	updateRecentProjectsMenu();
}

void MainWindow::writeSettings()
{
	// Ask child views to persist their own groups first so their writes appear in the file.
	// Slots connected to requestSaveViewSettings write synchronously, so emit + processEvents
	// ensures their work is completed before we read/merge the file.
	emit requestSaveViewSettings();
	QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

	const QString path = JsonSettings::defaultSettingsPath();

	// Load existing JSON (if any) so we can merge our values without clobbering other groups.
	QJsonObject root;
	QFile in(path);
	if (in.open(QIODevice::ReadOnly)) {
		const QByteArray data = in.readAll();
		in.close();
		const QJsonDocument doc = QJsonDocument::fromJson(data);
		if (doc.isObject()) root = doc.object();
	}

	// Application metadata (overwrite/ensure these keys)
	QJsonObject app;
	app.insert(QStringLiteral("version"), QString::fromUtf8(CTANALYZERX_VERSION).trimmed());
	app.insert(QStringLiteral("buildDate"), QString::fromUtf8(CTANALYZERX_BUILD_DATE).trimmed());
	const QString fullHash = QString::fromUtf8(CTANALYZERX_GIT_HASH).trimmed();
	app.insert(QStringLiteral("gitHashShort"), fullHash.left(7));
	app.insert(QStringLiteral("buildType"), QString::fromUtf8(CTANALYZERX_BUILD_TYPE).trimmed());
	app.insert(QStringLiteral("compiler"), QString::fromUtf8(CTANALYZERX_COMPILER).trimmed());
	app.insert(QStringLiteral("vtkDicomVersion"), QString::fromUtf8(CTANALYZERX_VTKDICOM_VERSION).trimmed());
	app.insert(QStringLiteral("os"), QSysInfo::prettyProductName());
	app.insert(QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture());
	app.insert(QStringLiteral("qtVersion"), QString::fromLatin1(QT_VERSION_STR));
	app.insert(QStringLiteral("vtkVersion"), QString::fromLatin1(vtkVersion::GetVTKVersionFull()));
	app.insert(QStringLiteral("itkVersion"),
		QStringLiteral("%1.%2.%3")
		.arg(QString::number(ITK_VERSION_MAJOR),
			QString::number(ITK_VERSION_MINOR),
			QString::number(ITK_VERSION_PATCH)));
	// Query OpenGL / GPU parameters and persist them under application -> opengl.
	QJsonObject opengl;

	// 1) Try to get a render window from any ImageFrameWidget child so we can make the GL context current.
	vtkGenericOpenGLRenderWindow* grw = nullptr;
	const auto frames = this->findChildren<ImageFrameWidget*>();
	for (ImageFrameWidget* f : frames) {
		if (!f) continue;
		vtkGenericOpenGLRenderWindow* candidate = vtkGenericOpenGLRenderWindow::SafeDownCast(f->renderWindow());
		if (candidate) {
			grw = candidate;
			break;
		}
	}

	// If we have a render window, make its context current so glGetString / glGetIntegerv work.
	if (grw) {
		// MakeCurrent is safe here; we only use it if a render window exists.
		grw->MakeCurrent();
		// Query GL strings if context is available
#if defined(GL_VENDOR) && defined(GL_RENDERER) && defined(GL_VERSION)
		const GLubyte* gv = glGetString(GL_VENDOR);
		const GLubyte* gr = glGetString(GL_RENDERER);
		const GLubyte* gvrs = glGetString(GL_VERSION);
		if (gv) opengl.insert(QStringLiteral("vendor"), QString::fromUtf8(reinterpret_cast<const char*>(gv)));
		if (gr) opengl.insert(QStringLiteral("renderer"), QString::fromUtf8(reinterpret_cast<const char*>(gr)));
		if (gvrs) opengl.insert(QStringLiteral("version"), QString::fromUtf8(reinterpret_cast<const char*>(gvrs)));
#endif
		// Query some GL limits as fallback (if available)
#if defined(GL_MAX_TEXTURE_SIZE)
		GLint maxTex = 0;
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
		if (maxTex > 0) opengl.insert(QStringLiteral("maxTextureSize"), maxTex);
#endif
#if defined(GL_MAX_3D_TEXTURE_SIZE)
		GLint max3D = 0;
		glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &max3D);
		if (max3D > 0) opengl.insert(QStringLiteral("max3DTextureSize"), max3D);
#endif
	}

	app.insert(QStringLiteral("openGL"), opengl);

	root.insert(QStringLiteral("application"), app);

	// Recent lists (overwrite the recent group, but keep other top-level keys intact)
	QJsonObject recent;
	QJsonArray rf;
	for (const QString& s : recentFiles) rf.append(s);
	QJsonArray rp;
	for (const QString& p : recentProjects) rp.append(p);
	recent.insert(QStringLiteral("recentFiles"), rf);
	recent.insert(QStringLiteral("recentProjects"), rp);
	root.insert(QStringLiteral("recent"), recent);

	// Appearance / geometry
	QRect rect(this->geometry());
	QJsonObject appearance;
	appearance.insert(QStringLiteral("geometry_x"), rect.x());
	appearance.insert(QStringLiteral("geometry_y"), rect.y());
	appearance.insert(QStringLiteral("geometry_w"), rect.width());
	appearance.insert(QStringLiteral("geometry_h"), rect.height());
	root.insert(QStringLiteral("appearance"), appearance);

	// Persist logging configuration under "logging"
	QJsonObject logging;
	logging.insert(QStringLiteral("rotateEnabled"), Logger::rotateEnabled());
	logging.insert(QStringLiteral("maxBackupFiles"), Logger::maxBackupFiles());
	logging.insert(QStringLiteral("maxFileSizeMB"), Logger::maxFileSizeMB());
	root.insert(QStringLiteral("logging"), logging);

	// Write merged JSON back to disk atomically
	QSaveFile out(path);
	if (!out.open(QIODevice::WriteOnly)) {
		qWarning() << "writeSettings: failed to open settings file for write:" << path;
		return;
	}
	const QJsonDocument outDoc(root);
	out.write(outDoc.toJson(QJsonDocument::Indented));
	if (!out.commit()) {
		qWarning() << "writeSettings: failed to commit settings file:" << path;
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
	}
	else {
		ui->lightboxWidget->setInputConnection(m_imageLoader->GetOutputPort(), true);

		// Update recent files list
		addToRecentFiles(filePath);
		writeSettings();

		// Inform exporter about input pipeline and path so it can auto-generate output names later.
		if (m_cropExporter && m_imageLoader) {
			m_cropExporter->setInputConnection(m_imageLoader->GetOutputPort());
			m_cropExporter->setInputFilePath(filePath);
		}
	}
}

void MainWindow::onCancelAndStartPendingFile()
{
	// Guard: only act when a file was deliberately staged by onActionOpen.
	// canceled() can also fire from dropEvent / resumeProjectWorkflow paths;
	// those paths do not set m_pendingOpenFile, so we silently return.
	if (m_pendingOpenFile.isEmpty())
		return;

	const QString filePath = m_pendingOpenFile;
	m_pendingOpenFile.clear();

	if (!m_workflowStateMachine)
		return;

	m_workflowStateMachine->setInputFilePath(filePath);
	m_workflowStateMachine->start();
	statusBar()->showMessage(tr("Opening: %1").arg(filePath), 2000);
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
		showProgressStart();
		// Ensure the progress bar is painted immediately while the read is running.
		progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "showProgressStart", Qt::QueuedConnection);
	}
}

void MainWindow::onVtkEndEvent()
{
	if (QThread::currentThread() == this->thread()) {
		showProgressEnd();
		progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "showProgressEnd", Qt::QueuedConnection);
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
		showProgressValue(value);
		progressBar->update();
		// brief event pump so the widget repaints while the blocking read continues
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "showProgressValue", Qt::QueuedConnection, Q_ARG(int, value));
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
	if (!m_workflowStateMachine) {
		openFile(droppedPath);
		event->acceptProposedAction();
		return;
	}

	// If not active, start immediately (ask machine if it is active)
	if (!m_workflowStateMachine->isWorkflowActive()) {
		m_workflowStateMachine->setInputFilePath(droppedPath);
		m_workflowStateMachine->start();
		statusBar()->showMessage(tr("Queued file for processing: %1").arg(droppedPath), 2000);
		event->acceptProposedAction();
		return;
	}

	// Machine is active — prompt user to cancel and restart
	const auto resp = QMessageBox::question(this, tr("Processing in progress"),
		tr("A processing job is currently running.\n\nCancel it and start processing the dropped file?\n\n%1").arg(droppedPath),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

	if (resp == QMessageBox::Yes) {
		// Wait for canceled() then start the new job. Use a self-disconnecting connection.
		auto conn = QSharedPointer<QMetaObject::Connection>::create();
		*conn = connect(m_workflowStateMachine, &WorkflowStateMachine::canceled, this, [this, droppedPath, conn]() {
			// now safe to start new job
			m_workflowStateMachine->setInputFilePath(droppedPath);
			m_workflowStateMachine->start();
			disconnect(*conn);
		}, Qt::QueuedConnection);

		// Request cancellation; machine will emit canceled() and our lambda will start new job.
		m_workflowStateMachine->cancel();
		statusBar()->showMessage(tr("Canceling current job and will start processing dropped file..."), 2000);
		event->acceptProposedAction();
		return;
	}

	// User chose not to cancel — ignore or accept w/out action to indicate handled
	statusBar()->showMessage(tr("Dropped file ignored while processing is active"), 2000);
	event->acceptProposedAction();
}

void MainWindow::showProgressValue(int percent)
{
	progressBar->setValue(percent);
	progressBar->setVisible(true);
}

void MainWindow::showProgressStart()
{
	progressBar->setValue(0);
	progressBar->setVisible(true);
	progressBar->setEnabled(true);
}

void MainWindow::showProgressEnd()
{
	progressBar->setValue(100);
	progressBar->setVisible(false);
}

void MainWindow::showEvent(QShowEvent* e)
{
	QMainWindow::showEvent(e);
	verifySettings();
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

void MainWindow::onProcessingRequestSaveCropped()
{
	// Automatic save requested by the state machine.
	if (!m_workflowStateMachine) return;
	if (!m_cropExporter) {
		statusBar()->showMessage(tr("No crop exporter available"), 5000);
		return;
	}

	const QString inputPath = m_workflowStateMachine->inputFilePath();
	if (inputPath.isEmpty()) {
		statusBar()->showMessage(tr("StateMachine requested save but no input path is set"), 5000);
		return;
	}

	// The CropExporter will auto-generate an output filename based on the input file path.
	// Show loader and call apply() synchronously; CropExporter emits progress/finished signals.
	showProgressStart();
	m_cropExporter->apply();
	// writeFinished handler (connected earlier) will hide the loader and handle post-save actions.
}

void MainWindow::onProcessingRequestComputeThreshold()
{
	if (!m_imageLoader || !m_imageLoader->GetOutput()) {
		qWarning() << "No image loaded for threshold computation";
		statusBar()->showMessage(tr("Cannot compute threshold: no image loaded"), 3000);
		return;
	}

	vtkImageData* vtkImg = m_imageLoader->GetOutput();

	// Create worker + thread
	OtsuThresholdWorker* worker = new OtsuThresholdWorker(this); // temporary parent
	QThread* workerThread = new QThread(this);
	worker->moveToThread(workerThread);

	// Start compute when thread starts
	connect(workerThread, &QThread::started, worker, [worker, vtkImg]() {
		worker->compute(vtkImg, nullptr);
	}, Qt::QueuedConnection);

	// Handle successful computation -> append to sidecar
	connect(worker, &OtsuThresholdWorker::computeFinished, this,
		[this](bool ok, double threshold) {
				if (!m_workflowStateMachine) return;

				if (ok) {
					// NEW: let the state machine persist threshold in new canonical keys
					m_workflowStateMachine->onThresholdComputed(threshold, QStringLiteral("otsu"));

					statusBar()->showMessage(
						tr("Computed threshold: %1").arg(threshold),
						3000
					);
				}
				else {
					statusBar()->showMessage(
						tr("Threshold computation returned invalid result"),
						3000
					);
					// Optional later: set up manual threshold UI. For now, fail loudly.
					m_workflowStateMachine->failed(QStringLiteral("Otsu threshold failed"));
				}
		}, Qt::QueuedConnection);

	// Progress handling
	connect(worker, &OtsuThresholdWorker::computeStarted,
			this, &MainWindow::showProgressStart, Qt::QueuedConnection);

	connect(worker, &OtsuThresholdWorker::computeProgress,
			this, &MainWindow::showProgressValue, Qt::QueuedConnection);

	connect(worker, &OtsuThresholdWorker::computeFinished,
			this, &MainWindow::showProgressEnd, Qt::QueuedConnection);

	// Error handling
	connect(worker, &OtsuThresholdWorker::computeError, this,
		[this](const QString& reason) {
			qWarning() << "OtsuThresholdWorker error:" << reason;
			this->showProgressEnd();
			statusBar()->showMessage(
				tr("Threshold compute error: %1").arg(reason),
				5000
			);
		}, Qt::QueuedConnection);

	connect(worker, &OtsuThresholdWorker::computeCanceled, this,
		[this]() {
			this->showProgressEnd();
			statusBar()->showMessage(
				tr("Threshold computation canceled"),
				2000
			);
		}, Qt::QueuedConnection);

	// Thread lifecycle
	connect(worker, &OtsuThresholdWorker::computeFinished,
			workerThread, &QThread::quit, Qt::QueuedConnection);

	connect(worker, &OtsuThresholdWorker::computeError,
			workerThread, &QThread::quit, Qt::QueuedConnection);

	connect(worker, &OtsuThresholdWorker::computeCanceled,
			workerThread, &QThread::quit, Qt::QueuedConnection);

	// Cleanup
	connect(workerThread, &QThread::finished,
			worker, &QObject::deleteLater, Qt::QueuedConnection);

	connect(workerThread, &QThread::finished,
			workerThread, &QObject::deleteLater, Qt::QueuedConnection);

	// Start worker thread
	workerThread->start();

	qDebug() << "Started Otsu threshold computation";
}

void MainWindow::updateUiForState(WorkflowStateMachine::State s)
{
	auto hasImage = [&]() -> bool {
		if (!m_imageLoader) return false;
		if (auto img = m_imageLoader->GetOutput()) {
			int d[3];
			img->GetDimensions(d);
			return (d[0] > 1 && d[1] > 1 && d[2] > 1);
		}
		return false;
		};

	const bool imgPresent = hasImage();
	const bool isIdle = (s == WorkflowStateMachine::Idle ||
						 s == WorkflowStateMachine::Completed ||
						 s == WorkflowStateMachine::ErrorState);

	ui->actionSave->setEnabled(imgPresent && isIdle);

	// Progress indicator
	const bool showProgress = (s == WorkflowStateMachine::LoadingImage ||
	 //s == WorkflowStateMachine::LoadingSidecar ||  // consider showing progress for sidecar load if it becomes a bottleneck
	 //s == WorkflowStateMachine::ComputingThreshold || // consider showing progress for threshold compute if it becomes a bottleneck
							   s == WorkflowStateMachine::LoadingCropped);

	if (showProgress) {
		showProgressStart();
	}
	else {
		showProgressEnd();
	}

	// Status message
	QString statusMsg = tr("Workflow: %1").arg(WorkflowStateMachine::stateToString(s));
	statusBar()->showMessage(statusMsg, 2500);
}

// One-time verification of recent caches invoked when the main window is first shown.
void MainWindow::verifySettings()
{
	if (m_settingsVerified) return;
	m_settingsVerified = true;

	// Prune invalid entries and persist cleaned lists if necessary.
	verifyRecentFiles();
	verifyRecentProjects();
}

void MainWindow::verifyRecentFiles()
{
	bool changed = false;
	if (!recentFiles.isEmpty()) {
		QStringList kept;
		for (const QString& path : recentFiles) {
			// Accept directories and files if they exist and the loader can read them.
			if (QFile::exists(path) || QFileInfo(path).isDir()) {
				if (ImageLoader::CanReadFile(path)) {
					kept.append(path);
					continue;
				}
			}
			// drop invalid / missing entry
			changed = true;
		}
		recentFiles = kept;
	}

	if (changed) {
		writeSettings(); // persist cleaned list

	}
	// Refresh menu to reflect cleaned entries
	updateRecentFilesMenu();
}

void MainWindow::verifyRecentProjects()
{
	bool changed = false;
	if (!recentProjects.isEmpty()) {
		QStringList kept;
		for (const QString& projPath : recentProjects) {
			// Check file exists
			if (!QFile::exists(projPath)) {
				qWarning() << "Removing missing project from recents:" << projPath;
				changed = true;
				continue;
			}

			// Check file is readable
			QFile f(projPath);
			if (!f.open(QIODevice::ReadOnly)) {
				qWarning() << "Removing unreadable project from recents:" << projPath;
				changed = true;
				continue;
			}

			// Validate JSON structure
			const QByteArray data = f.readAll();
			f.close();
			const QJsonDocument doc = QJsonDocument::fromJson(data);
			if (!doc.isObject()) {
				qWarning() << "Removing malformed project from recents:" << projPath;
				changed = true;
				continue;
			}

			// Validate it's a valid sidecar (has required "inputImage" key)
			QJsonObject root = doc.object();
			if (!root.contains("source")) {
				qWarning() << "Removing invalid sidecar from recents (missing 'source'):" << projPath;
				changed = true;
				continue;
			}

			// All checks passed - keep this entry
			kept.append(projPath);
		}
		recentProjects = kept;
	}

	if (changed) {
		writeSettings();
	}

	// Refresh Projects menu to reflect cleaned entries
	updateRecentProjectsMenu();
}

void MainWindow::updateRecentProjectsMenu()
{
	// Remove previous actions
	QList<QAction*> old = m_projectsMenu->actions();
	for (QAction* a : old) {
		m_projectsMenu->removeAction(a);
		delete a;
	}

	// Add recent projects (up to 10)
	int count = 0;
	for (const QString& proj : recentProjects) {
		if (count++ >= 10) break;

		QFileInfo fi(proj);
		QString display = fi.fileName();
		QAction* act = new QAction(display, this);
		act->setToolTip(proj);
		act->setProperty("isRecentProject", true);

		connect(act, &QAction::triggered, this, [this, proj]() {
			resumeProjectWorkflow(proj);
			});

		m_projectsMenu->addAction(act);
	}

	// Add clear or placeholder
	if (!recentProjects.isEmpty()) {
		m_projectsMenu->addSeparator();
		QAction* clear = new QAction(tr("Clear recent projects"), this);
		clear->setObjectName("actionClearRecentProjects");
		connect(clear, &QAction::triggered, this, &MainWindow::clearRecentProjects);
		m_projectsMenu->addAction(clear);
	}
}

void MainWindow::clearRecentProjects()
{
	recentProjects.clear();
	updateRecentProjectsMenu();
	writeSettings();
	statusBar()->showMessage(tr("Cleared recent projects"), 2000);
}

void MainWindow::addToRecentProjects(const QString& projectPath)
{
	if (projectPath.isEmpty()) return;

	// Validate file exists
	if (!QFile::exists(projectPath)) {
		qWarning() << "Cannot add non-existent project to recents:" << projectPath;
		return;
	}

	// Validate file is readable and contains valid JSON
	QFile f(projectPath);
	if (!f.open(QIODevice::ReadOnly)) {
		qWarning() << "Cannot read project file:" << projectPath;
		return;
	}

	const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
	f.close();

	if (!doc.isObject()) {
		qWarning() << "Invalid JSON project file:" << projectPath;
		return;
	}

	// Validate it's a valid sidecar (has required "inputImage" key)
	QJsonObject root = doc.object();
	if (!root.contains("source")) {
		qWarning() << "Project file missing 'source' key:" << projectPath;
		return;
	}

	// Maintain most-recent-first unique list (max 10)
	recentProjects.removeAll(projectPath);
	recentProjects.prepend(projectPath);
	while (recentProjects.size() > 10)
		recentProjects.removeLast();

	updateRecentProjectsMenu();
}

void MainWindow::resumeProjectWorkflow(const QString& projectPath)
{
	if (!m_workflowStateMachine) {
		statusBar()->showMessage(tr("State machine not available"), 3000);
		return;
	}

	// Check if workflow is already running
	if (m_workflowStateMachine->isWorkflowActive()) {
		auto reply = QMessageBox::question(this,
			tr("Workflow Active"),
			tr("A workflow is currently running.\n\nClose it and resume the selected project?\n\n%1")
			.arg(QFileInfo(projectPath).fileName()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

		if (reply != QMessageBox::Yes) {
			return;
		}

		// Save the current workflow state before closing it,
		// so the user can resume it again later from the recent projects menu.
		m_workflowStateMachine->saveWorkflowState();

		// Cancel current workflow and wait for cleanup
		auto conn = QSharedPointer<QMetaObject::Connection>::create();
		*conn = connect(m_workflowStateMachine, &WorkflowStateMachine::canceled,
			this, [this, projectPath, conn]() {
				// Recursively call after cancel completes
				resumeProjectWorkflow(projectPath);
				disconnect(*conn);
				// QSharedPointer will automatically delete the connection
			}, Qt::QueuedConnection);

		m_workflowStateMachine->cancel();
		return;
	}

	// Load project sidecar
	if (!m_workflowStateMachine->loadProjectSidecarFile(projectPath)) {
		QMessageBox::warning(this, tr("Resume Failed"),
			tr("Could not load workflow state from:\n%1\n\nThe file may be corrupted.")
			.arg(projectPath));
		return;
	}

	// Add to recent projects
	addToRecentProjects(projectPath);
	writeSettings();
	updateRecentProjectsMenu();

	// Resume workflow
	statusBar()->showMessage(
		tr("Resuming workflow: %1").arg(QFileInfo(projectPath).fileName()),
		3000);

	m_workflowStateMachine->resume();
}

void MainWindow::onProcessingRequestLoadImage()
{
	if (!m_workflowStateMachine) return;

	const QString path = m_workflowStateMachine->inputFilePath();
	if (path.isEmpty()) {
		showProgressEnd();
		return;
	}

	// Just load the image - state machine handles all workflow decisions
	openAndNotifyImageLoaded(path, /*showProgress=*/true);
}

void MainWindow::onProcessingRequestOpenImage(const QString& path)
{
	if (path.isEmpty()) return;

	// Quiet open invoked from project loader/state machine. Do not show progress UI.
	openAndNotifyImageLoaded(path, /*showProgress=*/false);
}

void MainWindow::onProjectLoaded(const QString& projectPath)
{
	if (projectPath.isEmpty()) return;
	// Ensure project is present in recents and persist settings.
	addToRecentProjects(projectPath);
	writeSettings();
	updateRecentProjectsMenu();
	statusBar()->showMessage(tr("Loaded project: %1").arg(projectPath), 3000);
}

bool MainWindow::openAndNotifyImageLoaded(const QString& path, bool showProgress)
{
	if (path.isEmpty()) return false;

	if (showProgress) {
		ui->actionOpen->setEnabled(false);
		ui->actionSave->setEnabled(false);
		showProgressStart();
	}

	// Just load the file
	openFile(path);

	// Validate image
	bool success = false;
	if (m_imageLoader) {
		if (auto img = m_imageLoader->GetOutput()) {
			int dims[3];
			img->GetDimensions(dims);
			if (dims[0] > 1 && dims[1] > 1 && dims[2] > 1) {
				success = true;
			}
		}
	}

	if (showProgress) {
		showProgressEnd();
	}

	if (!success) {
		if (showProgress) {
			statusBar()->showMessage(
				tr("Failed to load image '%1'").arg(path),
				5000
			);
			ui->actionOpen->setEnabled(true);
			ui->actionSave->setEnabled(true);
		}
		return false;
	}

	// Notify state machine - it will handle sidecar and workflow orchestration
	if (m_workflowStateMachine) {
		// IMPORTANT:
		// - Do NOT call readSidecarForInput() here; sidecar lifecycle is owned by LoadingSidecar/ensureSidecarForSource().
		// - Do NOT overwrite the source input file when we're opening a derived image requested by the machine.
		const bool isOpeningDerived = (!m_workflowStateMachine->lastDerivedPath().isEmpty() &&
			path == m_workflowStateMachine->lastDerivedPath());

		if (!isOpeningDerived) {
			// Source open (user open or resume open source)
			m_workflowStateMachine->setInputFilePath(path);
		}

		if (isOpeningDerived) {
			m_workflowStateMachine->croppedLoaded();
		}
		else {
			m_workflowStateMachine->imageLoaded();
		}
	}

	if (showProgress) {
		statusBar()->showMessage(
			tr("Loaded image: %1").arg(path),
			3000
		);
		ui->actionOpen->setEnabled(true);
		ui->actionSave->setEnabled(true);
	}

	return true;
}

void MainWindow::onProcessingRequestLoadCropped()
{
	if (!m_workflowStateMachine) return;

	const QString path = m_workflowStateMachine->lastDerivedPath();
	if (path.isEmpty() || !QFile::exists(path)) {
		qWarning() << "State machine requested load of a cropped image that does not exist:" << path;
		// Optionally, tell the state machine it failed so it can transition to an error state or back to Idle.
		// m_workflowStateMachine->failed(tr("Cropped image not found."));
		return;
	}

	// Reuse the existing helper to load the image and notify the state machine.
	// We can show progress here as it's an explicit loading step.
	openAndNotifyImageLoaded(path, /*showProgress=*/true);
}

void MainWindow::onProcessingRequestLoadLandmarks(const QJsonObject& landmarksData)
{
	if (!m_workflowPanelWidget) {
		qWarning() << "No workflow panel widget available for loading landmarks";
		return;
	}

	// Get the LandmarkWidget from the panel
	auto* landmarkWidget = m_workflowPanelWidget->landmarkWidget();

	if (!landmarkWidget) {
		qWarning() << "No landmark widget available";
		statusBar()->showMessage(tr("Cannot load landmarks: widget not available"), 3000);
		return;
	}

	// Extract landmarks array from the parameters object
	// The sidecar stores landmarks in operations[].parameters
	QJsonArray landmarksArray;

	if (landmarksData.contains("landmarks") && landmarksData.value("landmarks").isArray()) {
		// Format 1: Direct "landmarks" key
		landmarksArray = landmarksData.value("landmarks").toArray();
	}
	else if (landmarksData.contains("parameters") && landmarksData.value("parameters").isObject()) {
		// Format 2: Nested in "parameters.landmarks"
		QJsonObject params = landmarksData.value("parameters").toObject();
		if (params.contains("landmarks") && params.value("landmarks").isArray()) {
			landmarksArray = params.value("landmarks").toArray();
		}
	}
	else {
		// Format 3: The entire object IS the parameters (most likely from state machine)
		// Assume it contains a "landmarks" key directly
		if (landmarksData.value("landmarks").isArray()) {
			landmarksArray = landmarksData.value("landmarks").toArray();
		}
	}

	if (landmarksArray.isEmpty()) {
		qWarning() << "No landmarks array found in data";
		statusBar()->showMessage(tr("No landmarks to load"), 2000);
		return;
	}

	// Load landmarks using existing method
	landmarkWidget->loadLandmarksFromJson(landmarksArray);

	statusBar()->showMessage(
		tr("Loaded %1 landmark(s)").arg(landmarksArray.size()),
		2000
	);

	qDebug() << "Successfully loaded" << landmarksArray.size() << "landmarks from sidecar";
}

void MainWindow::onProcessingRequestSaveLandmarks(const QJsonArray& landmarks)
{
	// This is an executor slot triggered by state machine

	if (!m_workflowStateMachine) {
		qWarning() << "Cannot save landmarks: no state machine available";
		return;
	}

	const QString inputPath = m_workflowStateMachine->inputFilePath();
	if (inputPath.isEmpty()) {
		qWarning() << "Cannot save landmarks: no input file path set";
		statusBar()->showMessage(tr("Cannot save landmarks: no input file loaded"), 3000);
		return;
	}

	if (landmarks.isEmpty()) {
		qWarning() << "No landmarks to save";
		statusBar()->showMessage(tr("No landmarks to save"), 2000);
		return;
	}

	// Package landmarks into parameters object
	QJsonObject params;
	params.insert(QStringLiteral("landmarks"), landmarks);

	// Delegate to state machine for sidecar persistence
	m_workflowStateMachine->appendHistoryToSidecar(
		inputPath,
		QStringLiteral("place_landmarks"),
		params
	);

	// Provide user feedback
	statusBar()->showMessage(
		tr("Saved %1 landmark(s) to project").arg(landmarks.size()),
		3000
	);

	qDebug() << "Saved" << landmarks.size() << "landmarks via state machine to" << inputPath;
}
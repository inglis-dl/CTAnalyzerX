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

	// --- Window/Level controller (owned by LightboxWidget) ---
	if (ui->lightboxWidget) {
		if (auto* wlController = ui->lightboxWidget->windowLevelController()) {
			// Let workflow panel manage controller location if available
			if (m_workflowPanelWidget) {
				m_workflowPanelWidget->insertAppearanceWidget(wlController);
			}
			else if (ui->controlPanelLayout) {
				ui->controlPanelLayout->insertWidget(1, wlController);
			}
			else {
				wlController->setParent(ui->controlPanel);
			}
		}
	}

	// Other view-mode sync left intact where only lightbox is required
	if (ui->lightboxWidget /* && ui->volumeControlsWidget */) {
		// If you later re-add VolumeControlsWidget wiring, guard and connect here.
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
			openFile(filePath);
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

// Handle drop: open the first supported file
void MainWindow::dropEvent(QDropEvent* event)
{
	const QMimeData* mimeData = event->mimeData();
	if (mimeData->hasUrls()) {
		for (const QUrl& url : mimeData->urls()) {
			QString filePath = url.toLocalFile();
			if (ImageLoader::CanReadFile(filePath)) {
				openFile(filePath);
				event->acceptProposedAction();
				return;
			}
		}
	}
	event->ignore();
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
	if (!path.isEmpty()) {
		// Reuse existing openFile helper to load and display image
		openFile(path);
		statusBar()->showMessage(tr("StateMachine: loading image '%1'").arg(path), 3000);
	}
	else {
		statusBar()->showMessage(tr("StateMachine: requestLoadImage (no path set)"), 3000);
		qDebug() << "ImageProcessingStateMachine requested load image but input path is empty.";
	}
}

void MainWindow::onProcessingRequestDefineCrop()
{
	// Hook for UI to enable crop tools. As a minimal integration we notify user.
	statusBar()->showMessage(tr("StateMachine: define crop"), 3000);
	qDebug() << "StateMachine requested crop definition.";
	// TODO: enable crop UI / forward to appropriate widget
}

void MainWindow::onProcessingRequestApplyCrop()
{
	statusBar()->showMessage(tr("StateMachine: apply crop"), 3000);
	qDebug() << "StateMachine requested applying crop.";
	// TODO: trigger crop worker and notify state machine via notifyCropApplied()
}

void MainWindow::onProcessingRequestLoadCropped()
{
	statusBar()->showMessage(tr("StateMachine: load cropped image"), 3000);
	qDebug() << "StateMachine requested load cropped.";
	// TODO: open cropped file (worker should call notifyCroppedLoaded())
}

void MainWindow::onProcessingRequestPlaceFiducials()
{
	statusBar()->showMessage(tr("StateMachine: place fiducials"), 3000);
	qDebug() << "StateMachine requested fiducial placement.";
	// TODO: forward to LightboxWidget / SelectionFrame controls
}

void MainWindow::onProcessingRequestStartInteractiveRotation()
{
	statusBar()->showMessage(tr("StateMachine: start interactive rotation"), 3000);
	qDebug() << "StateMachine requested interactive rotation.";
	// TODO: show rotation widget and let user manipulate then call notifyInteractiveRotationFinished()
}

void MainWindow::onProcessingRequestApplyRotation()
{
	statusBar()->showMessage(tr("StateMachine: apply rotation"), 3000);
	qDebug() << "StateMachine requested apply rotation.";
	// TODO: trigger rotation worker and call notifyRotationApplied()
}

void MainWindow::onProcessingRequestLoadRotated()
{
	statusBar()->showMessage(tr("StateMachine: load rotated image"), 3000);
	qDebug() << "StateMachine requested load rotated image.";
	// TODO: open rotated file
}

void MainWindow::onProcessingRequestComputeThreshold()
{
	statusBar()->showMessage(tr("StateMachine: compute threshold"), 3000);
	qDebug() << "StateMachine requested compute threshold.";
	// TODO: run threshold computation (Otsu/histogram) and notify state machine
}

void MainWindow::onProcessingRequestSegment()
{
	statusBar()->showMessage(tr("StateMachine: segment"), 3000);
	qDebug() << "StateMachine requested segmentation.";
	// TODO: run segmentation worker and call notifySegmentationDone()
}

void MainWindow::onProcessingRequestSaveSegment()
{
	statusBar()->showMessage(tr("StateMachine: save segment"), 3000);
	qDebug() << "StateMachine requested save segment.";
	// TODO: save segmentation and call notifySaved()
}

void MainWindow::onProcessingFinished()
{
	statusBar()->showMessage(tr("Processing finished"), 5000);
	qDebug() << "Image processing finished.";
	// TODO: post-processing UI updates
}

void MainWindow::onProcessingError(const QString& reason)
{
	qDebug() << "ImageProcessingStateMachine error:" << reason;
	QMessageBox::critical(this, tr("Processing Error"), reason);
	statusBar()->showMessage(tr("Processing error: %1").arg(reason), 10000);
}

#include "MainWindow.h"
#include "ui_MainWindow.h"

#include "LightboxWidget.h"
#include "ImageLoader.h"
#include "WindowLevelController.h"
#include "WindowLevelBridge.h"

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

#include <vtkVersion.h>   // VTK version macros
#include <vtkEventQtSlotConnect.h>

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

	imageLoader = vtkSmartPointer<ImageLoader>::New();

	vtkConnections = vtkSmartPointer<vtkEventQtSlotConnect>::New();

	vtkConnections->Connect(
	imageLoader, vtkCommand::StartEvent,
	this, SLOT(onVtkStartEvent()));

	vtkConnections->Connect(
		imageLoader, vtkCommand::EndEvent,
		this, SLOT(onVtkEndEvent()));

	vtkConnections->Connect(
		imageLoader, vtkCommand::ProgressEvent,
		this, SLOT(onVtkProgressEvent()));

	setupPanelConnections();

	loadRecentFiles();
}

MainWindow::~MainWindow()
{
	saveRecentFiles();
	delete ui;
}

void MainWindow::loadVolume(vtkSmartPointer<vtkImageData> imageData)
{
	currentImageData = imageData;
	if (!imageData || imageData->GetDimensions()[0] <= 1 ||
		imageData->GetDimensions()[1] <= 1 ||
		imageData->GetDimensions()[2] <= 1) {
		// If image is invalid, set default image in LightboxWidget
		ui->lightboxWidget->setDefaultImage();
	}
	else {
		ui->lightboxWidget->setImageData(imageData);
	}
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
	// control the volume cropping planes in the volumeview
	connect(ui->volumeControlsWidget, &VolumeControlsWidget::croppingRegionChanged,
		ui->lightboxWidget->getVolumeView(), &VolumeView::setCroppingRegion);

	// toggle volume slice planes
	connect(ui->volumeControlsWidget, &VolumeControlsWidget::slicePlaneToggle,
		ui->lightboxWidget->getVolumeView(), &VolumeView::setSlicePlanesVisible);

	// update the range sliders when the image extents change
	connect(ui->lightboxWidget->getVolumeView(), &VolumeView::imageExtentsChanged,
		ui->volumeControlsWidget, &VolumeControlsWidget::setRangeSliders);

	// synchronize cropping enabled state when VolumeView resets it (e.g., new image)
	connect(ui->lightboxWidget->getVolumeView(), &VolumeView::croppingEnabledChanged,
		ui->volumeControlsWidget, &VolumeControlsWidget::onExternalCroppingChanged);

	// --- Window/Level controller (now owned by LightboxWidget)
	if (ui->lightboxWidget) {
		if (auto* wlController = ui->lightboxWidget->windowLevelController()) {
			// If VolumeControlsWidget exposes an insert helper, use it so the groupBoxWindowLevel
			// will size itself to the controller. Otherwise fall back to previous behavior.
			if (ui->volumeControlsWidget) {
				ui->volumeControlsWidget->insertWindowLevelController(wlController);
			}
			else if (ui->controlPanelLayout) {
				ui->controlPanelLayout->insertWidget(1, wlController); // insert after VolumeControlsWidget
			}
			else {
				wlController->setParent(ui->controlPanel);
			}
		}
	}

	// NOTE: signal wiring between controller <-> bridge <-> views happens inside LightboxWidget.
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
		// Optionally, set an icon based on file type
		/*
		if (displayName.endsWith(".isq", Qt::CaseInsensitive)) {
			action->setIcon(QIcon(":/icons/isq.png")); // Provide a suitable icon resource
		}
		else if (displayName.endsWith(".dcm", Qt::CaseInsensitive) || displayName.endsWith(".dicom", Qt::CaseInsensitive)) {
			action->setIcon(QIcon(":/icons/dicom.png")); // Provide a suitable icon resource
		}
		*/
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

void MainWindow::loadRecentFiles()
{
	QSettings settings("CTAnalyzerX", "RecentFiles");
	recentFiles = settings.value("recentFiles").toStringList();
	updateRecentFilesMenu();
}

void MainWindow::saveRecentFiles()
{
	QSettings settings("CTAnalyzerX", "RecentFiles");
	settings.setValue("recentFiles", recentFiles);
}

void MainWindow::clearRecentFiles()
{
	recentFiles.clear();
	updateRecentFilesMenu();
	saveRecentFiles();
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
			QString("The selected file cannot be opened. It may not exist, is not readable, or is not a supported type (DICOM or ISQ).\n\nFile: %1").arg(filePath));
		return;
	}

	// Set image type based on extension
	QString lower = filePath.toLower();
	if (lower.endsWith(".isq")) {
		imageLoader->SetImageType(ImageLoader::ImageType::ScancoISQ);
	}
	else if (lower.endsWith(".dcm") || lower.endsWith(".dicom")) {
		imageLoader->SetImageType(ImageLoader::ImageType::DICOM);
	}

	imageLoader->SetInputPath(filePath);

	// Try to load the image with detailed error feedback
	vtkSmartPointer<vtkImageData> vtkImage;
	try {
		imageLoader->Update();
		vtkImage = imageLoader->GetOutput();
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

	// Display the loaded image
	loadVolume(vtkImage);

	// Update recent files list
	addToRecentFiles(filePath);

	saveRecentFiles();
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

void MainWindow::showEvent(QShowEvent* event)
{
	QMainWindow::showEvent(event);

	if (!defaultImageLoaded) {
		if (ui->lightboxWidget) {
			ui->lightboxWidget->setDefaultImage();
		}
		defaultImageLoaded = true;
	}
}

void MainWindow::onVtkStartEvent()
{
	progressBar->setValue(0);
	progressBar->setVisible(true);
}

void MainWindow::onVtkEndEvent()
{
	progressBar->setValue(100);
	progressBar->setVisible(false);
}

void MainWindow::onVtkProgressEvent()
{
	if (!imageLoader) return;
	double progress = imageLoader->GetProgress(); // vtkAlgorithm::GetProgress()
	progressBar->setValue(static_cast<int>(progress * 100));
	progressBar->setVisible(true);
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
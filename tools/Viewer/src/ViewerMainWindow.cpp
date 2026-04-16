#include "ViewerMainWindow.h"

#include "CollapsibleGroupBox.h"
#include "ImageInfoWidget.h"
#include "ImageLoader.h"
#include "LightboxWidget.h"
#include "ScalarOpacityFunctionWidget.h"
#include "WindowLevelWidget.h"

#include <vtkEventQtSlotConnect.h>
#include <vtkImageData.h>
#include <vtkPiecewiseFunction.h>
#include <vtkVersion.h>

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <limits>

// ─────────────────────────────────────────────────────────────────────────────

ViewerMainWindow::ViewerMainWindow(QWidget* parent)
	: QMainWindow(parent)
{
	setWindowTitle(QStringLiteral("CTAXViewer"));
	resize(1200, 800);
	setAcceptDrops(true);

	m_imageLoader    = vtkSmartPointer<ImageLoader>::New();
	m_vtkConnections = vtkSmartPointer<vtkEventQtSlotConnect>::New();

	buildUi();
	buildMenus();
	// wireConnections() is deferred to showEvent() because it depends on m_lightbox,
	// which is constructed lazily once an OpenGL context is available.
	readSettings();
}

ViewerMainWindow::~ViewerMainWindow()
{
	writeSettings();
}

// ── UI construction ──────────────────────────────────────────────────────────

void ViewerMainWindow::buildUi()
{
	// ── Status bar progress ───────────────────────────────────────────────────
	m_progressBar = new QProgressBar(this);
	m_progressBar->setRange(0, 100);
	m_progressBar->setValue(0);
	m_progressBar->setVisible(false);
	statusBar()->addPermanentWidget(m_progressBar);

	// ── Left panel ────────────────────────────────────────────────────────────
	// Scrollable container so the panel never clips on small screens.
	auto* scrollContent  = new QWidget;
	auto* scrollLayout   = new QVBoxLayout(scrollContent);
	scrollLayout->setContentsMargins(4, 4, 4, 4);
	scrollLayout->setSpacing(6);

	// Image info section
	m_imageInfo = new ImageInfoWidget(scrollContent);
	auto* grpImageInfo = new CollapsibleGroupBox(tr("Info"), scrollContent);
	auto* imageInfoLayout = new QVBoxLayout;
	imageInfoLayout->setContentsMargins(0, 0, 0, 0);
	imageInfoLayout->addWidget(m_imageInfo);
	grpImageInfo->setLayout(imageInfoLayout);

	// Window / level section
	m_windowLevel = new WindowLevelWidget(scrollContent);
	auto* grpWindowLevel = new CollapsibleGroupBox(tr("Window/Level"), scrollContent);
	auto* windowLevelLayout = new QVBoxLayout;
	windowLevelLayout->setContentsMargins(0, 0, 0, 0);
	windowLevelLayout->addWidget(m_windowLevel);
	grpWindowLevel->setLayout(windowLevelLayout);

	scrollLayout->addWidget(grpImageInfo);
	scrollLayout->addWidget(grpWindowLevel);
	scrollLayout->addStretch();

	auto* scrollArea = new QScrollArea;
	scrollArea->setWidgetResizable(true);
	scrollArea->setWidget(scrollContent);
	scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scrollArea->setFrameShape(QFrame::NoFrame);
	scrollArea->setMinimumWidth(260);
	scrollArea->setMaximumWidth(340);

	// ── Right panel (lightbox) placeholder ───────────────────────────────────
	// LightboxWidget contains VolumeView which creates a vtkGPUVolumeRayCastMapper.
	// That mapper requires an active OpenGL context which is only available after
	// the window has been shown. Defer construction to showEvent() and use a plain
	// QWidget as a stand-in so the splitter layout is valid from the start.
	m_lightboxPlaceholder = new QWidget(this);
	m_lightboxPlaceholder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	// ── Splitter ──────────────────────────────────────────────────────────────
	m_splitter = new QSplitter(Qt::Horizontal, this);
	m_splitter->setChildrenCollapsible(false);
	m_splitter->addWidget(scrollArea);
	m_splitter->addWidget(m_lightboxPlaceholder);
	m_splitter->setStretchFactor(0, 0);
	m_splitter->setStretchFactor(1, 1);

	setCentralWidget(m_splitter);
}

void ViewerMainWindow::buildMenus()
{
	// ── File menu ─────────────────────────────────────────────────────────────
	auto* fileMenu = menuBar()->addMenu(tr("&File"));

	auto* actOpen = new QAction(tr("&Open…"), this);
	actOpen->setShortcut(QKeySequence::Open);
	connect(actOpen, &QAction::triggered, this, &ViewerMainWindow::onActionOpen);
	fileMenu->addAction(actOpen);

	fileMenu->addSeparator();

	// Recent files (populated dynamically)
	m_recentFilesMenu = fileMenu->addMenu(tr("Recent Files"));

	fileMenu->addSeparator();

	auto* actScreenshot = new QAction(tr("Take &Screenshot…"), this);
	connect(actScreenshot, &QAction::triggered, this, &ViewerMainWindow::onActionScreenshot);
	fileMenu->addAction(actScreenshot);

	fileMenu->addSeparator();

	auto* actExit = new QAction(tr("E&xit"), this);
	actExit->setShortcut(QKeySequence::Quit);
	connect(actExit, &QAction::triggered, this, &ViewerMainWindow::onActionExit);
	fileMenu->addAction(actExit);

	// ── Help menu ─────────────────────────────────────────────────────────────
	auto* helpMenu = menuBar()->addMenu(tr("&Help"));
	auto* actAbout = new QAction(tr("&About…"), this);
	connect(actAbout, &QAction::triggered, this, &ViewerMainWindow::onActionAbout);
	helpMenu->addAction(actAbout);
}

void ViewerMainWindow::wireConnections()
{
	// ── VTK loader events ─────────────────────────────────────────────────────
	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::StartEvent,
		this, SLOT(onVtkStartEvent()));
	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::EndEvent,
		this, SLOT(onVtkEndEvent()));
	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::ProgressEvent,
		this, SLOT(onVtkProgressEvent()));

	// ImageLoader JSON metadata -> ImageInfoWidget
	if (auto* emitter = m_imageLoader->metaEmitter()) {
		connect(emitter, &ImageLoaderMetaEmitter::metaUpdated,
			m_imageInfo, &ImageInfoWidget::updateFromMeta,
			Qt::QueuedConnection);
	}

	// LightboxWidget default-image metadata -> ImageInfoWidget
	connect(m_lightbox, &LightboxWidget::metaReady,
		m_imageInfo, &ImageInfoWidget::updateFromMeta,
		Qt::QueuedConnection);

	// ── Window / Level wiring ─────────────────────────────────────────────────
	// Register the WindowLevelWidget with the Lightbox so it mediates all
	// propagation (controller <-> slice views <-> volume view).
	m_lightbox->setWindowLevelWidget(m_windowLevel);
}

// ── Slots ────────────────────────────────────────────────────────────────────

void ViewerMainWindow::showEvent(QShowEvent* event)
{
	QMainWindow::showEvent(event);

	// Build and wire the LightboxWidget the first time the window is shown so
	// that an OpenGL context exists before vtkGPUVolumeRayCastMapper::New() is called.
	if (!m_lightbox)
	{
		m_lightbox = new LightboxWidget(this);
		m_lightbox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

		// Swap the placeholder out for the real LightboxWidget.
		const int idx = m_splitter->indexOf(m_lightboxPlaceholder);
		m_splitter->replaceWidget(idx, m_lightbox);
		m_lightboxPlaceholder->deleteLater();
		m_lightboxPlaceholder = nullptr;

		wireConnections();
	}

	updateRecentFilesMenu();
}

void ViewerMainWindow::closeEvent(QCloseEvent* event)
{
	writeSettings();
	QMainWindow::closeEvent(event);
}

void ViewerMainWindow::onActionOpen()
{
	const QString path = QFileDialog::getOpenFileName(
		this,
		tr("Open Image"),
		{},
		tr("All Supported (*.isq *.nii *.nii.gz *.dcm *.dicom);;"
		   "ISQ Files (*.isq);;"
		   "NIfTI Files (*.nii *.nii.gz);;"
		   "DICOM Files (*.dcm *.dicom);;"
		   "All Files (*)"));

	if (!path.isEmpty())
		openFile(path);
}

void ViewerMainWindow::onActionScreenshot()
{
	const QString path = QFileDialog::getSaveFileName(
		this, tr("Save Screenshot"), {},
		tr("PNG Files (*.png);;JPEG Files (*.jpg)"));

	if (path.isEmpty())
		return;

	const QImage img = this->grab().toImage();
	if (!img.save(path)) {
		QMessageBox::warning(this, tr("Screenshot"),
			tr("Failed to save screenshot to:\n%1").arg(path));
	}
	else {
		statusBar()->showMessage(tr("Screenshot saved: %1").arg(path), 3000);
	}
}

void ViewerMainWindow::onActionExit()
{
	close();
}

void ViewerMainWindow::onActionAbout()
{
	const QString vtkVer = QString::fromLatin1(vtkVersion::GetVTKVersionFull());
	const QString qtVer  = QString::fromLatin1(QT_VERSION_STR);

	QMessageBox::about(this, tr("About CTAXViewer"),
		tr("CTAXViewer\n\n"
		   "3-D image viewer.\n\n"
		   "Qt:  %1\n"
		   "VTK: %2")
		.arg(qtVer, vtkVer));
}

// ── VTK progress forwarding ──────────────────────────────────────────────────

void ViewerMainWindow::onVtkStartEvent()
{
	if (QThread::currentThread() == thread()) {
		showProgressStart();
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "showProgressStart", Qt::QueuedConnection);
	}
}

void ViewerMainWindow::onVtkEndEvent()
{
	if (QThread::currentThread() == thread()) {
		showProgressEnd();
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "showProgressEnd", Qt::QueuedConnection);
	}
}

void ViewerMainWindow::onVtkProgressEvent()
{
	if (!m_imageLoader) return;
	const int value = static_cast<int>(
		std::clamp(m_imageLoader->GetProgress(), 0.0, 1.0) * 100.0);

	if (QThread::currentThread() == thread()) {
		showProgressValue(value);
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else {
		QMetaObject::invokeMethod(this, "showProgressValue",
			Qt::QueuedConnection, Q_ARG(int, value));
	}
}

void ViewerMainWindow::showProgressStart()
{
	m_progressBar->setValue(0);
	m_progressBar->setVisible(true);
	m_progressBar->setEnabled(true);
}

void ViewerMainWindow::showProgressValue(int percent)
{
	m_progressBar->setValue(percent);
	m_progressBar->setVisible(true);
}

void ViewerMainWindow::showProgressEnd()
{
	m_progressBar->setValue(100);
	m_progressBar->setVisible(false);
}

// ── File loading ─────────────────────────────────────────────────────────────

void ViewerMainWindow::openFile(const QString& filePath)
{
	if (!ImageLoader::CanReadFile(filePath)) {
		QMessageBox::warning(this, tr("Cannot Open File"),
			tr("The file cannot be opened or is not a supported format "
			   "(DICOM, ISQ, NIfTI).\n\nFile: %1").arg(filePath));
		return;
	}

	// Detect image type from extension
	const QString lower = filePath.toLower();
	if (lower.endsWith(QLatin1String(".isq"))) {
		m_imageLoader->SetImageType(ImageLoader::ImageType::ScancoISQ);
	}
	else if (lower.endsWith(QLatin1String(".nii")) ||
	         lower.endsWith(QLatin1String(".nii.gz"))) {
		m_imageLoader->SetImageType(ImageLoader::ImageType::NIFTI);
	}
	else {
		// DICOM file or directory
		m_imageLoader->SetImageType(ImageLoader::ImageType::DICOM);
	}

	m_imageLoader->SetInputPath(filePath);

	vtkSmartPointer<vtkImageData> image;
	try {
		m_imageLoader->Update();
		image = m_imageLoader->GetOutput();
	}
	catch (const std::exception& ex) {
		QMessageBox::critical(this, tr("Load Error"),
			tr("Failed to load:\n%1\n\nDetails: %2").arg(filePath, QString::fromLocal8Bit(ex.what())));
		return;
	}
	catch (...) {
		QMessageBox::critical(this, tr("Load Error"),
			tr("An unknown error occurred while loading:\n%1").arg(filePath));
		return;
	}

	if (!image) {
		QMessageBox::critical(this, tr("Load Error"),
			tr("Loader returned no data for:\n%1").arg(filePath));
		return;
	}

	int dims[3] = {};
	image->GetDimensions(dims);
	if (dims[0] <= 1 || dims[1] <= 1 || dims[2] <= 1) {
		QMessageBox::critical(this, tr("Invalid Data"),
			tr("The loaded volume has invalid dimensions and cannot be displayed.\n\nFile: %1")
			.arg(filePath));
		return;
	}

	// Push data into Lightbox
	m_lightbox->setInputConnection(m_imageLoader->GetOutputPort(), /*newImg=*/ true);

	// Feed image data to the window-level controller
	m_windowLevel->setImageData(image);

	// Update ImageInfoWidget directly (loader meta emitter also fires, this is a safety net)
	m_imageInfo->setImage(image, filePath);

	addToRecentFiles(filePath);
	writeSettings();
	statusBar()->showMessage(tr("Loaded: %1").arg(filePath), 4000);
}

// ── Drag / drop ───────────────────────────────────────────────────────────────

void ViewerMainWindow::dragEnterEvent(QDragEnterEvent* event)
{
	if (event->mimeData()->hasUrls()) {
		for (const QUrl& url : event->mimeData()->urls()) {
			if (ImageLoader::CanReadFile(url.toLocalFile())) {
				event->acceptProposedAction();
				return;
			}
		}
	}
	event->ignore();
}

void ViewerMainWindow::dropEvent(QDropEvent* event)
{
	if (!event->mimeData()->hasUrls()) {
		event->ignore();
		return;
	}
	for (const QUrl& url : event->mimeData()->urls()) {
		const QString path = url.toLocalFile();
		if (ImageLoader::CanReadFile(path)) {
			openFile(path);
			event->acceptProposedAction();
			return;
		}
	}
	event->ignore();
}

// ── Recent files ──────────────────────────────────────────────────────────────

void ViewerMainWindow::addToRecentFiles(const QString& filePath)
{
	m_recentFiles.removeAll(filePath);
	m_recentFiles.prepend(filePath);
	while (m_recentFiles.size() > 10)
		m_recentFiles.removeLast();
	updateRecentFilesMenu();
}

void ViewerMainWindow::updateRecentFilesMenu()
{
	m_recentFilesMenu->clear();

	if (m_recentFiles.isEmpty()) {
		auto* placeholder = new QAction(tr("(none)"), this);
		placeholder->setEnabled(false);
		m_recentFilesMenu->addAction(placeholder);
		return;
	}

	for (const QString& path : m_recentFiles) {
		const QString label = QFileInfo(path).fileName();
		auto* act = new QAction(label, this);
		act->setToolTip(path);
		connect(act, &QAction::triggered, this, [this, path]() {
			openFile(path);
		});
		m_recentFilesMenu->addAction(act);
	}

	m_recentFilesMenu->addSeparator();
	auto* clearAct = new QAction(tr("Clear Recent Files"), this);
	connect(clearAct, &QAction::triggered, this, [this]() {
		m_recentFiles.clear();
		updateRecentFilesMenu();
		writeSettings();
	});
	m_recentFilesMenu->addAction(clearAct);
}

// ── Settings persistence ──────────────────────────────────────────────────────

void ViewerMainWindow::readSettings()
{
	QSettings s(QStringLiteral("CTAnalyzerX"), QStringLiteral("CTAXViewer"));

	s.beginGroup(QStringLiteral("window"));
	const QRect geo = s.value(QStringLiteral("geometry")).toRect();
	if (geo.isValid() && !geo.isEmpty())
		setGeometry(geo);
	s.endGroup();

	s.beginGroup(QStringLiteral("recent"));
	m_recentFiles = s.value(QStringLiteral("files")).toStringList();
	s.endGroup();

	updateRecentFilesMenu();
}

void ViewerMainWindow::writeSettings()
{
	QSettings s(QStringLiteral("CTAnalyzerX"), QStringLiteral("CTAXViewer"));

	s.beginGroup(QStringLiteral("window"));
	s.setValue(QStringLiteral("geometry"), geometry());
	s.endGroup();

	s.beginGroup(QStringLiteral("recent"));
	s.setValue(QStringLiteral("files"), m_recentFiles);
	s.endGroup();
}
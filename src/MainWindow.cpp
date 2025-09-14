#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QKeyEvent>
#include <QImage>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>

#include "LightBoxWidget.h"
#include "ImageLoader.h"

#include <itkImage.h>
#include <itkImageSeriesReader.h>
#include <itkGDCMImageIO.h>
#include <itkGDCMSeriesFileNames.h>
#include <itkImageFileReader.h>
#include <itkImageToVTKImageFilter.h>

using ImageType = itk::Image<short, 3>;

MainWindow::MainWindow(QWidget* parent)
	: QMainWindow(parent), ui(new Ui::MainWindow)
{
	ui->setupUi(this);

	// Create and set LightBoxWidget as the central widget
	//lightBoxWidget = new LightBoxWidget(this);
	//setCentralWidget(lightBoxWidget);

	// Connect menu actions to slots
	connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::onActionOpen);
	connect(ui->actionSave, &QAction::triggered, this, &MainWindow::onActionSave);
	connect(ui->actionExit, &QAction::triggered, this, &MainWindow::onActionExit);
	connect(ui->actionAbout, &QAction::triggered, this, &MainWindow::onActionAbout);
	connect(ui->actionScreenshot, &QAction::triggered, this, &MainWindow::saveScreenshot);

	imageLoader = vtkSmartPointer<ImageLoader>::New();

	setupPanelConnections();

	//ui->lightBoxWidget->connectVolumeControlsWidget(ui->volumeControlsWidget);

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
		// If image is invalid, set default image in LightBoxWidget
		ui->lightBoxWidget->setDefaultImage();
	}
	else {
		ui->lightBoxWidget->setImageData(imageData);
	}
}

void MainWindow::onActionOpen()
{
	QString fileName = QFileDialog::getOpenFileName(this, tr("Open File"), "", tr("DICOM Folder (*.dcm);;ISQ Files (*.isq);;All Files (*)"));
	if (fileName.isEmpty()) return;

	vtkSmartPointer<vtkImageData> vtkImage;

	try {
		imageLoader->SetInputPath(fileName);
		imageLoader->Update();
		vtkImage = imageLoader->GetOutput();
		if (!vtkImage) {
			QMessageBox::critical(this, "Error", "Failed to load volume: Unsupported or invalid file.");
			return;
		}
	}
	catch (const std::exception& ex) {
		QMessageBox::critical(this, "Error", QString("Failed to load volume: %1").arg(ex.what()));
		return;
	}

	loadVolume(vtkImage);
	addToRecentFiles(fileName);
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
	QMessageBox::about(this, tr("About CTAnalyzerX"), tr("3D volume image visualization tool for DICOM and Scanco .isq files."));
}

void MainWindow::setupPanelConnections()
{
	// Analysis Panel: Run Analysis button and output box
	auto runButton = ui->btnRunAnalysis;
	auto outputBox = ui->txtAnalysisOutput;

	if (runButton && outputBox) {
		connect(runButton, &QPushButton::clicked, this, [=]() {
			outputBox->append("Running analysis...");
			// Insert actual analysis logic here
			outputBox->append("Analysis complete.");
		});
	}

	// control the volume cropping planes in the volumeview
	connect(ui->volumeControlsWidget, &VolumeControlsWidget::croppingRegionChanged,
		ui->lightBoxWidget->getVolumeView(), &VolumeView::setCroppingRegion);

	// update the range sliders when the image extents change
	connect(ui->lightBoxWidget->getVolumeView(), &VolumeView::imageExtentsChanged,
		ui->volumeControlsWidget, &VolumeControlsWidget::setRangeSliders);

	// toggle volume slice planes
	connect(ui->volumeControlsWidget, &VolumeControlsWidget::slicePlaneToggle,
		ui->lightBoxWidget->getVolumeView(), &VolumeView::setSlicePlanesVisible);
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

	// Add up to 10 recent file actions
	int count = 0;
	for (const QString& filePath : recentFiles) {
		if (count++ >= 10) break;
		QAction* action = new QAction(filePath, this);
		action->setProperty("isRecentFile", true);
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
	// Your file loading logic...

	if (!recentFiles.contains(filePath)) {
		recentFiles.prepend(filePath);
		if (recentFiles.size() > 10)
			recentFiles.removeLast();
	}

	updateRecentFilesMenu();
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

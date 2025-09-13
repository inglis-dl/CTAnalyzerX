#include "MainWindow.h"
#include "ui_MainWindow.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QKeyEvent>
#include <QImage>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
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
	loadRecentFiles();

	lightBoxWidget = new LightBoxWidget(this);
	setCentralWidget(lightBoxWidget);

	setupVolumeRenderingToggle();
	setupViewModeToggle();
	setupScreenshotButton();

	addDockWidget(Qt::LeftDockWidgetArea, ui->dockFileBrowser);
	addDockWidget(Qt::RightDockWidgetArea, ui->dockSliceControls);
	addDockWidget(Qt::BottomDockWidgetArea, ui->dockAnalysisPanel);

	setupDockConnections();
}

MainWindow::~MainWindow()
{
	saveRecentFiles();
	delete ui;
}

void MainWindow::loadVolume(vtkSmartPointer<vtkImageData> imageData)
{
	currentImageData = imageData;
	lightBoxWidget->setImageData(imageData);
}

void MainWindow::onActionOpen()
{
	QString fileName = QFileDialog::getOpenFileName(this, tr("Open File"), "", tr("DICOM Folder (*.dcm);;ISQ Files (*.isq);;All Files (*)"));
	if (fileName.isEmpty()) return;

	vtkSmartPointer<vtkImageData> vtkImage;

	try {
		if (fileName.endsWith(".isq")) {
			auto reader = itk::ImageFileReader<ImageType>::New();
			reader->SetFileName(fileName.toStdString());
			reader->Update();

			auto connector = itk::ImageToVTKImageFilter<ImageType>::New();
			connector->SetInput(reader->GetOutput());
			connector->Update();

			vtkImage = connector->GetOutput();
		}
		else {
			auto namesGenerator = itk::GDCMSeriesFileNames::New();
			namesGenerator->SetDirectory(fileName.toStdString());

			auto reader = itk::ImageSeriesReader<ImageType>::New();
			reader->SetImageIO(itk::GDCMImageIO::New());
			reader->SetFileNames(namesGenerator->GetInputFileNames());
			reader->Update();

			auto connector = itk::ImageToVTKImageFilter<ImageType>::New();
			connector->SetInput(reader->GetOutput());
			connector->Update();

			vtkImage = connector->GetOutput();
		}
	}
	catch (itk::ExceptionObject& ex) {
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

void MainWindow::onActionZoom()
{
	QMessageBox::information(this, tr("Zoom"), tr("Zoom action triggered."));
}

void MainWindow::saveScreenshot()
{
	QImage screenshot = lightBoxWidget->grabFramebuffer();
	QString filePath = QFileDialog::getSaveFileName(this, "Save Screenshot", "", "PNG Files (*.png);;JPEG Files (*.jpg)");
	if (!filePath.isEmpty()) {
		screenshot.save(filePath);
		QMessageBox::information(this, "Screenshot Saved", "Saved to:\n" + filePath);
	}
}

void MainWindow::updateOpacity(int value)
{
	// Optional: delegate to LightBoxWidget if needed
}

void MainWindow::updateColor(int value)
{
	// Optional: delegate to LightBoxWidget if needed
}

void MainWindow::setupVolumeRenderingToggle()
{
	QPushButton* toggleButton = new QPushButton("Toggle Volume Rendering");
	connect(toggleButton, &QPushButton::clicked, this, &MainWindow::onActionZoom); // Replace with actual toggle if needed
	QWidget* dockWidget = ui->dockAnalysisPanel->widget();
	QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(dockWidget->layout());
	if (!layout) {
		layout = new QVBoxLayout(dockWidget);
		dockWidget->setLayout(layout);
	}
	layout->addWidget(toggleButton);
}

void MainWindow::setupViewModeToggle()
{
	QPushButton* toggleButton = new QPushButton("Toggle View Mode");
	connect(toggleButton, &QPushButton::clicked, this, &MainWindow::onActionSave); // Replace with actual toggle if needed
	QWidget* dockWidget = ui->dockAnalysisPanel->widget();
	QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(dockWidget->layout());
	if (!layout) {
		layout = new QVBoxLayout(dockWidget);
		dockWidget->setLayout(layout);
	}
	layout->addWidget(toggleButton);
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
	ui->menuRecentFiles->clear();
	for (const QString& filePath : recentFiles) {
		QAction* action = new QAction(filePath, this);
		connect(action, &QAction::triggered, this, [=](/*filePath*/) {
			onActionOpen(); // Replace with openFile(filePath) if implemented
		});
		ui->menuRecentFiles->addAction(action);
	}
	ui->menuRecentFiles->addSeparator();
	QAction* clearAction = new QAction("Clear Recent Files", this);
	connect(clearAction, &QAction::triggered, this, &MainWindow::clearRecentFiles);
	ui->menuRecentFiles->addAction(clearAction);
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

void MainWindow::setupDockConnections()
{
	// File Browser: Browse Files button
	if (auto browseButton = ui->dockFileBrowser->findChild<QPushButton*>("btnBrowseFiles")) {
		connect(browseButton, &QPushButton::clicked, this, &MainWindow::onActionOpen);
	}

	// Slice Controls: Slider and Label
	auto sliceSlider = ui->dockSliceControls->findChild<QSlider*>("sliderSlicePosition");
	auto sliceLabel = ui->dockSliceControls->findChild<QLabel*>("lblSlicePosition");

	if (sliceSlider && sliceLabel) {
		connect(sliceSlider, &QSlider::valueChanged, this, = {
			sliceLabel->setText(QString("Slice Position: %1").arg(value));
		// Optional: sync with LightBoxWidget
		lightBoxWidget->setAxialSlice(value);
		});
	}

	// Analysis Panel: Run Analysis button and output box
	auto runButton = ui->dockAnalysisPanel->findChild<QPushButton*>("btnRunAnalysis");
	auto outputBox = ui->dockAnalysisPanel->findChild<QTextEdit*>("txtAnalysisOutput");

	if (runButton && outputBox) {
		connect(runButton, &QPushButton::clicked, this, = {
			outputBox->append("Running analysis...");
		// Insert actual analysis logic here
		outputBox->append("Analysis complete.");
		});
	}
}

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>
#include <QProgressBar>
#include <vtkSmartPointer.h>

#include <memory>

// Add include for state machine
#include "ImageProcessingStateMachine.h"

namespace Ui {
	class MainWindow;
}

class vtkImageData;
class ImageLoader;
class vtkEventQtSlotConnect;
class CropExporter;
class WorkflowPanelWidget;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow();

signals:
	void requestLoadViewSettings();
	void requestSaveViewSettings();

protected:
	void keyPressEvent(QKeyEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;
	void closeEvent(QCloseEvent* event) override; // persist settings on close
	void showEvent(QShowEvent* e) override;

private slots:
	void onActionOpen();
	void onActionSave();
	void onActionExit();
	void onActionAbout();
	void saveScreenshot();
	void clearRecentFiles();
	void onVtkStartEvent();
	void onVtkEndEvent();
	void onVtkProgressEvent();
	void showProgressValue(int percent);
	void showProgressStart();
	void showProgressEnd();

	// ImageProcessingStateMachine integration slots
	void onProcessingRequestLoadImage();
	void onProcessingRequestDefineCrop();
	void onProcessingRequestSaveCropped();
	void onProcessingRequestLoadCropped();
	void onProcessingRequestPlaceFiducials();
	void onProcessingRequestStartInteractiveRotation();
	void onProcessingRequestApplyRotation();
	void onProcessingRequestLoadRotated();
	void onProcessingRequestComputeThreshold();
	void onProcessingRequestSegment();
	void onProcessingRequestSaveSegment();
	void onProcessingFinished();
	void onProcessingError(const QString& reason);

private:
	void setupPanelConnections();
	void addToRecentFiles(const QString& filePath);
	void updateRecentFilesMenu();
	void loadRecentFiles();
	void saveRecentFiles();
	void openFile(const QString& filePath);

	// JSON-backed settings helpers
	void readSettings();
	void writeSettings();

	// New: centralized UI update helper driven by state machine
	void updateUiForState(ImageProcessingStateMachine::State s);

	Ui::MainWindow* ui;
	QStringList recentFiles;
	vtkSmartPointer<vtkImageData> currentImageData;
	vtkSmartPointer<vtkEventQtSlotConnect> vtkConnections;
	vtkSmartPointer<ImageLoader> m_imageLoader = nullptr;
	QProgressBar* progressBar = nullptr;
	bool defaultImageLoaded = false;

	// Left-side workflow panel (replaces legacy control widgets)
	WorkflowPanelWidget* m_workflowPanelWidget = nullptr;

	// Image processing state machine
	ImageProcessingStateMachine* m_processingStateMachine = nullptr;

	// Crop exporter (signal/slot driven, no direct ownership of UI/loader/state machine pointers)
	CropExporter* m_cropExporter = nullptr;

	// Note: VolumeControlsWidget / VolumeRotationWidget usage has been removed
	// per refactor to use WorkflowPanelWidget (left) and LightboxWidget (right).
};

#endif // MAINWINDOW_H
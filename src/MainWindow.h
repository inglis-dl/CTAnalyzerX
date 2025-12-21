#pragma once

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
	void closeEvent(QCloseEvent* event) override;
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
	// Slot invoked when state machine asks MainWindow to open a specific image (project load)
	void onProcessingRequestOpenImage(const QString& path);
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
	// Verify and prune recent caches at startup
	void verifySettings();                 // one-time call on first show
	void verifyRecentFiles();              // prune recentFiles list
	void verifyRecentProjects();           // prune recentProjects list
	void loadRecentFiles();
	void saveRecentFiles();
	void openFile(const QString& filePath);

	// Helper used by both state-machine-driven loads and project-driven opens.
	// - path: file to open
	// - showProgress: when true, show loader UI and disable top-level actions.
	// Returns true if the image was loaded successfully and the state machine was notified.
	bool openAndNotifyImageLoaded(const QString& path, bool showProgress);

	// JSON-backed settings helpers
	void readSettings();
	void writeSettings();

	// New: centralized UI update helper driven by state machine
	void updateUiForState(ImageProcessingStateMachine::State s);

	Ui::MainWindow* ui;
	QStringList recentFiles;

	// Projects (JSON sidecar) support
	QStringList recentProjects;                  // most-recent first
	QMenu* m_projectsMenu = nullptr;
	// Ensure verifySettings runs once when the main window is first shown.
	bool m_settingsVerified = false;

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

	// Projects menu helpers
	void addToRecentProjects(const QString& projectPath);
	void updateRecentProjectsMenu();
	void clearRecentProjects();
	void openProjectFile(const QString& sidecarPath);

	// Slot invoked when state-machine indicates a project was loaded (so we can add to recents)
	void onProjectLoaded(const QString& projectPath);
};

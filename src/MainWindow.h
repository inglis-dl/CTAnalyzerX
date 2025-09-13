#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>

#include "LightBoxWidget.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow();

private slots:
	void onActionOpen();
	void onActionSave();
	void onActionZoom();
	void saveScreenshot();
	void updateOpacity(int value);
	void updateColor(int value);
	void clearRecentFiles();

private:
	Ui::MainWindow* ui;
	LightBoxWidget* lightBoxWidget;
	vtkSmartPointer<vtkImageData> currentImageData;

	void loadVolume(vtkSmartPointer<vtkImageData> imageData);
	void setupVolumeRenderingToggle();
	void setupViewModeToggle();
	void setupScreenshotButton();
	void addToRecentFiles(const QString& filePath);
	void updateRecentFilesMenu();
	void loadRecentFiles();
	void saveRecentFiles();
	void setupDockConnections();


protected:
	void keyPressEvent(QKeyEvent* event) override;

	QStringList recentFiles;
};

#endif // MAINWINDOW_H

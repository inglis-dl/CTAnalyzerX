#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>
#include <QProgressBar>
#include <vtkSmartPointer.h>

namespace Ui {
	class MainWindow;
}

class vtkImageData;
class ImageLoader;
class vtkEventQtSlotConnect;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow();

protected:
	void keyPressEvent(QKeyEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

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

private:
	void setupPanelConnections();
	void loadVolume(vtkSmartPointer<vtkImageData> imageData);
	void addToRecentFiles(const QString& filePath);
	void updateRecentFilesMenu();
	void loadRecentFiles();
	void saveRecentFiles();
	void openFile(const QString& filePath);

	Ui::MainWindow* ui;
	QStringList recentFiles;
	vtkSmartPointer<vtkImageData> currentImageData;
	vtkSmartPointer<vtkEventQtSlotConnect> vtkConnections;
	vtkSmartPointer<ImageLoader> imageLoader = nullptr;
	QProgressBar* progressBar = nullptr;
	bool defaultImageLoaded = false;
};

#endif // MAINWINDOW_H
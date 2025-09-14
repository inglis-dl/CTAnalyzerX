#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>
#include <vtkSmartPointer.h>

namespace Ui {
	class MainWindow;
}

class vtkImageData;
class ImageLoader;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow();

protected:
	void keyPressEvent(QKeyEvent* event) override;

private slots:
	void onActionOpen();
	void onActionSave();
	void onActionExit();
	void onActionAbout();
	void saveScreenshot();
	void clearRecentFiles();

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
	vtkSmartPointer<ImageLoader> imageLoader = nullptr;
	//LightBoxWidget* lightBoxWidget; // Assuming you have this elsewhere
};

#endif // MAINWINDOW_H
#pragma once

#include <QMainWindow>
#include <QProgressBar>

#include <vtkSmartPointer.h>

#include <limits>

class vtkImageData;
class ImageLoader;
class vtkEventQtSlotConnect;

namespace Ui { class MainWindow; }

class PrototypeMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit PrototypeMainWindow(QWidget* parent = nullptr);
	~PrototypeMainWindow() override;

	// Reads the sidecar JSON at `sidecarPath`, caches the threshold value,
	// loads the referenced crop image, and calls setImage().
	void loadFromSidecar(const QString& sidecarPath);

	// Schedules loadFromSidecar() to run after the event loop starts,
	// so the window is visible and can show progress during the load.
	void loadFromSidecarAsync(const QString& sidecarPath);

	// Sets the image on the volume view and applies window/level derived from
	// the image data: level = cached threshold, window = scalar standard deviation.
	void setImage(vtkImageData* image);

private slots:
	void onVtkStartEvent();
	void onVtkEndEvent();
	void onVtkProgressEvent();
	void showProgressStart();
	void showProgressValue(int percent);
	void showProgressEnd();

private:
	Ui::MainWindow* ui = nullptr;

	// Threshold cached from the sidecar JSON; used as the WL level in setImage().
	double m_threshold = std::numeric_limits<double>::quiet_NaN();

	vtkSmartPointer<ImageLoader>           m_imageLoader;
	vtkSmartPointer<vtkEventQtSlotConnect> m_vtkConnections;
	QProgressBar* m_progressBar = nullptr;
};
#pragma once

#include <QMainWindow>
#include <QProgressBar>

#include <vtkSmartPointer.h>

#include <limits>
#include <array>

class vtkActor;
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

	// PCA overlay actors (axis shafts + tip spheres). Cleared and rebuilt each setImage().
	// 3 axis lines + 6 sphere tip actors (both ends per axis) + 1 circumsphere wireframe.
	std::array<vtkSmartPointer<vtkActor>, 3>  m_axisActors;
	std::array<vtkSmartPointer<vtkActor>, 6>  m_tipActors;
	vtkSmartPointer<vtkActor>                 m_circumsphereActor;
	// Add this member to the private section of PrototypeMainWindow

// 3 ring actors for PCA overlay (one per principal axis)
	std::array<vtkSmartPointer<vtkActor>, 3> m_ringActors;

	// Removes all previously added PCA overlay actors from the VolumeView renderer.
	void clearPcaOverlay();
};
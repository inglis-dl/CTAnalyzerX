#ifndef VOLUMEVIEW_H
#define VOLUMEVIEW_H

#include <QFrame>
#include "ui_VolumeView.h"

#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkRenderWindow.h>

class vtkRenderer;
class vtkVolume;
class vtkVolumeProperty;
class vtkGPUVolumeRayCastMapper;
class vtkImagePlaneWidget;
class vtkGenericOpenGLRenderWindow;
class vtkImageShiftScale;

class VolumeView : public QFrame {
	Q_OBJECT
		Q_PROPERTY(bool slicePlanesVisible READ getSlicePlanesVisible WRITE setSlicePlanesVisible NOTIFY slicePlanesVisibleChanged)

public:
	explicit VolumeView(QWidget* parent = nullptr);

	void setImageData(vtkImageData* image);
	void updateSlicePlanes(int x, int y, int z);

	vtkRenderWindow* GetRenderWindow() const;

	// Property accessors
	bool getSlicePlanesVisible() const;
	void setSlicePlanesVisible(bool visible);

signals:
	// Signal emitted when a new image is set, with extents for each axis
	void imageExtentsChanged(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);
	void slicePlanesVisibleChanged(bool visible);

public slots:
	// Set cropping region for volume rendering
	void setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);

private:
	Ui::VolumeView ui;

	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow;

	vtkSmartPointer<vtkImageShiftScale> shiftScale;
	vtkSmartPointer<vtkVolume> volume;
	vtkSmartPointer<vtkVolumeProperty> property;
	vtkSmartPointer<vtkGPUVolumeRayCastMapper> mapper;

	vtkSmartPointer<vtkImageData> imageData;

	vtkSmartPointer<vtkImagePlaneWidget> yzPlane;
	vtkSmartPointer<vtkImagePlaneWidget> xzPlane;
	vtkSmartPointer<vtkImagePlaneWidget> xyPlane;

	bool imageInitialized = false;
	bool slicePlanesVisible = false;
};

#endif // VOLUMEVIEW_H


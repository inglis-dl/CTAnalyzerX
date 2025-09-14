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
	void updateSlicePlanes(int axial, int sagittal, int coronal);

	vtkRenderWindow* GetRenderWindow() const;

	// Property accessors
	bool getSlicePlanesVisible() const;
	void setSlicePlanesVisible(bool visible);

signals:
	// Signal emitted when a new image is set, with extents for each axis
	void imageExtentsChanged(int axialMin, int axialMax, int sagittalMin, int sagittalMax, int coronalMin, int coronalMax);
	void slicePlanesVisibleChanged(bool visible);

public slots:
	// Set cropping region for volume rendering
	void setCroppingRegion(int axialMin, int axialMax, int sagittalMin, int sagittalMax, int coronalMin, int coronalMax);

private:
	Ui::VolumeView ui;

	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow;

	vtkSmartPointer<vtkImageShiftScale> shiftScale;
	vtkSmartPointer<vtkVolume> volume;
	vtkSmartPointer<vtkVolumeProperty> property;
	vtkSmartPointer<vtkGPUVolumeRayCastMapper> mapper;

	vtkSmartPointer<vtkImageData> imageData;

	vtkSmartPointer<vtkImagePlaneWidget> axialPlane;
	vtkSmartPointer<vtkImagePlaneWidget> sagittalPlane;
	vtkSmartPointer<vtkImagePlaneWidget> coronalPlane;

	bool slicePlanesVisible = false;
};

#endif // VOLUMEVIEW_H


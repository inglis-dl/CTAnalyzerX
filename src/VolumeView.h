
#ifndef VOLUMEVIEW_H
#define VOLUMEVIEW_H

#include <QVTKOpenGLNativeWidget.h>
#include <vtkSmartPointer.h>

class vtkImageData;
class vtkVolume;
class vtkRenderer;
class vtkSmartVolumeMapper;
class vtkImagePlaneWidget;
class vtkRenderWindow;

class VolumeView : public QVTKOpenGLNativeWidget {
	Q_OBJECT
public:
	explicit VolumeView(QWidget* parent = nullptr);
	void setImageData(vtkImageData* image);
	void updateSlicePlanes(int axial, int sagittal, int coronal);
	void setCroppingRegion(int axialMin, int axialMax,
						   int sagittalMin, int sagittalMax,
						   int coronalMin, int coronalMax);
	void toggleSlicePlanes(bool visible);
	vtkRenderWindow* GetRenderWindow() const;


private:
	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkRenderWindow> renderWindow;
	vtkSmartPointer<vtkVolume> volume;
	vtkSmartPointer<vtkSmartVolumeMapper> mapper;
	vtkSmartPointer<vtkImageData> imageData;


	vtkSmartPointer<vtkImagePlaneWidget> axialPlane;
	vtkSmartPointer<vtkImagePlaneWidget> sagittalPlane;
	vtkSmartPointer<vtkImagePlaneWidget> coronalPlane;
	bool slicePlanesVisible = false;
};

#endif // VOLUMEVIEW_H


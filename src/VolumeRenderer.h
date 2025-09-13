#ifndef VOLUMERENDERER_H
#define VOLUMERENDERER_H

#include <vtkSmartPointer.h>

class vtkImageData;
class vtkRenderer;
class vtkRenderWindow;
class vtkVolume;
class vtkVolumeProperty;
class vtkGPUVolumeRayCastMapper;

class VolumeRenderer
{
public:
	VolumeRenderer();
	~VolumeRenderer();

	void setInputData(vtkImageData* image);
	void render(vtkRenderWindow* renderWindow);

private:
	void setupVolume();

	vtkSmartPointer<vtkImageData> inputImage;
	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkVolume> volume;
	vtkSmartPointer<vtkVolumeProperty> volumeProperty;
	vtkSmartPointer<vtkGPUVolumeRayCastMapper> volumeMapper;
};

#endif // VOLUMERENDERER_H



#include "VolumeRenderer.h"

#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>

VolumeRenderer::VolumeRenderer()
	: renderer(vtkSmartPointer<vtkRenderer>::New()),
	volume(vtkSmartPointer<vtkVolume>::New()),
	volumeProperty(vtkSmartPointer<vtkVolumeProperty>::New()),
	volumeMapper(vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New())
{
}

VolumeRenderer::~VolumeRenderer() = default;

void VolumeRenderer::setInputData(vtkImageData* image)
{
	inputImage = image;
	volumeMapper->SetInputData(inputImage);
	setupVolume();
}

void VolumeRenderer::setupVolume()
{
	// Opacity mapping
	vtkSmartPointer<vtkPiecewiseFunction> opacity = vtkSmartPointer<vtkPiecewiseFunction>::New();
	opacity->AddPoint(0, 0.0);
	opacity->AddPoint(255, 1.0);

	// Color mapping
	vtkSmartPointer<vtkColorTransferFunction> color = vtkSmartPointer<vtkColorTransferFunction>::New();
	color->AddRGBPoint(0.0, 0.0, 0.0, 0.0);
	color->AddRGBPoint(255.0, 1.0, 1.0, 1.0);

	volumeProperty->SetColor(color);
	volumeProperty->SetScalarOpacity(opacity);
	volumeProperty->ShadeOn();
	volumeProperty->SetInterpolationTypeToLinear();

	volume->SetMapper(volumeMapper);
	volume->SetProperty(volumeProperty);
}

void VolumeRenderer::render(vtkRenderWindow* renderWindow)
{
	renderer->RemoveAllViewProps();
	renderer->AddVolume(volume);
	renderer->ResetCamera();

	renderWindow->AddRenderer(renderer);
	renderWindow->Render();
}

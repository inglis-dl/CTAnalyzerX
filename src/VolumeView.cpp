
#include "VolumeView.h"
#include <vtkSmartVolumeMapper.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>

#include <vtkImageData.h>
#include <vtkVolume.h>
#include <vtkRenderer.h>
#include <vtkSmartVolumeMapper.h>
#include <vtkImagePlaneWidget.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkVolumeProperty.h>
#include <vtkGenericOpenGLRenderWindow.h>

VolumeView::VolumeView(QWidget* parent)
	: QVTKOpenGLNativeWidget(parent) {

	renderer = vtkSmartPointer<vtkRenderer>::New();
	renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
	renderWindow->AddRenderer(renderer);
	setRenderWindow(renderWindow);
}

void VolumeView::updateSlicePlanes(int axial, int sagittal, int coronal) {
	// Optional: show slice planes or crosshairs
}

void VolumeView::setCroppingRegion(int axialMin, int axialMax,
								   int sagittalMin, int sagittalMax,
								   int coronalMin, int coronalMax) {
	if (!mapper || !imageData) return;

	mapper->SetCropping(true);
	mapper->SetCroppingRegionPlanes(
		sagittalMin, sagittalMax,  // X (sagittal)
		coronalMin, coronalMax,    // Y (coronal)
		axialMin, axialMax         // Z (axial)
	);
	this->GetRenderWindow()->Render();
}

void VolumeView::setImageData(vtkImageData* image) {
	imageData = image;

	mapper = vtkSmartPointer<vtkSmartVolumeMapper>::New();
	mapper->SetInputData(image);
	mapper->SetCropping(false);

	auto property = vtkSmartPointer<vtkVolumeProperty>::New();
	property->ShadeOff();
	property->SetInterpolationTypeToLinear();

	volume = vtkSmartPointer<vtkVolume>::New();
	volume->SetMapper(mapper);
	volume->SetProperty(property);

	renderer->AddVolume(volume);
	renderer->ResetCamera();

	auto interactor = this->GetRenderWindow()->GetInteractor();

	axialPlane = vtkSmartPointer<vtkImagePlaneWidget>::New();
	axialPlane->SetInteractor(interactor);
	axialPlane->SetInputData(image);
	axialPlane->SetPlaneOrientationToZAxes();
	axialPlane->SetSliceIndex(image->GetExtent()[5] / 2);
	axialPlane->SetEnabled(slicePlanesVisible);

	sagittalPlane = vtkSmartPointer<vtkImagePlaneWidget>::New();
	sagittalPlane->SetInteractor(interactor);
	sagittalPlane->SetInputData(image);
	sagittalPlane->SetPlaneOrientationToXAxes();
	sagittalPlane->SetSliceIndex(image->GetExtent()[1] / 2);
	sagittalPlane->SetEnabled(slicePlanesVisible);

	coronalPlane = vtkSmartPointer<vtkImagePlaneWidget>::New();
	coronalPlane->SetInteractor(interactor);
	coronalPlane->SetInputData(image);
	coronalPlane->SetPlaneOrientationToYAxes();
	coronalPlane->SetSliceIndex(image->GetExtent()[3] / 2);
	coronalPlane->SetEnabled(slicePlanesVisible);

	this->GetRenderWindow()->Render();
}

void VolumeView::toggleSlicePlanes(bool visible) {
	slicePlanesVisible = visible;
	if (axialPlane) axialPlane->SetEnabled(visible);
	if (sagittalPlane) sagittalPlane->SetEnabled(visible);
	if (coronalPlane) coronalPlane->SetEnabled(visible);
	this->GetRenderWindow()->Render();
}

vtkRenderWindow* VolumeView::GetRenderWindow() const {
	return QVTKOpenGLNativeWidget::renderWindow();
}


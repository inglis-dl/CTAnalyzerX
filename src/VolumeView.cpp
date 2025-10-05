#include "VolumeView.h"
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkImagePlaneWidget.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageShiftScale.h>
#include <QVTKOpenGLNativeWidget.h>

VolumeView::VolumeView(QWidget* parent)
	: QFrame(parent)
{
	ui.setupUi(this);

	renderer = vtkSmartPointer<vtkRenderer>::New();
	renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
	renderWindow->AddRenderer(renderer);
	ui.renderArea->setRenderWindow(renderWindow);

	renderer->GradientBackgroundOn();
	double color[3] = { 0., 0., 0. };
	renderer->SetBackground(color);  // black (lower part of gradient)
	color[2] = 1.;
	renderer->SetBackground2(color);  // blue (upper part of gradient)

	shiftScale = vtkSmartPointer<vtkImageShiftScale>::New();
	shiftScale->SetOutputScalarTypeToUnsignedShort();
	shiftScale->ClampOverflowOn();

	mapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();

	property = vtkSmartPointer<vtkVolumeProperty>::New();
	property->ShadeOff();
	property->SetInterpolationTypeToLinear();

	volume = vtkSmartPointer<vtkVolume>::New();
	volume->SetMapper(mapper);
	volume->SetProperty(property);
}

bool VolumeView::getSlicePlanesVisible() const {
	return slicePlanesVisible;
}

void VolumeView::setSlicePlanesVisible(bool visible) {
	if (slicePlanesVisible == visible)
		return;
	slicePlanesVisible = visible;

	// Mutually exclusive: show either volume or planes, not both
	if (visible) {
		// Hide volume rendering
		if (volume && renderer->HasViewProp(volume)) {
			renderer->RemoveVolume(volume);
		}
		// Show planes
		if (axialPlane) axialPlane->SetEnabled(true);
		if (sagittalPlane) sagittalPlane->SetEnabled(true);
		if (coronalPlane) coronalPlane->SetEnabled(true);
	}
	else {
		// Hide planes
		if (axialPlane) axialPlane->SetEnabled(false);
		if (sagittalPlane) sagittalPlane->SetEnabled(false);
		if (coronalPlane) coronalPlane->SetEnabled(false);
		// Show volume rendering
		if (volume && !renderer->HasViewProp(volume)) {
			renderer->AddVolume(volume);
		}
	}
	emit slicePlanesVisibleChanged(slicePlanesVisible);

	renderWindow->Render();
}

void VolumeView::setImageData(vtkImageData* image) {
	if (!image) return;
	imageData = image;

	double range[2];
	image->GetScalarRange(range);

	// Insert vtkImageShiftScale to ensure unsigned short input for the mapper
	shiftScale->SetInputData(image);

	// Configure shift/scale based on input scalar type
	int scalarType = image->GetScalarType();
	double shift = 0.0;
	double scale = 1.0;

	switch (scalarType) {
		case VTK_CHAR:
		// Map [-128,127] to [0,255]
		shift = 128.0;
		scale = 1.0;
		break;
		case VTK_SIGNED_CHAR:
		shift = 128.0;
		scale = 1.0;
		break;
		case VTK_UNSIGNED_CHAR:
		shift = 0.0;
		scale = 1.0;
		break;
		case VTK_SHORT:
		// Map [-32768,32767] to [0,65535]
		shift = 32768.0;
		scale = 1.0;
		break;
		case VTK_UNSIGNED_SHORT:
		shift = 0.0;
		scale = 1.0;
		break;
		case VTK_INT:
		// Map full int range to [0,65535]
		shift = 2147483648.0;
		scale = 65535.0 / 4294967295.0;
		break;
		case VTK_UNSIGNED_INT:
		shift = 0.0;
		scale = 65535.0 / 4294967295.0;
		break;
		case VTK_FLOAT:
		case VTK_DOUBLE: {
			// Map min/max to [0,65535]
			shift = -range[0];
			double diff = range[1] - range[0];
			scale = (diff != 0.0) ? (65535.0 / diff) : 1.0;
			break;
		}
		default:
		shift = 0.0;
		scale = 1.0;
		break;
	}
	shiftScale->SetShift(shift);
	shiftScale->SetScale(scale);
	shiftScale->Update();

	if (shiftScale->GetOutputPort() && mapper) {
		mapper->SetInputConnection(shiftScale->GetOutputPort());
	}

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

	int extent[6];
	image->GetExtent(extent);
	emit imageExtentsChanged(
		extent[4], extent[5],
		extent[0], extent[1],
		extent[2], extent[3]
	);

	renderWindow->Render();
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

	renderWindow->Render();
}

void VolumeView::updateSlicePlanes(int axial, int sagittal, int coronal) {
	// Optional: show slice planes or crosshairs
}

vtkRenderWindow* VolumeView::GetRenderWindow() const {
	return renderWindow;
}
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

	auto interactor = renderWindow->GetInteractor();

	yzPlane = vtkSmartPointer<vtkImagePlaneWidget>::New();
	yzPlane->SetInteractor(interactor);

	xzPlane = vtkSmartPointer<vtkImagePlaneWidget>::New();
	xzPlane->SetInteractor(interactor);

	xyPlane = vtkSmartPointer<vtkImagePlaneWidget>::New();
	xyPlane->SetInteractor(interactor);

	imageInitialized = false;
}

bool VolumeView::getSlicePlanesVisible() const {
	return slicePlanesVisible;
}

void VolumeView::setSlicePlanesVisible(bool visible) {

	bool modified = (slicePlanesVisible != visible);
	slicePlanesVisible = visible;

	// Mutually exclusive: show either volume or planes, not both
	if (visible) {
		// Hide volume rendering
		if (volume && renderer->HasViewProp(volume)) {
			renderer->RemoveVolume(volume);
		}
		// Show planes
		if (yzPlane) yzPlane->SetEnabled(true);
		if (xzPlane) xzPlane->SetEnabled(true);
		if (xyPlane) xyPlane->SetEnabled(true);

		yzPlane->InteractionOff();
		xzPlane->InteractionOff();
		xyPlane->InteractionOff();
	}
	else {
		// Hide planes
		if (yzPlane) yzPlane->SetEnabled(false);
		if (xzPlane) xzPlane->SetEnabled(false);
		if (xyPlane) xyPlane->SetEnabled(false);
		// Show volume rendering
		if (volume && !renderer->HasViewProp(volume)) {
			renderer->AddVolume(volume);
		}
	}
	if (modified)
		emit slicePlanesVisibleChanged(slicePlanesVisible);

	renderWindow->Render();
}

void VolumeView::setImageData(vtkImageData* image) {
	if (!image) return;
	imageData = image;

	double range[2];
	imageData->GetScalarRange(range);

	// Insert vtkImageShiftScale to ensure unsigned short input for the mapper
	shiftScale->SetInputData(imageData);

	// Configure shift/scale based on input scalar type
	int scalarType = imageData->GetScalarType();
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
	}
	shiftScale->SetShift(shift);
	shiftScale->SetScale(scale);
	shiftScale->Update();

	yzPlane->SetInputData(shiftScale->GetOutput());
	xzPlane->SetInputData(shiftScale->GetOutput());
	xyPlane->SetInputData(shiftScale->GetOutput());

	if (!imageInitialized) {
		mapper->SetInputConnection(shiftScale->GetOutputPort());

		yzPlane->SetPlaneOrientationToXAxes();
		xzPlane->SetPlaneOrientationToYAxes();
		xyPlane->SetPlaneOrientationToZAxes();

		imageInitialized = true;
	}

	renderer->ResetCamera();

	yzPlane->SetSliceIndex(imageData->GetExtent()[1] / 2);
	yzPlane->SetEnabled(slicePlanesVisible);

	xzPlane->SetSliceIndex(imageData->GetExtent()[3] / 2);
	xzPlane->SetEnabled(slicePlanesVisible);

	xyPlane->SetSliceIndex(imageData->GetExtent()[5] / 2);
	xyPlane->SetEnabled(slicePlanesVisible);


	int extent[6];
	imageData->GetExtent(extent);
	emit imageExtentsChanged(
		extent[0], extent[1],
		extent[2], extent[3],
		extent[4], extent[5]
	);

	setSlicePlanesVisible(slicePlanesVisible);

	renderWindow->Render();
}

void VolumeView::setCroppingRegion(int xMin, int xMax,
								   int yMin, int yMax,
								   int zMin, int zMax) {
	if (!mapper || !imageData) return;

	mapper->SetCropping(true);
	mapper->SetCroppingRegionPlanes(
		xMin, xMax,
		yMin, yMax,
		zMin, zMax
	);

	renderWindow->Render();
}

void VolumeView::updateSlicePlanes(int x, int y, int z) {
	yzPlane->SetSliceIndex(x);
	xzPlane->SetSliceIndex(y);
	xyPlane->SetSliceIndex(z);

	renderWindow->Render();
}

vtkRenderWindow* VolumeView::GetRenderWindow() const {
	return renderWindow;
}
#include "SliceView.h"
#include "ui_SliceView.h"
#include <QWheelEvent>
#include <QSlider>
#include <QLabel>
#include <algorithm>

#include <vtkRenderWindowInteractor.h>
#include <vtkImageData.h>
#include <vtkCamera.h>
#include <vtkImageMapToWindowLevelColors.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkInformation.h>
#include <vtkImageProperty.h>
#include <vtkEventQtSlotConnect.h>

SliceView::SliceView(QWidget* parent, Orientation orientation)
	: QFrame(parent), orientation(orientation), currentSlice(0), minSlice(0), maxSlice(0)
{
	ui = new Ui::SliceView();
	ui->setupUi(this);

	renderer = vtkSmartPointer<vtkRenderer>::New();

	renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();

	interactorStyle = vtkSmartPointer<vtkInteractorStyleImage>::New();
	interactorStyle->SetInteractionModeToImage2D();
	interactorStyle->SetDefaultRenderer(renderer);
	interactorStyle->AutoAdjustCameraClippingRangeOn();

	shiftScaleFilter = vtkSmartPointer<vtkImageShiftScale>::New();
	windowLevelFilter = vtkSmartPointer<vtkImageMapToWindowLevelColors>::New();

	// Initialize slice mapper and image slice
	sliceMapper = vtkSmartPointer<vtkImageSliceMapper>::New();
	imageSlice = vtkSmartPointer<vtkImageSlice>::New();
	imageSlice->SetMapper(sliceMapper);
	imageProperty = imageSlice->GetProperty();
	imageProperty->SetInterpolationTypeToLinear();
	imageSlice->SetProperty(imageProperty);


	// Enable automatic camera-facing for the slice
	sliceMapper->SliceFacesCameraOff();
	sliceMapper->SliceAtFocalPointOff();

	renderWindow->AddRenderer(renderer);
	ui->renderArea->setRenderWindow(renderWindow);

	if (auto iren = renderWindow->GetInteractor()) {
		iren->SetInteractorStyle(interactorStyle);
	}

	renderer->GradientBackgroundOn();
	double color[3] = { 0., 0., 0. };
	renderer->SetBackground(color);  // black (lower part of gradient)
	color[2] = 1.;
	renderer->SetBackground2(color);  // blue (upper part of gradient)

	this->qvtkConnection = vtkSmartPointer<vtkEventQtSlotConnect>::New();
	this->qvtkConnection->Connect(interactorStyle, vtkCommand::LeftButtonPressEvent,
		this, SLOT(trapSpin(vtkObject*)));

	connect(ui->sliderSlicePosition, &QSlider::valueChanged, this, &SliceView::setSliceIndex);
	connect(this, &SliceView::sliceChanged, this, [this](int value) {
		ui->labelSliceInfo->setText(QString("Slice: %1").arg(value));
	});
}

SliceView::~SliceView() {
	delete ui;
}

void SliceView::setImageData(vtkImageData* image) {
	if (!image) return;
	imageData = image;

	// Set the input image to the window/level filter

	shiftScaleFilter->SetInputData(imageData);
	shiftScaleFilter->SetOutputScalarTypeToUnsignedChar();

	double range[2];
	imageData->GetScalarRange(range);

	shiftScaleFilter->SetShift(-range[0]);
	if (VTK_UNSIGNED_CHAR_MAX > (range[1] - range[0]))
	{
		shiftScaleFilter->SetScale(1);
	}
	else
	{
		shiftScaleFilter->SetScale(VTK_UNSIGNED_CHAR_MAX / (range[1] - range[0]));
	}

	windowLevelFilter->SetInputConnection(shiftScaleFilter->GetOutputPort());

	// Set the output of the window/level filter to the slice mapper
	sliceMapper->SetInputConnection(windowLevelFilter->GetOutputPort());

	const int components = image->GetNumberOfScalarComponents();
	switch (components)
	{
		case 1:
		windowLevelFilter->SetActiveComponent(0);
		windowLevelFilter->PassAlphaToOutputOff();
		windowLevelFilter->SetOutputFormatToLuminance();
		break;
		case 2:
		case 3:
		windowLevelFilter->SetOutputFormatToRGB();
		windowLevelFilter->PassAlphaToOutputOff();
		break;
		case 4:
		windowLevelFilter->SetOutputFormatToRGBA();
		windowLevelFilter->PassAlphaToOutputOn();
		break;
	}

	// Add imageActor to renderer only the first time a valid image is set
	if (!imageInitialized) {
		renderer->AddViewProp(imageSlice);
		imageInitialized = true;
	}

	windowLevelFilter->Modified();
	windowLevelFilter->Update();

	updateSliceRange();

	updateCamera(); // update the camera for the specified orientation

	setSliceIndex((minSlice + maxSlice) / 2);
}

void SliceView::updateCamera() {
	if (!imageData)	return;

	const double* origin = imageData->GetOrigin();
	const double* spacing = imageData->GetSpacing();
	int extent[6];
	imageData->GetExtent(extent);

	int w = orientation;
	int u = 0;
	int v = 1;
	switch (w)
	{
		case 0: u = 1; v = 2; break;  // YZ
		case 1: u = 0; v = 2; break;  // XZ
		case 2: u = 0; v = 1; break;  // XY
	}

	// compute the bounds of the first slice of the image for this orientation
	double bounds[6];
	bounds[2 * u] = origin[u] + spacing[u] * extent[2 * u];
	bounds[2 * u + 1] = origin[u] + spacing[u] * extent[2 * u + 1];
	bounds[2 * v] = origin[v] + spacing[v] * extent[2 * v];
	bounds[2 * v + 1] = origin[v] + spacing[v] * extent[2 * v + 1];
	bounds[2 * w] = origin[w] + spacing[w] * extent[2 * w];
	bounds[2 * w + 1] = bounds[2 * w];

	double fpt[3];
	double pos[3];
	double vup[3] = { 0.0, 0.0, 0.0 };
	double vpn[3] = { 0.0, 0.0, 0.0 };
	vup[v] = 1.0;
	vpn[w] = 1.0;

	fpt[u] = pos[u] = origin[u] + 0.5 * spacing[u] * (extent[2 * u] + extent[2 * u + 1]);
	fpt[v] = pos[v] = origin[v] + 0.5 * spacing[v] * (extent[2 * v] + extent[2 * v + 1]);
	fpt[w] = origin[w] + spacing[w] * (1 == w ? extent[2 * w + 1] : extent[2 * w]);
	pos[w] = fpt[w] + vpn[w] * spacing[w];

	auto camera = renderer->GetActiveCamera();
	camera->SetFocalPoint(fpt);
	camera->SetPosition(pos);
	camera->SetViewUp(vup);

	renderer->ResetCamera(bounds);

	currentSlice = static_cast<int>(extent[2 * w]);
}

void SliceView::updateSliceRange() {
	minSlice = sliceMapper->GetSliceNumberMinValue();
	maxSlice = sliceMapper->GetSliceNumberMaxValue();

	ui->sliderSlicePosition->setMinimum(minSlice);
	ui->sliderSlicePosition->setMaximum(maxSlice);
}

int SliceView::getMinSliceIndex() const {
	return minSlice;
}

int SliceView::getMaxSliceIndex() const {
	return maxSlice;
}

void SliceView::setSliceIndex(int index) {

	int clampedIndex = std::clamp(index, minSlice, maxSlice);

	//if (currentSlice != clampedIndex) {
	currentSlice = clampedIndex;
	ui->sliderSlicePosition->blockSignals(true);
	ui->sliderSlicePosition->setValue(currentSlice);
	ui->sliderSlicePosition->blockSignals(false);

	updateSlice();

	emit sliceChanged(currentSlice);
	//}
}

int SliceView::getSliceIndex() const {
	return currentSlice;
}

void SliceView::updateSlice() {
	if (!imageData) return;

	sliceMapper->SetSliceNumber(currentSlice);
	sliceMapper->Update();

	int u, v, w = orientation;
	switch (w)
	{
		case 0: u = 1; v = 2; break;
		case 1: u = 0; v = 2; break;
		case 2: u = 0; v = 1; break;
	}

	const double* origin = imageData->GetOrigin();
	const double* spacing = imageData->GetSpacing();

	auto cam = renderer->GetActiveCamera();
	if (cam)
	{
		double fpt[3];
		cam->GetFocalPoint(fpt);
		fpt[w] = origin[w] + spacing[w] * currentSlice;

		const double* vpn = cam->GetViewPlaneNormal();
		const double d = cam->GetDistance();
		cam->SetFocalPoint(fpt);

		double pos[3];
		pos[u] = fpt[u];
		pos[v] = fpt[v];
		pos[w] = fpt[w] + d * vpn[w];
		cam->SetPosition(pos);
	}

	renderWindow->Render();
}

void SliceView::setOrientation(Orientation newOrientation) {
	if (orientation != newOrientation)
	{
		orientation = newOrientation;

		// Set the orientation on the mapper before querying min/max
		switch (orientation) {
			case YZ: sliceMapper->SetOrientationToX(); break; // z-y plane x normal
			case XZ: sliceMapper->SetOrientationToY(); break; // x-z plane y normal
			case XY: sliceMapper->SetOrientationToZ(); break; // x-y plane z normal
		}
		emit orientationChanged(orientation);
	}
}

SliceView::Orientation SliceView::getOrientation() const {
	return orientation;
}

vtkRenderWindow* SliceView::GetRenderWindow() const {
	return renderWindow;
}

void SliceView::setOrientationToXY()
{
	setOrientation(XY);
}

void SliceView::setOrientationToYZ()
{
	setOrientation(YZ);
}

void SliceView::setOrientationToXZ()
{
	setOrientation(XZ);
}

void SliceView::setInterpolation(Interpolation newInterpolation)
{
	if (newInterpolation != interpolation) {
		interpolation = newInterpolation;
		switch (interpolation) {
			case Nearest:
			imageProperty->SetInterpolationTypeToNearest();
			break;
			case Linear:
			imageProperty->SetInterpolationTypeToLinear();
			break;
			case Cubic:
			imageProperty->SetInterpolationTypeToCubic();
			break;
		}
		renderWindow->Render();
		emit interpolationChanged(interpolation);
	}
}

SliceView::Interpolation SliceView::getInterpolation() const {
	return interpolation;
}

void SliceView::setInterpolationToNearest()
{
	setInterpolation(Nearest);
}

void SliceView::setInterpolationToLinear()
{
	setInterpolation(Linear);
}

void SliceView::setInterpolationToCubic()
{
	setInterpolation(Cubic);
}

void SliceView::trapSpin(vtkObject* obj)
{
	auto style = vtkInteractorStyleImage::SafeDownCast(obj);
	if (style->GetInteractor()->GetControlKey())
		return;

	style->OnLeftButtonDown();
}
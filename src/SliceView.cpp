#include "SliceView.h"
#include "SceneFrameWidget.h"
#include <QWidget>
#include <QVBoxLayout>
#include "ui_SliceView.h"
#include <vtkRenderWindow.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkInteractorStyleImage.h>
#include <vtkImageData.h>
#include <vtkCamera.h>
#include <vtkImageMapToWindowLevelColors.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkInformation.h>
#include <vtkImageProperty.h>
#include <vtkEventQtSlotConnect.h>

SliceView::SliceView(QWidget* parent, ViewOrientation initialOrientation)
	: SceneFrameWidget(parent)
	, ui(new Ui::SliceView)
{
	// Match vtkKWImageFrame defaults
	setAllowClose(false);
	setAllowChangeTitle(false);
	setTitleBarVisible(true);
	setSelectionListVisible(true);

	// Install Designer UI into frame body
	auto* content = new QFrame(this);
	ui->setupUi(content);
	setSceneContent(content);

	// Eliminate all paddings and spacings inside the slice view content so that:
	// - the render area touches the title bar (no gap under header),
	// - the slider touches the render area (no gap above slider).
	if (auto* rootLayout = content->layout()) {
		rootLayout->setContentsMargins(0, 0, 0, 0);
		rootLayout->setSpacing(0);
	}
	// Also apply to any nested layouts created by Designer
	const auto allLayouts = content->findChildren<QLayout*>();
	for (QLayout* lay : allLayouts) {
		if (!lay) continue;
		lay->setContentsMargins(0, 0, 0, 0);
		lay->setSpacing(0);
	}

	// Existing pipeline initialization (unchanged)
	renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
	renderer = vtkSmartPointer<vtkRenderer>::New();
	renderWindow->AddRenderer(renderer);

	// Use parallel projection for 2D imaging (no perspective distortion)
	if (auto* cam = renderer->GetActiveCamera()) {
		cam->ParallelProjectionOn();
	}

	ui->renderArea->setRenderWindow(renderWindow); // mount VTK window into Qt widget
	setFocusProxy(ui->renderArea);                 // keyboard/mouse go to the view

	// Set interactor style after the widget created one for the renderWindow
	if (auto* iren = renderWindow->GetInteractor()) {
		interactorStyle = vtkSmartPointer<vtkInteractorStyleImage>::New();
		interactorStyle->SetInteractionModeToImage2D();
		interactorStyle->SetDefaultRenderer(renderer);
		interactorStyle->AutoAdjustCameraClippingRangeOn();
		iren->SetInteractorStyle(interactorStyle);
	}

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

	updateCamera();

	// Apply initial orientation via base API (keeps menu/actions synced)
	switch (initialOrientation) {
		case VIEW_ORIENTATION_YZ: setViewOrientationToYZ(); break;
		case VIEW_ORIENTATION_XZ: setViewOrientationToXZ(); break;
		case VIEW_ORIENTATION_XY:
		default: setViewOrientationToXY(); break;
	}

	updateActionEnableStates();
}

SliceView::~SliceView()
{
	delete ui;
}

vtkRenderWindow* SliceView::getRenderWindow() const
{
	return renderWindow;
}

void SliceView::resetCamera()
{
	if (renderer) {
		renderer->ResetCamera();
		render();
	}
}

void SliceView::orthogonalizeView()
{
	updateCamera();
	render();
}

void SliceView::flipHorizontal()
{
	// If your view has horizontal flip, apply it here (e.g., camera view-up or actor transform)
	// Then render(); otherwise leave empty or disable via canFlipHorizontal
}

void SliceView::flipVertical()
{
	// If your view has vertical flip, apply it here and render()
}

void SliceView::rotateCamera(double degrees)
{
	if (!renderer) return;
	if (auto* cam = renderer->GetActiveCamera()) {
		cam->Roll(degrees);
		render();
	}
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

	// Ensure mapper orientation matches current view as soon as input exists
	switch (m_viewOrientation) {
		case VIEW_ORIENTATION_YZ: sliceMapper->SetOrientationToX(); break; // z-y plane x normal
		case VIEW_ORIENTATION_XZ: sliceMapper->SetOrientationToY(); break; // x-z plane y normal
		case VIEW_ORIENTATION_XY:
		default:                  sliceMapper->SetOrientationToZ(); break; // x-y plane z normal
	}

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
	if (!m_imageInitialized) {
		renderer->AddViewProp(imageSlice);
		m_imageInitialized = true;
	}

	windowLevelFilter->Modified();
	windowLevelFilter->Update();

	updateSliceRange();

	// Set camera and show a valid slice immediately (center)
	updateCamera();
	setSliceIndex((m_minSlice + m_maxSlice) / 2);
}

void SliceView::updateCamera() {
	if (!imageData)	return;

	const double* origin = imageData->GetOrigin();
	const double* spacing = imageData->GetSpacing();
	int extent[6];
	imageData->GetExtent(extent);

	int w = m_viewOrientation;
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
	bounds[2 * w + 1] = bounds[2 * w]; // zero thickness in view direction

	double fpt[3];
	double pos[3];
	double vup[3] = { 0.0, 0.0, 0.0 };
	double vpn[3] = { 0.0, 0.0, 0.0 };
	vup[v] = 1.0;  // up is the second in-plane axis
	vpn[w] = 1.0;  // look along the view-normal axis

	fpt[u] = pos[u] = origin[u] + 0.5 * spacing[u] * (extent[2 * u] + extent[2 * u + 1]);
	fpt[v] = pos[v] = origin[v] + 0.5 * spacing[v] * (extent[2 * v] + extent[2 * v + 1]);
	fpt[w] = origin[w] + spacing[w] * (1 == w ? extent[2 * w + 1] : extent[2 * w]);
	pos[w] = fpt[w] + vpn[w] * spacing[w];

	auto camera = renderer->GetActiveCamera();
	camera->ParallelProjectionOn(); // ensure 2D projection
	camera->SetFocalPoint(fpt);
	camera->SetPosition(pos);
	camera->SetViewUp(vup);
	camera->OrthogonalizeViewUp();  // guard against accumulated roll

	// Fit the slice to the viewport and set sensible distance/scale
	renderer->ResetCamera(bounds);
	renderer->ResetCameraClippingRange(bounds);

	// Move current slice to the start of the axis (will be centered later)
	m_currentSlice = static_cast<int>(extent[2 * w]);
}

void SliceView::setViewOrientation(SceneFrameWidget::ViewOrientation orientation)
{
	if (m_viewOrientation == orientation)
		return;

	m_viewOrientation = orientation;

	// If no image/pipeline yet, just broadcast and return (avoid VTK errors).
	if (!imageData || !sliceMapper || sliceMapper->GetNumberOfInputConnections(0) == 0) {
		emit viewOrientationChanged(orientation);
		return;
	}

	// Update mapper orientation for the selected plane
	switch (m_viewOrientation) {
		case VIEW_ORIENTATION_YZ: sliceMapper->SetOrientationToX(); break; // z-y plane x normal
		case VIEW_ORIENTATION_XZ: sliceMapper->SetOrientationToY(); break; // x-z plane y normal
		case VIEW_ORIENTATION_XY:
		default: sliceMapper->SetOrientationToZ(); break; // x-y plane z normal
	}

	// Recompute slice range and camera, then pick a visible slice (center)
	updateSliceRange();
	updateCamera();
	setSliceIndex((m_minSlice + m_maxSlice) / 2); // also triggers render()

	emit viewOrientationChanged(orientation);
}

void SliceView::updateSliceRange() {
	// Guard against mapper without input to avoid VTK errors
	if (!sliceMapper || sliceMapper->GetNumberOfInputConnections(0) == 0)
		return;

	// Make sure information is current so min/max are valid
	sliceMapper->Update();

	m_minSlice = sliceMapper->GetSliceNumberMinValue();
	m_maxSlice = sliceMapper->GetSliceNumberMaxValue();

	ui->sliderSlicePosition->setMinimum(m_minSlice);
	ui->sliderSlicePosition->setMaximum(m_maxSlice);
}

void SliceView::updateSlice() {
	if (!imageData) return;

	sliceMapper->SetSliceNumber(m_currentSlice);
	sliceMapper->Update();

	int u = 0, v = 1, w = m_viewOrientation;
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
		fpt[w] = origin[w] + spacing[w] * m_currentSlice;

		const double* vpn = cam->GetViewPlaneNormal();
		const double d = cam->GetDistance();
		cam->SetFocalPoint(fpt);

		double pos[3];
		pos[u] = fpt[u];
		pos[v] = fpt[v];
		pos[w] = fpt[w] + d * vpn[w];
		cam->SetPosition(pos);
	}

	renderer->ResetCameraClippingRange(); // ensure slice is not clipped
	render();                    // let SceneFrameWidget coalesce
}

int SliceView::getMinSliceIndex() const {
	return m_minSlice;
}

int SliceView::getMaxSliceIndex() const {
	return m_maxSlice;
}

void SliceView::setSliceIndex(int index) {

	int clampedIndex = std::clamp(index, m_minSlice, m_maxSlice);

	m_currentSlice = clampedIndex;
	ui->sliderSlicePosition->blockSignals(true);
	ui->sliderSlicePosition->setValue(m_currentSlice);
	ui->sliderSlicePosition->blockSignals(false);

	updateSlice();

	emit sliceChanged(m_currentSlice);
}

int SliceView::getSliceIndex() const {
	return m_currentSlice;
}

void SliceView::setInterpolation(Interpolation newInterpolation)
{
	if (newInterpolation != m_interpolation) {
		m_interpolation = newInterpolation;
		switch (m_interpolation) {
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
		emit interpolationChanged(m_interpolation);
	}
}

SliceView::Interpolation SliceView::getInterpolation() const {
	return m_interpolation;
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
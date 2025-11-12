#include "ImageFrameWidget.h"

#include <algorithm>
#include <cmath>

#include <vtkAlgorithm.h>
#include <vtkCamera.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkImageData.h>
#include <vtkImageShiftScale.h>
#include <vtkMath.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkCubeSource.h>
#include <vtkLineSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkPropAssembly.h>
#include <vtkAlgorithmOutput.h>
#include <vtkDataObject.h>


ImageFrameWidget::ImageFrameWidget(QWidget* parent)
	: SelectionFrameWidget(parent)
{
	setAllowClose(false);
	setAllowChangeTitle(false);
	setTitleBarVisible(true);
	setSelectionListVisible(true);

	// Create the shared render surface
	m_renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
	m_renderer = vtkSmartPointer<vtkRenderer>::New();
	// Main renderer is layer 0
	m_renderWindow->SetNumberOfLayers(2);
	m_renderWindow->AddRenderer(m_renderer);
	m_renderer->SetLayer(0);

	// Create an overlay renderer (layer 1) for the orientation marker.
	m_orientationRenderer = vtkSmartPointer<vtkRenderer>::New();
	m_orientationRenderer->SetLayer(1);
	m_orientationRenderer->InteractiveOff();
	// Default viewport (hidden until we initialize marker); will be placed in a corner later.
	m_orientationRenderer->SetViewport(0.0, 0.0, 0.0, 0.0);
	m_renderWindow->AddRenderer(m_orientationRenderer);

	// Reasonable defaults; derived classes may further customize
	initializeRendererDefaults();

	m_shiftScaleFilter = vtkSmartPointer<vtkImageShiftScale>::New();
	m_shiftScaleFilter->SetOutputScalarTypeToUnsignedShort();
	m_shiftScaleFilter->ClampOverflowOn();

	// Orientation marker will be initialized lazily when an interactor is present.
	m_orientationWidget = nullptr;
	m_orientationAssembly = nullptr;
	m_orientationCubeActor = nullptr;
}

ImageFrameWidget::~ImageFrameWidget() = default;

void ImageFrameWidget::setImageData(vtkImageData* image)
{
	if (!image) return;

	m_shiftScaleFilter->SetInputData(image);
	m_shiftScaleFilter->Update();
	m_imageData = image;

	updateData();
}

void ImageFrameWidget::setInputConnection(vtkAlgorithmOutput* port, bool newImg)
{
	// Avoid duplicate wiring: if the shift/scale filter is already connected to the same port,
	// do nothing. This prevents the "called twice" behavior when multiple owners try to set
	// the same connection (e.g., MainWindow wiring + child widgets wiring).
	if (m_shiftScaleFilter) {
		vtkAlgorithmOutput* cur = m_shiftScaleFilter->GetInputConnection(0, 0);
		// already connected to this exact port -> no-op
		if (cur == port) {
			// legacy: nothing to do
			if (!newImg)
				return;
		}
		else
		{
			m_shiftScaleFilter->SetInputConnection(port);
		}

		if (newImg) {
			m_shiftScaleFilter->UpdateInformation();
			m_shiftScaleFilter->Update();
		}
	}

	// Cache upstream producer (if any) and refresh m_imageData to reflect upstream.
	if (port) {
		vtkAlgorithm* prod = port->GetProducer();
		m_upstreamProducer = prod;
	}
	else {
		m_upstreamProducer = nullptr;
	}

	m_imageInitialized = !newImg;

	refreshImageDataFromPipeline();

	// mark image as initialized and trigger data update/render
	//m_imageInitialized = (port != nullptr);
	updateData(); // derived classes override updateData() to pull whatever they need
}

void ImageFrameWidget::refreshImageDataFromPipeline()
{
	// Prefer the actual upstream producer's output (pre-shift/scale).
	vtkDataObject* inObj = nullptr;

	// 1) If we have an upstream producer cached, ask it for output (ensure produced).
	if (m_upstreamProducer) {
		// Ensure the producer has published its information (extent/spacing/scalar type).
		// UpdateInformation is cheaper and avoids producing full data; it is the correct call
		// when consumers only need metadata (e.g., extents) to configure mappers/cameras.
		m_upstreamProducer->UpdateInformation();

		// If necessary, callers may force a full Update() elsewhere when they require the actual data.
		inObj = m_upstreamProducer->GetOutputDataObject(0);
	}

	// 2) If there was no upstream producer or it produced no vtkDataObject,
	//    check whether m_shiftScaleFilter has raw input data (SetInputData path).
	if (!inObj && m_shiftScaleFilter) {
		// Ensure the shared shift/scale filter has published its information too.
		// This helps downstream mappers that read from the filter's output port.
		m_shiftScaleFilter->UpdateInformation();

		// Prefer the filter's output object if the filter has produced one, otherwise fall back to its input.
		inObj = m_shiftScaleFilter->GetOutputDataObject(0);
		if (!inObj) {
			inObj = m_shiftScaleFilter->GetInputDataObject(0, 0);
			// clear cached producer since this path means input was provided directly
			if (inObj) m_upstreamProducer = nullptr;
		}
	}

	// 3) Cast to vtkImageData if possible; m_imageData is used elsewhere expecting the upstream image.
	m_imageData = vtkImageData::SafeDownCast(inObj);
}

void ImageFrameWidget::ensureOrientationMarkerInitialized()
{
	// If already created, nothing to do.
	// Use m_orientationCubeActor as the sentinel (we build the cube actor here).
	if (m_orientationCubeActor) return;

	if (!m_renderWindow) return;
	auto* iren = m_renderWindow->GetInteractor();
	if (!iren) return; // interactor not ready yet; defer initialization

	// Build a small wireframe cube centered on origin and three half-axes (positive halves)
	auto cube = vtkSmartPointer<vtkCubeSource>::New();
	cube->SetXLength(1.0);
	cube->SetYLength(1.0);
	cube->SetZLength(1.0);
	cube->SetCenter(0.0, 0.0, 0.0);

	auto cubeMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
	cubeMapper->SetInputConnection(cube->GetOutputPort());

	m_orientationCubeActor = vtkSmartPointer<vtkActor>::New();
	m_orientationCubeActor->SetMapper(cubeMapper);
	auto prop = m_orientationCubeActor->GetProperty();
	prop->SetRepresentationToWireframe();
	prop->SetColor(1.0, 1.0, 1.0);
	prop->SetLineWidth(1.0);
	prop->SetLighting(false);
	prop->SetSpecular(0.0);
	prop->SetDiffuse(0.0);
	prop->SetAmbient(1.0);
	m_orientationCubeActor->PickableOff();

	// scale down so marker doesn't dominate the viewport
	m_orientationCubeActor->SetScale(0.5, 0.5, 0.5);

	// Create three line actors for positive X (red), Y (green), Z (blue).
	const double axisLen = 0.6; // visible positive half-length
	struct AxisSpec { double dx, dy, dz; double r, g, b; };
	AxisSpec axes[3] = {
		{ axisLen, 0.0, 0.0, 1.0, 0.0, 0.0 }, // +X red
		{ 0.0, axisLen, 0.0, 0.0, 1.0, 0.0 }, // +Y green
		{ 0.0, 0.0, axisLen, 0.0, 0.0, 1.0 }  // +Z blue
	};

	// We keep an assembly for compatibility but the overlay renderer will host actors directly.
	auto assembly = vtkSmartPointer<vtkPropAssembly>::New();
	assembly->AddPart(m_orientationCubeActor);

	for (int i = 0; i < 3; ++i) {
		auto line = vtkSmartPointer<vtkLineSource>::New();
		line->SetPoint1(0.0, 0.0, 0.0);
		line->SetPoint2(axes[i].dx, axes[i].dy, axes[i].dz);

		auto lm = vtkSmartPointer<vtkPolyDataMapper>::New();
		lm->SetInputConnection(line->GetOutputPort());

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(lm);
		auto prop = actor->GetProperty();
		prop->SetColor(axes[i].r, axes[i].g, axes[i].b);
		prop->SetLineWidth(2.0);
		prop->SetLighting(false);
		prop->SetSpecular(0.0);
		prop->SetDiffuse(0.0);
		prop->SetAmbient(1.0);
		actor->PickableOff();

		assembly->AddPart(actor);
	}

	// Store assembly and create orientation widget
	m_orientationAssembly = assembly;

	// Add the cube + axis actors to the overlay renderer (layer 1).
	// We iterate the assembly parts and add them individually so we can control the overlay renderer's camera.
	for (int i = 0; i < m_orientationAssembly->GetParts()->GetNumberOfItems(); ++i) {
		vtkProp* part = vtkProp::SafeDownCast(m_orientationAssembly->GetParts()->GetItemAsObject(i));
		if (!part) continue;
		// If part is an actor, add to overlay renderer
		if (auto* a = vtkActor::SafeDownCast(part)) {
			m_orientationRenderer->AddActor(a);
		}
	}

	// Place overlay viewport in bottom-right corner by default (small), keep it hidden if flag off.
	if (m_orientationMarkerVisible) {
		m_orientationRenderer->SetViewport(0.78, 0.02, 0.98, 0.22);
	}
	else {
		m_orientationRenderer->SetViewport(0.0, 0.0, 0.0, 0.0);
	}

	// Configure a simple camera for the marker that looks at origin. We'll sync orientation during render().
	if (m_orientationRenderer) {
		auto* markerCam = m_orientationRenderer->GetActiveCamera();
		if (markerCam) {
			markerCam->ParallelProjectionOn();
			markerCam->SetFocalPoint(0.0, 0.0, 0.0);
			markerCam->SetPosition(0.0, 0.0, 3.0); // distance framing the unit cube
			markerCam->SetViewUp(0.0, 1.0, 0.0);
			markerCam->OrthogonalizeViewUp();
			m_orientationRenderer->ResetCamera();
		}
	}
}

void ImageFrameWidget::initializeRendererDefaults()
{
	m_renderer->GradientBackgroundOn();
	double color[3] = { 0., 0., 0. };
	m_renderer->SetBackground(color);  // black (lower part of gradient)
	color[2] = 1.;
	m_renderer->SetBackground2(color);  // blue (upper part of gradient)
}

void ImageFrameWidget::resetCamera()
{
	m_renderer->ResetCamera();
}

void ImageFrameWidget::render()
{
	// Ensure orientation marker exists and is attached to the interactor before render.
	ensureOrientationMarkerInitialized();

	if (auto* rw = getRenderWindow()) {
		if (auto* grw = vtkGenericOpenGLRenderWindow::SafeDownCast(rw)) {
			if (!grw->GetReadyForRendering()) {
				return; // avoid rendering before a current context exists
			}
		}

		// Sync orientation marker camera to main camera rotation so the marker reflects the same rotation
		// while keeping the marker camera focused on origin at a fixed distance. This isolates the marker
		// camera from the main camera's position and avoids interfering with main-camera resets.
		if (m_orientationRenderer && m_renderer) {
			auto* mainCam = m_renderer->GetActiveCamera();
			auto* markerCam = m_orientationRenderer->GetActiveCamera();
			if (mainCam && markerCam) {
				double dop[3]; mainCam->GetDirectionOfProjection(dop);
				double up[3]; mainCam->GetViewUp(up);

				// Adopt main camera's "up" to keep visual consistency
				markerCam->SetViewUp(up);
				// Keep focal point fixed at origin for the small marker
				markerCam->SetFocalPoint(0.0, 0.0, 0.0);
				// Position marker camera along the opposite direction-of-projection at a fixed distance
				const double dist = 3.0;
				markerCam->SetPosition(-dop[0] * dist, -dop[1] * dist, -dop[2] * dist);
				markerCam->OrthogonalizeViewUp();
				m_orientationRenderer->ResetCameraClippingRange();
			}
		}
		rw->Render();
	}
}

void ImageFrameWidget::setOrientationMarkerVisible(bool visible)
{
	m_orientationMarkerVisible = visible;
	if (m_orientationRenderer) {
		// Toggle viewport to show/hide the overlay renderer (simple approach).
		if (visible) m_orientationRenderer->SetViewport(0.78, 0.02, 0.98, 0.22);
		else         m_orientationRenderer->SetViewport(0.0, 0.0, 0.0, 0.0);
	}
}

void ImageFrameWidget::setViewOrientation(ViewOrientation orient)
{
	if (m_viewOrientation == orient) return;
	m_viewOrientation = orient;
	emit viewOrientationChanged(m_viewOrientation);

	// Notify derived classes / other users
	notifyViewOrientationChanged();
}

void ImageFrameWidget::notifyViewOrientationChanged()
{
	emit viewOrientationChanged(m_viewOrientation);
}

vtkRenderWindow* ImageFrameWidget::getRenderWindow() const
{
	return this->m_renderWindow;
}

QString ImageFrameWidget::orientationLabel(ViewOrientation orient) const
{
	switch (orient) {
		case VIEW_ORIENTATION_XY: return QStringLiteral("XY");
		case VIEW_ORIENTATION_YZ: return QStringLiteral("YZ");
		case VIEW_ORIENTATION_XZ: return QStringLiteral("XZ");
		default: return QString();
	}
}

ImageFrameWidget::ViewOrientation ImageFrameWidget::labelToOrientation(const QString& label) const
{
	if (label == QLatin1String("XY")) return VIEW_ORIENTATION_XY;
	if (label == QLatin1String("YZ")) return VIEW_ORIENTATION_YZ;
	if (label == QLatin1String("XZ")) return VIEW_ORIENTATION_XZ;
	return m_viewOrientation; // no change on unknown label
}

// Helper: obtain the vtkImageData produced by whatever is connected to our shift/scale input.
// Returns nullptr if there is no upstream connection or the produced data is not vtkImageData.
vtkImageData* ImageFrameWidget::upstreamInputImage() const
{
	if (!m_shiftScaleFilter) return nullptr;

	// Get the first input connection on port 0 (common case)
	vtkAlgorithmOutput* inPort = m_shiftScaleFilter->GetInputConnection(0, 0);
	if (!inPort) return nullptr;

	vtkAlgorithm* producer = inPort->GetProducer();
	if (!producer) return nullptr;

	// Ensure producer has produced its output (may be a source or pipeline element)
	producer->Update();

	// Get the actual output data object and cast to vtkImageData
	vtkDataObject* outObj = producer->GetOutputDataObject(0);
	return vtkImageData::SafeDownCast(outObj);
}

void ImageFrameWidget::computeShiftScaleFromInput()
{
	if (!m_imageData)
		return;

	m_nativeScalarType = m_imageData->GetScalarType();

	double scalarRange[2] = { 0.0, 1.0 };
	m_imageData->GetScalarRange(scalarRange);

	// Guard against NaN/Inf and inverted ranges
	const double r0 = std::isfinite(scalarRange[0]) ? scalarRange[0] : 0.0;
	const double r1 = std::isfinite(scalarRange[1]) ? scalarRange[1] : 1.0;
	m_scalarRangeMin = std::min(r0, r1);
	m_scalarRangeMax = std::max(r0, r1);

	// Default: no shift, no scale (preserve original numeric distribution)
	m_scalarShift = 0.0;
	m_scalarScale = 1.0;

	// Destination numeric limits (unsigned short)
	const double USHORT_MAX_D = static_cast<double>(std::numeric_limits<unsigned short>::max());

	// If the incoming range is degenerate, keep identity mapping
	const double diff = m_scalarRangeMax - m_scalarRangeMin;
	if (!(std::isfinite(diff)) || diff <= 0.0) {
		m_scalarShift = (m_scalarRangeMin < 0.0) ? -m_scalarRangeMin : 0.0;
		m_scalarScale = 1.0;
		m_shiftScaleFilter->SetShift(m_scalarShift);
		m_shiftScaleFilter->SetScale(m_scalarScale);
		m_shiftScaleFilter->Update();
		return;
	}

	// Strategy:
	// - Only apply a shift if the data contains negative values (so the minimum becomes 0).
	// - Only apply scaling if, after shifting, values exceed the unsigned-short max.
	//   Scaling compresses the numeric range to fit; it will never stretch (scale > 1).
	double shift = 0.0;
	if (m_scalarRangeMin < 0.0) {
		// move minimum to zero (preserve relative positions)
		shift = -m_scalarRangeMin;
	}

	// After shift, the maximum value to consider is:
	// - if shift was applied: max' = max + shift  (which equals range)
	// - if no shift: max' = max
	double maxAfterShift = m_scalarRangeMax + shift;

	// If data fits within [0, USHORT_MAX] after shifting, keep scale = 1
	if (maxAfterShift <= USHORT_MAX_D && maxAfterShift >= 0.0) {
		m_scalarShift = shift;
		m_scalarScale = 1.0;
	}
	else {
		// Need to compress into [0, USHORT_MAX].
		// Use denominator == maxAfterShift (for shift==0 this is original max;
		// for shift>0 this is the data span after shift).
		double denom = maxAfterShift;
		if (denom <= 0.0 || !std::isfinite(denom)) {
			// defensive fallback: no scaling, just shift if needed
			m_scalarShift = shift;
			m_scalarScale = 1.0;
		}
		else {
			double scale = USHORT_MAX_D / denom;
			// never stretch: clamp to <= 1.0
			if (scale > 1.0) {
				scale = 1.0;
			}
			m_scalarShift = shift;
			m_scalarScale = scale;
		}
	}

	// Program the shared filter (it already outputs unsigned short)
	m_shiftScaleFilter->SetShift(m_scalarShift);
	m_shiftScaleFilter->SetScale(m_scalarScale);
	m_shiftScaleFilter->Update();
}

void ImageFrameWidget::onSelectionChanged(bool selected)
{
	// Optional feature: when enabled (default), only the selected frame is interactive
	if (!m_renderWindow) return;

	if (auto* iren = m_renderWindow->GetInteractor()) {
		if (restrictInteractionToSelection()) {
			if (selected) iren->Enable();
			else iren->Disable();
		}
		else {
			// Free mode: all frames interactive
			iren->Enable();
		}
	}
}

void ImageFrameWidget::resetWindowLevel()
{
	// Default implementation: delegate to derived setColorWindowLevel
	// using the retained baseline in native domain (VolumeView overrides setColorWindowLevel).
	if (!m_imageData) return;
	if (!std::isfinite(m_baselineWindowNative) || !std::isfinite(m_baselineLevelNative)) return;
	// If derived did not override setColorWindowLevel, this may be a no-op (SliceView overrides reset itself).
	setColorWindowLevel(m_baselineWindowNative, m_baselineLevelNative);
}

void ImageFrameWidget::setBaselineWindowLevel(double windowNative, double levelNative)
{
	m_baselineWindowNative = windowNative;
	m_baselineLevelNative = levelNative;
}

std::pair<double, double> ImageFrameWidget::mapWindowLevelToMapped(double windowNative, double levelNative) const
{
	// x_mapped = (x_native + shift) * scale
	const double levelMapped = (levelNative + m_scalarShift) * m_scalarScale;
	const double windowMapped = std::fabs(windowNative) * m_scalarScale;
	return { windowMapped, levelMapped };
}

std::pair<double, double> ImageFrameWidget::baselineMapped() const
{
	return mapWindowLevelToMapped(m_baselineWindowNative, m_baselineLevelNative);
}

// Return the canonical orientation if the main camera's view-normal is aligned with
// one of the principal axes within `maxAngleDeg` degrees; otherwise return -1.
int ImageFrameWidget::cameraAlignedOrientation(double maxAngleDeg) const
{
	if (!m_renderer) return -1;
	vtkCamera* cam = m_renderer->GetActiveCamera();
	if (!cam) return -1;

	double vpn[3];
	cam->GetViewPlaneNormal(vpn);

	// normalize (just in case)
	const double mag = vtkMath::Norm(vpn);
	if (mag <= 0.0) return -1;
	vpn[0] /= mag; vpn[1] /= mag; vpn[2] /= mag;

	// canonical axes: X, Y, Z -> map to ViewOrientation (look along X -> YZ, Y -> XZ, Z -> XY)
	const double axes[3][3] = { {1.0,0.0,0.0}, {0.0,1.0,0.0}, {0.0,0.0,1.0} };

	double bestAngle = 180.0;
	int bestAxis = -1;
	for (int i = 0; i < 3; ++i) {
		double dot = std::fabs(vpn[0] * axes[i][0] + vpn[1] * axes[i][1] + vpn[2] * axes[i][2]);
		if (dot > 1.0) dot = 1.0;
		if (dot < -1.0) dot = -1.0;
		const double angleDeg = std::acos(dot) * (180.0 / vtkMath::Pi());
		if (angleDeg < bestAngle) {
			bestAngle = angleDeg;
			bestAxis = i;
		}
	}

	if (bestAxis < 0) return -1;
	if (bestAngle > maxAngleDeg) return -1;

	// Map axis -> ViewOrientation (look along axis -> the corresponding slice plane)
	switch (bestAxis) {
		case 0: return VIEW_ORIENTATION_YZ; // looking along +X => YZ
		case 1: return VIEW_ORIENTATION_XZ; // looking along +Y => XZ
		case 2: return VIEW_ORIENTATION_XY; // looking along +Z => XY
		default: return -1;
	}
}

void ImageFrameWidget::cacheImageGeometry()
{
	if (!m_imageData) return;
	m_imageData->GetExtent(m_extent);
	m_imageData->GetSpacing(m_spacing);
	m_imageData->GetOrigin(m_origin);
}

void ImageFrameWidget::refreshEndpointFromUpstream()
{
	// Capture main camera state (deep copy) so we can restore it later.
	vtkSmartPointer<vtkCamera> savedCam = nullptr;
	if (m_renderer) {
		if (auto* cam = m_renderer->GetActiveCamera()) {
			savedCam = vtkSmartPointer<vtkCamera>::New();
			savedCam->DeepCopy(cam);
		}
	}

	// Let derived classes capture view-specific state (slice index, WL, etc.)
	captureDerivedViewState();

	// Refresh pipeline metadata & internal image pointer
	refreshImageDataFromPipeline();
	if (m_imageData) {
		// Recompute shift/scale mapping and update shared filter
		computeShiftScaleFromInput();
		if (m_shiftScaleFilter) m_shiftScaleFilter->Update();
		// Cache extents/spacing/origin for derived views
		cacheImageGeometry();
	}

	// Restore camera state if we captured it
	if (savedCam && m_renderer) {
		if (auto* cam = m_renderer->GetActiveCamera()) {
			cam->DeepCopy(savedCam);
			m_renderer->ResetCameraClippingRange();
		}
	}

	// Let derived classes restore their saved state (slice, WL, etc.)
	restoreDerivedViewState();

	// Final render
	render();
}
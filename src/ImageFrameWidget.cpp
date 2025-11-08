#include "ImageFrameWidget.h"

#include <algorithm>
#include <cmath>

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

void ImageFrameWidget::computeShiftScaleFromInput(vtkImageData* image)
{
	m_nativeScalarType = image->GetScalarType();
	double scalarRange[2] = { 0, 1 };
	image->GetScalarRange(scalarRange);
	// Guard against NaN/Inf and inverted ranges
	const double r0 = std::isfinite(scalarRange[0]) ? scalarRange[0] : 0.0;
	const double r1 = std::isfinite(scalarRange[1]) ? scalarRange[1] : 1.0;
	m_scalarRangeMin = std::min(r0, r1);
	m_scalarRangeMax = std::max(r0, r1);
	const double diff = m_scalarRangeMax - m_scalarRangeMin;

	// Default: keep as unsigned short
	m_scalarShift = 0.0;
	m_scalarScale = 1.0;

	switch (m_nativeScalarType) {
		case VTK_UNSIGNED_CHAR:
		case VTK_UNSIGNED_SHORT:
		m_scalarShift = 0.0;
		m_scalarScale = 1.0;
		break;
		case VTK_CHAR:
		case VTK_SIGNED_CHAR:
		m_scalarShift = 128.0; // map [-128,127] -> [0,255]
		m_scalarScale = 1.0;
		break;
		case VTK_SHORT:
		m_scalarShift = 32768.0; // map [-32768,32767] -> [0,65535]
		m_scalarScale = 1.0;
		break;
		default: {
			// For larger ranges or floating point: shift negatives, scale up to at most 16-bit range
			m_scalarShift = (m_scalarRangeMin < 0.0) ? -m_scalarRangeMin : 0.0;
			if (diff > 0.0) {
				// Preserve existing behavior: do not amplify if the range is already within 16-bit
				m_scalarScale = std::min(65535.0 / diff, 1.0);
			}
			else {
				m_scalarScale = 1.0;
			}
			break;
		}
	}

	// Program the shared filter
	m_shiftScaleFilter->SetOutputScalarTypeToUnsignedShort();
	m_shiftScaleFilter->SetShift(m_scalarShift);
	m_shiftScaleFilter->SetScale(m_scalarScale);

	m_shiftScaleFilter->SetInputData(m_imageData);
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

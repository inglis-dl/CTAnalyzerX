#include "VolumeView.h"
#include "ui_VolumeView.h"
#include "MenuButton.h"
#include "vtkImageOrthoPlanes.h"

#include <QAction>
#include <QMenu>
#include <QFrame>
#include <QTimer>
#include <QLayout>
#include <QSizePolicy>
#include <QDebug>

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkImageData.h>
#include <vtkImageSlice.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCamera.h>
#include <vtkMath.h>
#include <vtkProperty.h>
#include <vtkCommand.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkImageShiftScale.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageProperty.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>

#include <cmath> // added for std::lround


VolumeView::VolumeView(QWidget* parent)
	: ImageFrameWidget(parent)
	, ui(new Ui::VolumeView)
{
	// Install Designer UI into frame body
	auto* content = new QFrame(this);
	ui->setupUi(content);

	// Remove all margins/spacings so the render widget fills the area
	content->setContentsMargins(0, 0, 0, 0);
	if (auto* lay = content->layout()) {
		lay->setContentsMargins(0, 0, 0, 0);
		lay->setSpacing(0);
	}
	for (QLayout* l : content->findChildren<QLayout*>()) {
		l->setContentsMargins(0, 0, 0, 0);
		l->setSpacing(0);
	}

	ui->renderArea->setContentsMargins(0, 0, 0, 0);
	ui->renderArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setSceneContent(content);

	createMenuAndActions();

	ui->renderArea->setRenderWindow(m_renderWindow);
	setFocusProxy(ui->renderArea);
	ui->renderArea->setFocusPolicy(Qt::StrongFocus);
	ui->renderArea->installEventFilter(this);

	// Volume pipeline core
	m_mapper = vtkSmartPointer<vtkGPUVolumeRayCastMapper>::New();
	m_mapper->SetBlendModeToComposite();
	m_mapper->SetAutoAdjustSampleDistances(1);
	m_mapper->SetInputConnection(m_shiftScaleFilter->GetOutputPort());

	m_volumeProperty = vtkSmartPointer<vtkVolumeProperty>::New();
	m_volumeProperty->ShadeOff();
	m_volumeProperty->SetInterpolationTypeToLinear();

	m_volume = vtkSmartPointer<vtkVolume>::New();
	m_volume->SetMapper(m_mapper);
	m_volume->SetProperty(m_volumeProperty);

	// Transfer functions
	m_actualColorTF = vtkSmartPointer<vtkColorTransferFunction>::New();
	m_colorTF = vtkSmartPointer<vtkColorTransferFunction>::New();
	m_actualScalarOpacity = vtkSmartPointer<vtkPiecewiseFunction>::New();
	m_scalarOpacity = vtkSmartPointer<vtkPiecewiseFunction>::New();

	initializeDefaultTransferFunctions();

	auto interactor = m_renderWindow->GetInteractor();

	m_qvtk = vtkSmartPointer<vtkEventQtSlotConnect>::New();
	if (interactor) {
		// Use full-signature slot so we can abort plain 'r'/'R'
		m_qvtk->Connect(interactor, vtkCommand::CharEvent,
			this, SLOT(onInteractorChar(vtkObject*, unsigned long, void*, void*, vtkCommand*)),
			nullptr, 1.0f);

		m_qvtk->Connect(m_renderer->GetActiveCamera(), vtkCommand::ModifiedEvent,
			this, SLOT(onCameraModified(vtkObject*)), nullptr);
	}

	// inside VolumeView::VolumeView after creating m_imageSliceXY / m_sliceMappers
	m_orthoPlanes = vtkSmartPointer<vtkImageOrthoPlanes>::New();
	if (m_shiftScaleFilter) {
		m_orthoPlanes->SetInputConnection(m_shiftScaleFilter->GetOutputPort());
	}
	// keep ortho planes hidden initially (will be toggled via setOrthoPlanesVisible)
	m_orthoPlanes->SetPlaneVisibility(false, false, false);

	// ensure it's present in renderer so it participates in bounds/rendering
	if (m_renderer && !m_renderer->HasViewProp(m_orthoPlanes)) {
		m_renderer->AddViewProp(m_orthoPlanes);
	}

	// at end of createSliceOutlineActors(), after XY/XZ/YZ props are prepared
	if (m_orthoPlanes) {
		vtkActor* ax = m_orthoPlanes->GetOutlineActorX(); // X: YZ plane
		vtkActor* ay = m_orthoPlanes->GetOutlineActorY(); // Y: XZ plane
		vtkActor* az = m_orthoPlanes->GetOutlineActorZ(); // Z: XY plane
		if (ax) {
			ax->GetProperty()->SetColor(1.0, 0.0, 0.0);
		}
		if (ay) {
			ay->GetProperty()->SetColor(0.0, 1.0, 0.0);
		}
		if (az) {
			az->GetProperty()->SetColor(0.0, 0.0, 1.0);
		}
	}
}

VolumeView::~VolumeView()
{
	delete ui;
}

void VolumeView::createMenuAndActions()
{
	// Populate the menu with orientations + command items handled via MenuButton::itemSelected
	setSelectionList({
		QStringLiteral("Volume"),
		QStringLiteral("Slice Planes"),
		QStringLiteral("--"),
		QStringLiteral("XY"),
		QStringLiteral("YZ"),
		QStringLiteral("XZ"),
		QStringLiteral("Reset Camera")
	});

	// Drive behavior entirely from MenuButton::itemSelected
	if (auto* mb = menuButton()) {
		connect(mb, &MenuButton::itemSelected, this, [this](const QString& item) {
			// Orientation selections
			if (item == QLatin1String("XY") || item == QLatin1String("YZ") || item == QLatin1String("XZ")) {
				const ViewOrientation orient = labelToOrientation(item);
				setViewOrientation(orient);
				setTitle(m_orthoPlanesVisible ? QStringLiteral("Slice Planes") : QStringLiteral("Volume"));
				return;
			}

			if (item == QLatin1String("Volume")) {
				// Toggle to 3D volume rendering
				setOrthoPlanesVisible(false);
			}
			else if (item == QLatin1String("Slice Planes")) {
				// Toggle to slice planes view
				setOrthoPlanesVisible(true);
			}
			else if (item == QLatin1String("Reset Camera")) {
				resetCamera();
			}
			// Always restore the title to the current mode after any command
			setTitle(m_orthoPlanesVisible ? QStringLiteral("Slice Planes") : QStringLiteral("Volume"));
		});
	}
}

void VolumeView::initializeDefaultTransferFunctions()
{
	// Default grayscale in "actual" domain; mapped TFs are derived later
	m_actualColorTF->RemoveAllPoints();
	m_actualColorTF->AddRGBPoint(0.0, 0.0, 0.0, 0.0);
	m_actualColorTF->AddRGBPoint(65535.0, 1.0, 1.0, 1.0);
	m_actualColorTF->Build();

	// Scalar opacity default to simple 0..1 ramp
	m_actualScalarOpacity->RemoveAllPoints();
	m_actualScalarOpacity->AddPoint(0.0, 0.0);
	m_actualScalarOpacity->AddPoint(65535.0, 1.0);
	m_actualScalarOpacity->Modified();

	// Initialize mapped TFs to match actual until we know shift/scale
	m_colorTF->DeepCopy(m_actualColorTF);
	m_scalarOpacity->DeepCopy(m_actualScalarOpacity);

	m_volumeProperty->SetColor(m_colorTF);
	m_volumeProperty->SetScalarOpacity(m_scalarOpacity);
}

void VolumeView::updateData()
{
	if (!m_imageData) return;

	m_shiftScaleFilter->Update();

	// Compute mapping and connect the shared filter
	computeShiftScaleFromInput();
	cacheImageGeometry();

	//
	// Place slice actors / mappers to data bounds so rendering is robust.
	double b[6] = { 0,0,0,0,0,0 };
	m_shiftScaleFilter->GetOutput()->GetBounds(b);

	const int cx = (m_extent[0] + m_extent[1]) / 2;
	const int cy = (m_extent[2] + m_extent[3]) / 2;
	const int cz = (m_extent[4] + m_extent[5]) / 2;

	if (!m_imageInitialized) {
		if (m_orthoPlanes) {
			// Ensure the ortho-planes source is wired to the shared shifted/scaled image
			m_orthoPlanes->SetInputConnection(m_shiftScaleFilter->GetOutputPort());
			// Initialize to center slices
			m_orthoPlanes->SetSliceNumbers(cx, cy, cz);
			// match interpolation setting
			switch (m_interpolation) {
				case Nearest: m_orthoPlanes->SetInterpolationToNearest(); break;
				default:      m_orthoPlanes->SetInterpolationToLinear();  break;
			}
			m_orthoPlanes->Update();
			// ensure initial visibility matches current mode
			m_orthoPlanes->SetPlaneVisibility(m_orthoPlanesVisible, m_orthoPlanesVisible, m_orthoPlanesVisible);
		}

		m_imageInitialized = true;
	}

	// Rebuild the ACTUAL color TF to span the native image range (not 0..65535)
	const double diff = m_scalarRangeMax - m_scalarRangeMin;
	const double lb = diff > 0.0 ? (m_scalarRangeMin + 0.01 * diff) : m_scalarRangeMin;
	const double ub = diff > 0.0 ? (m_scalarRangeMax - 0.01 * diff) : m_scalarRangeMax;

	m_actualColorTF->RemoveAllPoints();
	m_actualColorTF->AddRGBPoint(lb, 0.0, 0.0, 0.0);
	m_actualColorTF->AddRGBPoint(ub, 1.0, 1.0, 1.0);
	m_actualColorTF->Build();

	// Remap ACTUAL -> MAPPED using current shift/scale, then attach to property
	updateMappedColorsFromActual();
	updateMappedOpacityFromActual();

	m_volumeProperty->SetColor(m_colorTF);
	m_volumeProperty->SetScalarOpacity(m_scalarOpacity);

	// Initialize WL in native domain and retain as baseline
	const double baseWindow = std::max(ub - lb, 1.0);
	const double baseLevel = 0.5 * (ub + lb);
	setBaselineWindowLevel(baseWindow, baseLevel);
	setColorWindowLevel(baseWindow, baseLevel); // updates opacity+color and renders
	setSliceWindowLevelNative(baseWindow, baseLevel);

	// set scalar opacity unit distance to match data spacing
	const double unit = (m_spacing[0] + m_spacing[1] + m_spacing[2]) / 3.0;
	m_volumeProperty->SetScalarOpacityUnitDistance(unit);

	resetCamera();

	// Use vtkImageOrthoPlanes (preferred) to set the center slices; legacy per-mapper code removed.
	if (m_orthoPlanes) {
		m_orthoPlanes->SetSliceNumbers(cx, cy, cz);
		m_orthoPlanes->Update();
	}

	// Reset cropping to full image extent to avoid applying stale/invalid crop planes
	if (m_mapper) {
		m_mapper->SetCroppingRegionPlanes(m_extent[0], m_extent[1],
										  m_extent[2], m_extent[3],
										  m_extent[4], m_extent[5]);
		m_mapper->SetCropping(false); // start with cropping off; UI can enable it
		// Notify UI that cropping has been disabled/reset for the new image
		emit croppingEnabledChanged(false);
	}

	emit imageExtentsChanged(m_extent[0], m_extent[1], m_extent[2], m_extent[3], m_extent[4], m_extent[5]);

	setOrthoPlanesVisible(m_orthoPlanesVisible);
	render();
}

void VolumeView::setColorWindowLevel(double window, double level)
{
	// Implement like vtkVolumeScene::DoWindowLevel, but in the native scalar domain.
	// Map to four points across [lower, upper] with outer plateaus.
	const double lower = level - 0.5 * std::fabs(window);
	const double upper = level + 0.5 * std::fabs(window);

	const double lb = std::max(lower, m_scalarRangeMin);
	const double ub = std::min(upper, m_scalarRangeMax);

	// Construct the "actual" scalar opacity 4-point function
	m_actualScalarOpacity->RemoveAllPoints();
	// edge order: min -> lb -> ub -> max
	const double lowVal = (window < 0.0) ? 1.0 : 0.0;
	const double highVal = (window < 0.0) ? 0.0 : 1.0;

	m_actualScalarOpacity->AddPoint(m_scalarRangeMin, lowVal);
	m_actualScalarOpacity->AddPoint(lb, lowVal);
	m_actualScalarOpacity->AddPoint(ub, highVal);
	m_actualScalarOpacity->AddPoint(m_scalarRangeMax, highVal);
	m_actualScalarOpacity->Modified();

	// Recompute mapped TFs using current shift/scale
	updateMappedOpacityFromActual();

	m_volumeProperty->SetScalarOpacity(m_scalarOpacity);

	// For color TF, keep grayscale but ensure mapping is current
	updateMappedColorsFromActual();
	m_volumeProperty->SetColor(m_colorTF);

	setSliceWindowLevelNative(window, level);

	render();

	emit windowLevelChanged(window, level);
}

void VolumeView::setInterpolation(Interpolation newInterpolation)
{
	if (newInterpolation == interpolation())
		return;

	m_interpolation = newInterpolation;
	switch (m_interpolation) {
		case Nearest:
		m_volumeProperty->SetInterpolationTypeToNearest();
		m_orthoPlanes->SetInterpolationToNearest(); // match volume property
		break;
		case Linear:
		case Cubic: // VTK volume property doesn't provide cubic; fall back to linear
		m_volumeProperty->SetInterpolationTypeToLinear();
		m_orthoPlanes->SetInterpolationToLinear(); // match volume property
		break;
	}
	render();
	emit interpolationChanged(m_interpolation);
}

void VolumeView::updateSlicePlanes(int x, int y, int z)
{
	if (!m_imageInitialized || !m_imageData || !m_orthoPlanes) return;

	const int cx = std::clamp(x, m_extent[0], m_extent[1]);
	const int cy = std::clamp(y, m_extent[2], m_extent[3]);
	const int cz = std::clamp(z, m_extent[4], m_extent[5]);

	// If using vtkImageOrthoPlanes prefer it
	m_orthoPlanes->SetSliceNumbers(cx, cy, cz);
	m_orthoPlanes->Update(); // recompute outlines & bounds

	if (m_orthoPlanesVisible)
		render();
}

void VolumeView::setOrthoPlanesVisible(bool visible)
{
	const bool modified = (m_orthoPlanesVisible != visible);
	m_orthoPlanesVisible = visible;

	// Mutually exclusive: show either volume or planes, not both
	if (visible) {
		if (m_volume && m_renderer->HasViewProp(m_volume))
			m_renderer->RemoveVolume(m_volume);
	}
	else {
		if (m_volume && !m_renderer->HasViewProp(m_volume))
			m_renderer->AddVolume(m_volume);
	}

	if (m_orthoPlanes) {
		m_orthoPlanes->SetPlaneVisibility(visible);
	}

	// Ensure the title and menu reflect the current mode so external UI (checkbox/menu) stays in sync.
	setTitle(m_orthoPlanesVisible ? QStringLiteral("Slice Planes") : QStringLiteral("Volume"));

	if (modified)
		emit orthoPlanesVisibleChanged(m_orthoPlanesVisible);

	render();
}

void VolumeView::setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax)
{
	if (!m_mapper || !m_imageData) return;

	auto clampNormalize = [](int& lo, int& hi, int loB, int hiB) {
		lo = std::clamp(lo, loB, hiB);
		hi = std::clamp(hi, loB, hiB);
		if (lo > hi) std::swap(lo, hi);
		// avoid a degenerate single-voxel region if possible
		if (lo == hi) {
			if (hi < hiB) ++hi;
			else if (lo > loB) --lo;
		}
		};

	clampNormalize(xMin, xMax, m_extent[0], m_extent[1]);
	clampNormalize(yMin, yMax, m_extent[2], m_extent[3]);
	clampNormalize(zMin, zMax, m_extent[4], m_extent[5]);

	// Convert voxel (continuous index) -> physical/world coordinates
	double idx[3], physMin[3], physMax[3], physPt[3];

	// X min/max
	idx[0] = static_cast<double>(xMin); idx[1] = 0.0; idx[2] = 0.0;
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		// fallback to origin + spacing calculation if TransformContinuousIndexToPhysicalPoint is not appropriate
		physMin[0] = m_origin[0] + idx[0] * m_spacing[0];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMin[0] = physPt[0];
	}
	idx[0] = static_cast<double>(xMax);
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		physMax[0] = m_origin[0] + idx[0] * m_spacing[0];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMax[0] = physPt[0];
	}

	// Y min/max
	idx[0] = 0.0; idx[1] = static_cast<double>(yMin); idx[2] = 0.0;
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		physMin[1] = m_origin[1] + idx[1] * m_spacing[1];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMin[1] = physPt[1];
	}
	idx[1] = static_cast<double>(yMax);
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		physMax[1] = m_origin[1] + idx[1] * m_spacing[1];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMax[1] = physPt[1];
	}

	// Z min/max
	idx[0] = 0.0; idx[1] = 0.0; idx[2] = static_cast<double>(zMin);
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		physMin[2] = m_origin[2] + idx[2] * m_spacing[2];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMin[2] = physPt[2];
	}
	idx[2] = static_cast<double>(zMax);
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		physMax[2] = m_origin[2] + idx[2] * m_spacing[2];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMax[2] = physPt[2];
	}

	// Apply cropping in physical coordinates (what vtkVolumeMapper expects)
	m_mapper->SetCropping(true);
	m_mapper->SetCroppingRegionPlanes(
		physMin[0], physMax[0],
		physMin[1], physMax[1],
		physMin[2], physMax[2]
	);

	render();
}

void VolumeView::resetCamera()
{
	if (!m_renderer) return;

	vtkCamera* cam = m_renderer->GetActiveCamera();
	if (!cam || !m_imageData) {
		m_renderer->ResetCamera();
		render();
		return;
	}

	// Compute volume bounds and center
	double bounds[6];
	m_imageData->GetBounds(bounds);

	const double center[3] = {
		0.5 * (bounds[0] + bounds[1]),
		0.5 * (bounds[2] + bounds[3]),
		0.5 * (bounds[4] + bounds[5])
	};

	// Preserve current rotation: direction of projection and view-up
	double dop[3];
	cam->GetDirectionOfProjection(dop); // normalized camera->focal
	double up[3];
	cam->GetViewUp(up);

	// Build an orthonormal camera basis (r = right, u = true up, d = dop)
	double r[3];
	vtkMath::Cross(dop, up, r);
	if (vtkMath::Norm(r) < 1e-12) {
		// Degenerate up; pick a safe fallback and rebuild basis
		up[0] = 0.0; up[1] = 1.0; up[2] = 0.0;
		vtkMath::Cross(dop, up, r);
	}
	vtkMath::Normalize(r);
	vtkMath::Cross(r, dop, up);
	vtkMath::Normalize(up);

	// Project the 8 corners to get half-width/half-height in the camera plane
	const double xs[2] = { bounds[0], bounds[1] };
	const double ys[2] = { bounds[2], bounds[3] };
	const double zs[2] = { bounds[4], bounds[5] };

	double maxAbsU = 0.0;
	double maxAbsV = 0.0;

	for (int ix = 0; ix < 2; ++ix) {
		for (int iy = 0; iy < 2; ++iy) {
			for (int iz = 0; iz < 2; ++iz) {
				const double p[3] = { xs[ix], ys[iy], zs[iz] };
				double v[3] = { p[0] - center[0], p[1] - center[1], p[2] - center[2] };
				const double u = v[0] * r[0] + v[1] * r[1] + v[2] * r[2];
				const double vv = v[0] * up[0] + v[1] * up[1] + v[2] * up[2];
				maxAbsU = std::max(maxAbsU, std::abs(u));
				maxAbsV = std::max(maxAbsV, std::abs(vv));
			}
		}
	}

	// Viewport aspect ratio
	int vpW = 1, vpH = 1;
	if (m_renderWindow) {
		int* size = m_renderWindow->GetSize();
		vpW = size[0];
		vpH = size[1];
	}

	const double aspect = (vpW > 0 && vpH > 0) ? static_cast<double>(vpW) / static_cast<double>(vpH) : 1.0;

	const double margin = 1.05; // small padding

	if (cam->GetParallelProjection()) {
		// Parallel: scale is half of world height visible
		// Ensure width also fits given the aspect ratio
		const double scaleV = maxAbsV;
		const double scaleH = (aspect > 0.0) ? (maxAbsU / aspect) : maxAbsU;
		const double scale = std::max(scaleV, scaleH) * margin;

		// Keep distance but re-center on the volume
		const double distance = cam->GetDistance();

		double pos[3] = {
			center[0] - dop[0] * distance,
			center[1] - dop[1] * distance,
			center[2] - dop[2] * distance
		};

		cam->SetFocalPoint(center);
		cam->SetPosition(pos);
		cam->SetViewUp(up);
		cam->SetParallelScale(scale);
		cam->OrthogonalizeViewUp();

		m_renderer->ResetCameraClippingRange(bounds);
		render();
		return;
	}

	// Perspective: compute distance so that both width and height fit
	const double vFovDeg = cam->GetViewAngle();
	const double vFovRad = vFovDeg * (vtkMath::Pi() / 180.0);
	const double tanV = std::tan(0.5 * vFovRad);
	const double tanH = tanV * aspect;

	// Guard against degenerate FOV
	const double distV = (tanV > 1e-12) ? (maxAbsV / tanV) : cam->GetDistance();
	const double distH = (tanH > 1e-12) ? (maxAbsU / tanH) : cam->GetDistance();
	const double distance = std::max(distV, distH) * margin;

	double pos[3] = {
		center[0] - dop[0] * distance,
		center[1] - dop[1] * distance,
		center[2] - dop[2] * distance
	};

	cam->SetFocalPoint(center);
	cam->SetPosition(pos);
	cam->SetViewUp(up);
	cam->OrthogonalizeViewUp();

	m_renderer->ResetCameraClippingRange(bounds);
	render();
}

void VolumeView::setViewOrientation(ImageFrameWidget::ViewOrientation orientation)
{
	const int orientationNow = cameraAlignedOrientation(0.1);
	if (orientationNow == static_cast<int>(orientation)) {
		// Already aligned; no change
		return;
	}

	m_viewOrientation = orientation;

	vtkCamera* cam = m_renderer ? m_renderer->GetActiveCamera() : nullptr;

	if (!m_imageData || !m_renderer || !cam) {
		notifyViewOrientationChanged();
		return;
	}

	double bounds[6] = { 0,0,0,0,0,0 };
	m_imageData->GetBounds(bounds);
	const double center[3] = {
		0.5 * (bounds[0] + bounds[1]),
		0.5 * (bounds[2] + bounds[3]),
		0.5 * (bounds[4] + bounds[5])
	};

	int w = static_cast<int>(m_viewOrientation);
	int u = 0, v = 1; // defaults for XY
	switch (w) {
		case VIEW_ORIENTATION_YZ: u = 1; v = 2; break; // look along +X, up +Z
		case VIEW_ORIENTATION_XZ: u = 0; v = 2; break; // look along +Y, up +Z
		case VIEW_ORIENTATION_XY:
		default:                  u = 0; v = 1; break; // look along +Z, up +Y
	}

	double viewUp[3] = { 0.0, 0.0, 0.0 };
	double vpn[3] = { 0.0, 0.0, 0.0 };
	viewUp[v] = 1.0;
	vpn[w] = 1.0;

	const double defaultDistance = std::max(1.0, cam->GetDistance());
	double pos[3] = {
		center[0] - vpn[0] * defaultDistance,
		center[1] - vpn[1] * defaultDistance,
		center[2] - vpn[2] * defaultDistance
	};

	cam->ParallelProjectionOn();
	cam->SetFocalPoint(center);
	cam->SetPosition(pos);
	cam->SetViewUp(viewUp);
	cam->OrthogonalizeViewUp();

	m_renderer->ResetCamera(bounds);
	m_renderer->ResetCameraClippingRange(bounds);

	render();

	notifyViewOrientationChanged();
}

// Map actual -> mapped for opacity using current shift/scale (like vtkVolumeScene::UpdateOpacityFunction)
void VolumeView::updateMappedOpacityFromActual()
{
	// Rebuild target function to match the actual function's nodes
	m_scalarOpacity->RemoveAllPoints();

	const int n = m_actualScalarOpacity->GetSize();
	double node[4];
	for (int i = 0; i < n; ++i) {
		m_actualScalarOpacity->GetNodeValue(i, node); // node[0]=x, node[1]=y, node[2..3] tension/continuity
		// apply (x + shift) * scale to map into the filter's post-mapping domain
		const double xMapped = (node[0] + m_scalarShift) * m_scalarScale;
		m_scalarOpacity->AddPoint(xMapped, node[1], node[2], node[3]);
	}
	m_scalarOpacity->Modified();
}

// Map actual -> mapped for color using current shift/scale (like vtkVolumeScene::UpdateColorFunction)
void VolumeView::updateMappedColorsFromActual()
{
	// Keep color space consistent
	m_colorTF->RemoveAllPoints();
	m_colorTF->SetColorSpace(m_actualColorTF->GetColorSpace());

	const int n = m_actualColorTF->GetSize();
	double node[6];
	const bool useRGB = (m_actualColorTF->GetColorSpace() == 0);

	for (int i = 0; i < n; ++i) {
		m_actualColorTF->GetNodeValue(i, node); // node[0]=x, node[1..3]=rgb/hsv, node[4..5]=mid/sharp
		const double xMapped = (node[0] + m_scalarShift) * m_scalarScale;
		if (useRGB) {
			m_colorTF->AddRGBPoint(xMapped, node[1], node[2], node[3], node[4], node[5]);
		}
		else {
			m_colorTF->AddHSVPoint(xMapped, node[1], node[2], node[3], node[4], node[5]);
		}
	}
	m_colorTF->Build();
}

void VolumeView::setShadingEnabled(bool on)
{
	if (m_shadingEnabled == on) return;
	m_shadingEnabled = on;
	if (m_volumeProperty) {
		if (on) m_volumeProperty->ShadeOn();
		else    m_volumeProperty->ShadeOff();
	}
	render();
}

void VolumeView::resetWindowLevel()
{
	// Apply retained baseline in native domain for BOTH color TF and opacity TF
	if (!m_imageData) return;

	const double w = baselineWindowNative();
	const double l = baselineLevelNative();
	if (!std::isfinite(w) || !std::isfinite(l)) return;

	// Rebuild the actual color TF to span [lower, upper] in the native domain (same as initial)
	const double lower = l - 0.5 * std::fabs(w);
	const double upper = l + 0.5 * std::fabs(w);

	// Clamp to native scalar range for safety
	const double lb = std::max(lower, m_scalarRangeMin);
	const double ub = std::min(upper, m_scalarRangeMax);

	m_actualColorTF->RemoveAllPoints();
	m_actualColorTF->AddRGBPoint(lb, 0.0, 0.0, 0.0);
	m_actualColorTF->AddRGBPoint(ub, 1.0, 1.0, 1.0);
	m_actualColorTF->Build();

	// Map and apply the color TF
	updateMappedColorsFromActual();
	m_volumeProperty->SetColor(m_colorTF);

	// Rebuild opacity from the same baseline WL
	setColorWindowLevel(w, l); // updates opacity TF (and re-maps colors for consistency)

	emit windowLevelChanged(w, l);
}

void VolumeView::onInteractorChar(vtkObject* caller, unsigned long /*eventId*/, void* /*clientData*/, void* /*callData*/, vtkCommand* command)
{
	auto* iren = vtkRenderWindowInteractor::SafeDownCast(caller);
	if (!iren) return;

	const int key = iren->GetKeyCode();
	const bool hasModifier = (iren->GetShiftKey() || iren->GetControlKey());

	if (key == 's' || key == 'S') {
		setShadingEnabled(!m_shadingEnabled);
		return;
	}

	// Plain r/R: restore WL + TFs to retained baseline; keep Shift/Ctrl+R for camera reset
	if ((key == 'r' || key == 'R') && !hasModifier) {
		resetWindowLevel();
		if (command) command->AbortFlagOn(); // don't let TrackballCamera reset camera on plain 'r'
	}
}

void VolumeView::onCameraModified(vtkObject* caller)
{
	auto master = vtkCamera::SafeDownCast(caller);

	if (!master || !m_orientationRenderer) return;

	auto slave = m_orientationRenderer->GetActiveCamera();
	if (!slave) return;

	// Copy direction and up
	double dop[3]; master->GetDirectionOfProjection(dop);
	double up[3];  master->GetViewUp(up);

	// Compute center to anchor marker camera (volume center if available)
	double center[3] = { 0.0, 0.0, 0.0 };
	if (m_imageData) {
		double b[6]; m_imageData->GetBounds(b);
		center[0] = 0.5 * (b[0] + b[1]);
		center[1] = 0.5 * (b[2] + b[3]);
		center[2] = 0.5 * (b[4] + b[5]);
	}

	// Match projection type / view angle
	if (master->GetParallelProjection()) {
		slave->ParallelProjectionOn();
	}
	else {
		slave->ParallelProjectionOff();
		slave->SetViewAngle(master->GetViewAngle());
	}

	// Choose a stable distance so the small cube is framed reasonably
	double dist = 1.0;
	if (m_imageData) {
		double b[6]; m_imageData->GetBounds(b);
		const double sx = b[1] - b[0];
		const double sy = b[3] - b[2];
		const double sz = b[5] - b[4];
		const double maxDim = std::max(std::max(sx, sy), sz);
		dist = std::max(1.5 * maxDim, 1.0);
	}

	// Position marker camera along -dop so it looks toward the center
	slave->SetFocalPoint(center);
	slave->SetPosition(center[0] - dop[0] * dist,
					   center[1] - dop[1] * dist,
					   center[2] - dop[2] * dist);
	slave->SetViewUp(up);
	slave->OrthogonalizeViewUp();

	render();
}

// Apply native-domain WL to the orthogonal image slice actors (mapped domain)
void VolumeView::setSliceWindowLevelNative(double window, double level)
{
	if (!m_imageData || !m_orthoPlanes) return;

	m_orthoPlanes->SetWindowLevelNative(window, level, m_scalarShift, m_scalarScale);
	render();
}

void VolumeView::captureDerivedViewState()
{
	// If no image, nothing to capture
	if (!m_imageData) return;

	// Save camera (deep copy)
	m_savedCamera = nullptr;
	if (m_renderer) {
		if (auto* cam = m_renderer->GetActiveCamera()) {
			m_savedCamera = vtkSmartPointer<vtkCamera>::New();
			m_savedCamera->DeepCopy(cam);
		}
	}

	// Compute a robust in-plane center using image bounds
	double bounds[6]; m_imageData->GetBounds(bounds);
	const double centerX = 0.5 * (bounds[0] + bounds[1]);
	const double centerY = 0.5 * (bounds[2] + bounds[3]);
	const double centerZ = 0.5 * (bounds[4] + bounds[5]);

	int ix = 0, iy = 0, iz = 0;
	if (m_orthoPlanes) {
		int centerIdx[3] = { 0, 0, 0 };
		m_orthoPlanes->GetSliceNumbers(centerIdx);
		ix = centerIdx[0];
		iy = centerIdx[1];
		iz = centerIdx[2];
	}

	// Save three world points (one for each orthogonal plane). Use vtkImageData transforms.
	// X-normal (YZ plane): use ix, centerY/centerZ -> need integer center indices for Y/Z
	int cyIdx = static_cast<int>(std::lround((centerY - m_origin[1]) / (m_spacing[1] != 0.0 ? m_spacing[1] : 1.0)));
	int czIdx = static_cast<int>(std::lround((centerZ - m_origin[2]) / (m_spacing[2] != 0.0 ? m_spacing[2] : 1.0)));
	{
		int ijk[3] = { ix, cyIdx, czIdx };
		m_imageData->TransformIndexToPhysicalPoint(ijk, m_savedSliceWorldX);
	}

	// Y-normal (XZ)
	int cxIdx = static_cast<int>(std::lround((centerX - m_origin[0]) / (m_spacing[0] != 0.0 ? m_spacing[0] : 1.0)));
	{
		int ijk[3] = { cxIdx, iy, czIdx };
		m_imageData->TransformIndexToPhysicalPoint(ijk, m_savedSliceWorldY);
	}

	// Z-normal (XY)
	{
		int ijk[3] = { cxIdx, cyIdx, iz };
		m_imageData->TransformIndexToPhysicalPoint(ijk, m_savedSliceWorldZ);
	}

	// Save visibility mode and TFs
	m_savedOrthoPlanesVisible = m_orthoPlanesVisible;

	m_savedActualColorTF = vtkSmartPointer<vtkColorTransferFunction>::New();
	if (m_actualColorTF) m_savedActualColorTF->DeepCopy(m_actualColorTF);
	else m_savedActualColorTF = nullptr;

	m_savedActualScalarOpacity = vtkSmartPointer<vtkPiecewiseFunction>::New();
	if (m_actualScalarOpacity) m_savedActualScalarOpacity->DeepCopy(m_actualScalarOpacity);
	else m_savedActualScalarOpacity = nullptr;

	m_hasSavedState = true;
}

void VolumeView::restoreDerivedViewState()
{
	if (!m_hasSavedState) return;

	// Restore TFs first so mapped TFs rebuild using current shift/scale
	if (m_savedActualColorTF) {
		if (!m_actualColorTF) m_actualColorTF = vtkSmartPointer<vtkColorTransferFunction>::New();
		m_actualColorTF->DeepCopy(m_savedActualColorTF);
	}
	if (m_savedActualScalarOpacity) {
		if (!m_actualScalarOpacity) m_actualScalarOpacity = vtkSmartPointer<vtkPiecewiseFunction>::New();
		m_actualScalarOpacity->DeepCopy(m_savedActualScalarOpacity);
	}

	// Recompute mapped TFs using current shift/scale and attach to the volume property
	updateMappedColorsFromActual();
	updateMappedOpacityFromActual();
	if (m_volumeProperty) {
		m_volumeProperty->SetColor(m_colorTF);
		m_volumeProperty->SetScalarOpacity(m_scalarOpacity);
	}

	// Ensure ortho-planes (or mappers) have up-to-date ranges
	if (m_orthoPlanes) {
		m_orthoPlanes->Update();
	}

	// Convert saved world points -> continuous indices and round to nearest index per axis.
	auto indexFromWorld = [this](int axis, const double world[3], int minVal, int maxVal) -> int {
		double cont[3] = { 0.0, 0.0, 0.0 };
		m_imageData->TransformPhysicalPointToContinuousIndex(world, cont);
		int idx = static_cast<int>(std::lround(cont[axis]));
		return std::clamp(idx, minVal, maxVal);
		};

	// Prefer vtkImageOrthoPlanes for restoring slice numbers; fall back to extents if ortho not present.
	if (m_orthoPlanes) {
		const int minX = m_extent[0], maxX = m_extent[1];
		const int minY = m_extent[2], maxY = m_extent[3];
		const int minZ = m_extent[4], maxZ = m_extent[5];

		int ix = indexFromWorld(0, m_savedSliceWorldX, minX, maxX);
		int iy = indexFromWorld(1, m_savedSliceWorldY, minY, maxY);
		int iz = indexFromWorld(2, m_savedSliceWorldZ, minZ, maxZ);

		m_orthoPlanes->SetSliceNumbers(ix, iy, iz);
		m_orthoPlanes->Update();
	}

	// Restore slice-planes visibility if needed (this will also call render())
	setOrthoPlanesVisible(m_savedOrthoPlanesVisible);

	// Restore camera if captured: use saved world intersection constructed from the three saved points.
	if (m_savedCamera && m_renderer) {
		if (auto* cam = m_renderer->GetActiveCamera()) {
			double savedDOP[3]; m_savedCamera->GetDirectionOfProjection(savedDOP);
			double savedUp[3];  m_savedCamera->GetViewUp(savedUp);

			// Intersection point: compose from saved per-plane points
			double focal[3] = {
				m_savedSliceWorldX[0],
				m_savedSliceWorldY[1],
				m_savedSliceWorldZ[2]
			};

			double dist = m_savedCamera->GetDistance();
			if (!(dist > 0.0)) dist = cam->GetDistance();

			cam->SetFocalPoint(focal);
			cam->SetPosition(focal[0] - savedDOP[0] * dist,
							 focal[1] - savedDOP[1] * dist,
							 focal[2] - savedDOP[2] * dist);
			cam->SetViewUp(savedUp);
			if (m_savedCamera->GetParallelProjection()) {
				cam->ParallelProjectionOn();
				cam->SetParallelScale(m_savedCamera->GetParallelScale());
			}
			else {
				cam->ParallelProjectionOff();
				cam->SetViewAngle(m_savedCamera->GetViewAngle());
			}
			cam->OrthogonalizeViewUp();
			m_renderer->ResetCameraClippingRange();
		}
	}

	// Final render
	if (!m_renderer) return;
	render();

	m_hasSavedState = false;
}


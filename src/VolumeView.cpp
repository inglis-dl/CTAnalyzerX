#include "VolumeView.h"
#include "ui_VolumeView.h"
#include "MenuButton.h"

#include <QAction>
#include <QMenu>
#include <QFrame>
#include <QTimer>
#include <QLayout>
#include <QSizePolicy>

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkGPUVolumeRayCastMapper.h>
#include <vtkVolume.h>
#include <vtkVolumeProperty.h>
#include <vtkImageData.h>
#include <vtkImagePlaneWidget.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>
#include <vtkCamera.h>
#include <vtkMath.h>
#include <vtkProperty.h> // added for plane colors

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

	// Plane widgets setup
	m_yzPlane = vtkSmartPointer<vtkImagePlaneWidget>::New(); // normal X
	m_xzPlane = vtkSmartPointer<vtkImagePlaneWidget>::New(); // normal Y
	m_xyPlane = vtkSmartPointer<vtkImagePlaneWidget>::New(); // normal Z

	m_yzPlane->SetInteractor(interactor);
	m_xzPlane->SetInteractor(interactor);
	m_xyPlane->SetInteractor(interactor);

	// Improve plane appearance/quality
	for (vtkImagePlaneWidget* pw : { m_yzPlane.GetPointer(), m_xzPlane.GetPointer(), m_xyPlane.GetPointer() }) {
		if (!pw) continue;
		pw->DisplayTextOff();
		pw->TextureInterpolateOn();
		pw->SetResliceInterpolateToLinear();
	}
	// Consistent axis colors: X=red, Y=green, Z=blue
	if (m_yzPlane && m_yzPlane->GetPlaneProperty()) m_yzPlane->GetPlaneProperty()->SetColor(1.0, 0.0, 0.0);
	if (m_xzPlane && m_xzPlane->GetPlaneProperty()) m_xzPlane->GetPlaneProperty()->SetColor(0.0, 1.0, 0.0);
	if (m_xyPlane && m_xyPlane->GetPlaneProperty()) m_xyPlane->GetPlaneProperty()->SetColor(0.0, 0.0, 1.0);
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
				setTitle(m_slicePlanesVisible ? QStringLiteral("Slice Planes") : QStringLiteral("Volume"));
				return;
			}

			if (item == QLatin1String("Volume")) {
				// Toggle to 3D volume rendering
				setSlicePlanesVisible(false);
			}
			else if (item == QLatin1String("Slice Planes")) {
				// Toggle to slice planes view
				setSlicePlanesVisible(true);
			}
			else if (item == QLatin1String("Reset Camera")) {
				resetCamera();
			}
			// Always restore the title to the current mode after any command
			setTitle(m_slicePlanesVisible ? QStringLiteral("Slice Planes") : QStringLiteral("Volume"));
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

void VolumeView::setImageData(vtkImageData* image)
{
	if (!image) return;

	m_imageData = image;

	// 1) Compute mapping and connect the shared filter
	computeShiftScaleFromInput(image);

	shiftScaleFilter->SetInputData(m_imageData);
	shiftScaleFilter->Update();

	// Plane widgets take the post-mapped data
	m_yzPlane->SetInputData(shiftScaleFilter->GetOutput());
	m_xzPlane->SetInputData(shiftScaleFilter->GetOutput());
	m_xyPlane->SetInputData(shiftScaleFilter->GetOutput());

	// Place widgets to data bounds so interactions/rendering are robust
	{
		double b[6] = { 0,0,0,0,0,0 };
		shiftScaleFilter->GetOutput()->GetBounds(b);
		m_yzPlane->PlaceWidget(b);
		m_xzPlane->PlaceWidget(b);
		m_xyPlane->PlaceWidget(b);
	}

	if (!m_imageInitialized) {
		// Volume mapper takes the post-mapped data
		m_mapper->SetInputConnection(shiftScaleFilter->GetOutputPort());

		m_yzPlane->SetPlaneOrientationToXAxes();
		m_xzPlane->SetPlaneOrientationToYAxes();
		m_xyPlane->SetPlaneOrientationToZAxes();

		m_imageInitialized = true;
	}

	// Rebuild the ACTUAL color TF to span the native image range (not 0..65535)
	{
		const double diff = m_scalarRangeMax - m_scalarRangeMin;
		const double lb = diff > 0.0 ? (m_scalarRangeMin + 0.01 * diff) : m_scalarRangeMin;
		const double ub = diff > 0.0 ? (m_scalarRangeMax - 0.01 * diff) : m_scalarRangeMax;

		m_actualColorTF->RemoveAllPoints();
		m_actualColorTF->AddRGBPoint(lb, 0.0, 0.0, 0.0);
		m_actualColorTF->AddRGBPoint(ub, 1.0, 1.0, 1.0);
		m_actualColorTF->Build();
	}

	// Remap ACTUAL -> MAPPED using current shift/scale, then attach to property
	updateMappedColorsFromActual();
	updateMappedOpacityFromActual();

	m_volumeProperty->SetColor(m_colorTF);
	m_volumeProperty->SetScalarOpacity(m_scalarOpacity);

	// Initialize WL in native domain
	{
		const double diff = m_scalarRangeMax - m_scalarRangeMin;
		const double lb = diff > 0.0 ? (m_scalarRangeMin + 0.01 * diff) : m_scalarRangeMin;
		const double ub = diff > 0.0 ? (m_scalarRangeMax - 0.01 * diff) : m_scalarRangeMax;
		const double baseWindow = std::max(ub - lb, 1.0);
		const double baseLevel = 0.5 * (ub + lb);
		setColorWindowLevel(baseWindow, baseLevel); // updates opacity+color and renders
	}

	// Optional: set scalar opacity unit distance to match data spacing
	{
		double sp[3] = { 1,1,1 };
		m_imageData->GetSpacing(sp);
		const double unit = (sp[0] + sp[1] + sp[2]) / 3.0;
		m_volumeProperty->SetScalarOpacityUnitDistance(unit);
	}

	// Camera/planes setup (unchanged)
	resetCamera();

	int extent[6] = { 0,0,0,0,0,0 };
	m_imageData->GetExtent(extent);
	m_yzPlane->SetSliceIndex((extent[0] + extent[1]) / 2);
	m_xzPlane->SetSliceIndex((extent[2] + extent[3]) / 2);
	m_xyPlane->SetSliceIndex((extent[4] + extent[5]) / 2);

	emit imageExtentsChanged(extent[0], extent[1], extent[2], extent[3], extent[4], extent[5]);

	setSlicePlanesVisible(m_slicePlanesVisible);
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
		break;
		case Linear:
		case Cubic: // VTK volume property doesn't provide cubic; fall back to linear
		m_volumeProperty->SetInterpolationTypeToLinear();
		break;
	}
	render();
	emit interpolationChanged(m_interpolation);
}

void VolumeView::updateSlicePlanes(int x, int y, int z)
{
	if (!m_imageInitialized || !m_imageData) return;

	int extent[6] = { 0,0,0,0,0,0 };
	m_imageData->GetExtent(extent);

	const int cx = std::clamp(x, extent[0], extent[1]);
	const int cy = std::clamp(y, extent[2], extent[3]);
	const int cz = std::clamp(z, extent[4], extent[5]);

	m_yzPlane->SetSliceIndex(cx);
	m_xzPlane->SetSliceIndex(cy);
	m_xyPlane->SetSliceIndex(cz);
	render();
}

void VolumeView::setSlicePlanesVisible(bool visible)
{
	const bool modified = (m_slicePlanesVisible != visible);
	m_slicePlanesVisible = visible;

	// Mutually exclusive: show either volume or planes, not both
	if (visible) {
		if (m_volume && m_renderer->HasViewProp(m_volume))
			m_renderer->RemoveVolume(m_volume);

		if (m_yzPlane) m_yzPlane->SetEnabled(true);
		if (m_xzPlane) m_xzPlane->SetEnabled(true);
		if (m_xyPlane) m_xyPlane->SetEnabled(true);

		// InteractionOff only AFTER SetEnabled(true) to avoid VTK warnings
		if (m_yzPlane) m_yzPlane->InteractionOff();
		if (m_xzPlane) m_xzPlane->InteractionOff();
		if (m_xyPlane) m_xyPlane->InteractionOff();
	}
	else {
		if (m_yzPlane) m_yzPlane->SetEnabled(false);
		if (m_xzPlane) m_xzPlane->SetEnabled(false);
		if (m_xyPlane) m_xyPlane->SetEnabled(false);

		if (m_volume && !m_renderer->HasViewProp(m_volume))
			m_renderer->AddVolume(m_volume);
	}
	if (modified)
		emit slicePlanesVisibleChanged(m_slicePlanesVisible);

	render();
}

void VolumeView::setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax)
{
	if (!m_mapper || !m_imageData) return;

	m_mapper->SetCropping(true);
	m_mapper->SetCroppingRegionPlanes(xMin, xMax, yMin, yMax, zMin, zMax);
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
	const bool changed = (m_viewOrientation != orientation);
	m_viewOrientation = orientation;

	vtkCamera* cam = m_renderer ? m_renderer->GetActiveCamera() : nullptr;

	if (!m_imageData || !m_renderer || !cam) {
		if (changed) emit viewOrientationChanged(m_viewOrientation);
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
	if (changed) emit viewOrientationChanged(m_viewOrientation);
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

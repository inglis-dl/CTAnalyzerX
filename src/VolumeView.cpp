#include "VolumeView.h"
#include "ui_VolumeView.h"
#include "MenuButton.h"

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

	// Create orthogonal vtkImageSlice / vtkImageSliceMapper actors for 3D slice-planes mode.
	m_sliceMapperYZ = vtkSmartPointer<vtkImageSliceMapper>::New();
	m_sliceMapperXZ = vtkSmartPointer<vtkImageSliceMapper>::New();
	m_sliceMapperXY = vtkSmartPointer<vtkImageSliceMapper>::New();

	m_sliceMapperYZ->StreamingOn();
	m_sliceMapperXZ->StreamingOn();
	m_sliceMapperXY->StreamingOn();

	m_imageSliceYZ = vtkSmartPointer<vtkImageSlice>::New();
	m_imageSliceXZ = vtkSmartPointer<vtkImageSlice>::New();
	m_imageSliceXY = vtkSmartPointer<vtkImageSlice>::New();

	m_imageSliceYZ->SetMapper(m_sliceMapperYZ);
	m_imageSliceXZ->SetMapper(m_sliceMapperXZ);
	m_imageSliceXY->SetMapper(m_sliceMapperXY);

	// Default interpolation for slice actors
	if (m_imageSliceYZ->GetProperty()) m_imageSliceYZ->GetProperty()->SetInterpolationTypeToLinear();
	if (m_imageSliceXZ->GetProperty()) m_imageSliceXZ->GetProperty()->SetInterpolationTypeToLinear();
	if (m_imageSliceXY->GetProperty()) m_imageSliceXY->GetProperty()->SetInterpolationTypeToLinear();

	createSliceOutlineActors();

	m_qvtk = vtkSmartPointer<vtkEventQtSlotConnect>::New();
	if (interactor) {
		// Use full-signature slot so we can abort plain 'r'/'R'
		m_qvtk->Connect(interactor, vtkCommand::CharEvent,
			this, SLOT(onInteractorChar(vtkObject*, unsigned long, void*, void*, vtkCommand*)),
			nullptr, 1.0f);

		m_qvtk->Connect(m_renderer->GetActiveCamera(), vtkCommand::ModifiedEvent,
			this, SLOT(onCameraModified(vtkObject*)), nullptr);
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

	// Compute mapping and connect the shared filter
	computeShiftScaleFromInput(image);

	m_shiftScaleFilter->SetInputData(m_imageData);
	m_shiftScaleFilter->Update();

	// TODO: attach ImageResliceHelper here to route post-shift/scale -> reslice -> mapper when integrating reslice workflow.
	//       e.g. helper->SetInputConnection(m_shiftScaleFilter->GetOutputPort()); mapper->SetInputConnection(helper->GetOutputPort());
	//
	// Feed orthogonal vtkImageSlice mappers from the post-shift/scale output so they can be shown in 3D mode.
	m_sliceMapperYZ->SetInputConnection(m_shiftScaleFilter->GetOutputPort());
	m_sliceMapperXZ->SetInputConnection(m_shiftScaleFilter->GetOutputPort());
	m_sliceMapperXY->SetInputConnection(m_shiftScaleFilter->GetOutputPort());

	// Ensure mapper orientation matches canonical axes (X normal => YZ plane, etc.)
	m_sliceMapperYZ->SetOrientationToX();
	m_sliceMapperXZ->SetOrientationToY();
	m_sliceMapperXY->SetOrientationToZ();
	//
	// Place slice actors / mappers to data bounds so rendering is robust.
	double b[6] = { 0,0,0,0,0,0 };
	m_shiftScaleFilter->GetOutput()->GetBounds(b);

	double spacing[3] = { 1,1,1 };
	m_imageData->GetSpacing(spacing);
	int extent[6] = { 0,0,0,0,0,0 };
	m_imageData->GetExtent(extent);
	double origin[3] = { 0,0,0 };
	m_imageData->GetOrigin(origin);

	const int cx = (extent[0] + extent[1]) / 2;
	const int cy = (extent[2] + extent[3]) / 2;
	const int cz = (extent[4] + extent[5]) / 2;

	if (!m_imageInitialized) {
		m_mapper->SetInputConnection(m_shiftScaleFilter->GetOutputPort());

		//
		// Add slices to the scene but keep them invisible until slicePlanesVisible is true.
		// Use AddViewProp so image slices render in the main renderer.
		if (m_imageSliceYZ && !m_renderer->HasViewProp(m_imageSliceYZ)) m_renderer->AddViewProp(m_imageSliceYZ);
		if (m_imageSliceXZ && !m_renderer->HasViewProp(m_imageSliceXZ)) m_renderer->AddViewProp(m_imageSliceXZ);
		if (m_imageSliceXY && !m_renderer->HasViewProp(m_imageSliceXY)) m_renderer->AddViewProp(m_imageSliceXY);
		//
		// Initially hide the slice actors (they will be shown when slicePlanesVisible==true)
		if (m_imageSliceYZ) m_imageSliceYZ->VisibilityOff();
		if (m_imageSliceXZ) m_imageSliceXZ->VisibilityOff();
		if (m_imageSliceXY) m_imageSliceXY->VisibilityOff();

		// add the outline actors to the scene
		if (m_outlineActorYZ && !m_renderer->HasViewProp(m_outlineActorYZ)) m_renderer->AddViewProp(m_outlineActorYZ);
		if (m_outlineActorXZ && !m_renderer->HasViewProp(m_outlineActorXZ)) m_renderer->AddViewProp(m_outlineActorXZ);
		if (m_outlineActorXY && !m_renderer->HasViewProp(m_outlineActorXY)) m_renderer->AddViewProp(m_outlineActorXY);
		if (m_outlineActorYZ) m_outlineActorYZ->VisibilityOff();
		if (m_outlineActorXZ) m_outlineActorXZ->VisibilityOff();
		if (m_outlineActorXY) m_outlineActorXY->VisibilityOff();

		const double wz = origin[2] + spacing[2] * static_cast<double>(cz);
		const double x0 = origin[0] + spacing[0] * extent[0];
		const double x1 = origin[0] + spacing[0] * extent[1];
		const double y0 = origin[1] + spacing[1] * extent[2];
		const double y1 = origin[1] + spacing[1] * extent[3];

		vtkSmartPointer<vtkPoints> ptsXY = vtkSmartPointer<vtkPoints>::New();
		ptsXY->SetNumberOfPoints(5);
		ptsXY->SetPoint(0, x0, y0, wz);
		ptsXY->SetPoint(1, x1, y0, wz);
		ptsXY->SetPoint(2, x1, y1, wz);
		ptsXY->SetPoint(3, x0, y1, wz);
		ptsXY->SetPoint(4, x0, y0, wz);

		vtkSmartPointer<vtkCellArray> lines = vtkSmartPointer<vtkCellArray>::New();
		vtkIdType ids[5] = { 0,1,2,3,4 };
		lines->InsertNextCell(5, ids);

		m_outlinePolyXY->SetPoints(ptsXY);
		m_outlinePolyXY->SetLines(lines);
		m_outlinePolyXY->Modified();

		const double wy = origin[1] + spacing[1] * static_cast<double>(cy);
		const double z0 = origin[2] + spacing[2] * extent[4];
		const double z1 = origin[2] + spacing[2] * extent[5];

		vtkSmartPointer<vtkPoints> ptsXZ = vtkSmartPointer<vtkPoints>::New();
		ptsXZ->SetNumberOfPoints(5);
		ptsXZ->SetPoint(0, x0, wy, z0);
		ptsXZ->SetPoint(1, x1, wy, z0);
		ptsXZ->SetPoint(2, x1, wy, z1);
		ptsXZ->SetPoint(3, x0, wy, z1);
		ptsXZ->SetPoint(4, x0, wy, z0);

		m_outlinePolyXZ->SetPoints(ptsXZ);
		m_outlinePolyXZ->SetLines(lines);
		m_outlinePolyXZ->Modified();

		const double wx = origin[0] + spacing[0] * static_cast<double>(cx);

		vtkSmartPointer<vtkPoints> ptsYZ = vtkSmartPointer<vtkPoints>::New();
		ptsYZ->SetNumberOfPoints(5);
		ptsYZ->SetPoint(0, wx, y0, z0);
		ptsYZ->SetPoint(1, wx, y1, z0);
		ptsYZ->SetPoint(2, wx, y1, z1);
		ptsYZ->SetPoint(3, wx, y0, z1);
		ptsYZ->SetPoint(4, wx, y0, z0); // close

		m_outlinePolyYZ->SetPoints(ptsYZ);
		m_outlinePolyYZ->SetLines(lines);
		m_outlinePolyYZ->Modified();

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

	// set scalar opacity unit distance to match data spacing
	const double unit = (spacing[0] + spacing[1] + spacing[2]) / 3.0;
	m_volumeProperty->SetScalarOpacityUnitDistance(unit);

	resetCamera();

	// Update static slice mappers to the center slices
	if (m_sliceMapperYZ) { m_sliceMapperYZ->SetSliceNumber(cx); m_sliceMapperYZ->Update(); }
	if (m_sliceMapperXZ) { m_sliceMapperXZ->SetSliceNumber(cy); m_sliceMapperXZ->Update(); }
	if (m_sliceMapperXY) { m_sliceMapperXY->SetSliceNumber(cz); m_sliceMapperXY->Update(); }


	// Reset cropping to full image extent to avoid applying stale/invalid crop planes
	if (m_mapper) {
		m_mapper->SetCroppingRegionPlanes(extent[0], extent[1],
										  extent[2], extent[3],
										  extent[4], extent[5]);
		m_mapper->SetCropping(false); // start with cropping off; UI can enable it
		// Notify UI that cropping has been disabled/reset for the new image
		emit croppingEnabledChanged(false);
	}

	emit imageExtentsChanged(extent[0], extent[1], extent[2], extent[3], extent[4], extent[5]);

	setSlicePlanesVisible(m_slicePlanesVisible);
	render();
}

void VolumeView::updateData()
{
	m_shiftScaleFilter->Update();
	m_mapper->Update();

	double spacing[3] = { 1,1,1 };
	m_imageData->GetSpacing(spacing);
	int extent[6] = { 0,0,0,0,0,0 };
	m_imageData->GetExtent(extent);
	double origin[3] = { 0,0,0 };
	m_imageData->GetOrigin(origin);

	const int cx = (extent[0] + extent[1]) / 2;
	const int cy = (extent[2] + extent[3]) / 2;
	const int cz = (extent[4] + extent[5]) / 2;
	updateSliceOutlineXY(cz);
	updateSliceOutlineXZ(cy);
	updateSliceOutlineYZ(cx);

	const double unit = (spacing[0] + spacing[1] + spacing[2]) / 3.0;
	m_volumeProperty->SetScalarOpacityUnitDistance(unit);

	resetCamera();

	// Update static slice mappers to the center slices
	if (m_sliceMapperYZ) { m_sliceMapperYZ->SetSliceNumber(cx); m_sliceMapperYZ->Update(); }
	if (m_sliceMapperXZ) { m_sliceMapperXZ->SetSliceNumber(cy); m_sliceMapperXZ->Update(); }
	if (m_sliceMapperXY) { m_sliceMapperXY->SetSliceNumber(cz); m_sliceMapperXY->Update(); }


	// Reset cropping to full image extent to avoid applying stale/invalid crop planes
	if (m_mapper) {
		m_mapper->SetCroppingRegionPlanes(extent[0], extent[1],
										  extent[2], extent[3],
										  extent[4], extent[5]);
		m_mapper->SetCropping(false); // start with cropping off; UI can enable it
		// Notify UI that cropping has been disabled/reset for the new image
		emit croppingEnabledChanged(false);
	}

	emit imageExtentsChanged(extent[0], extent[1], extent[2], extent[3], extent[4], extent[5]);

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

	// Move orthogonal imageSlice mappers to requested indices so the 3D view shows the same slices.
	if (m_sliceMapperYZ) { m_sliceMapperYZ->SetSliceNumber(cx); m_sliceMapperYZ->Update(); }
	if (m_sliceMapperXZ) { m_sliceMapperXZ->SetSliceNumber(cy); m_sliceMapperXZ->Update(); }
	if (m_sliceMapperXY) { m_sliceMapperXY->SetSliceNumber(cz); m_sliceMapperXY->Update(); }

	if (m_outlineActorXY) { updateSliceOutlineXY(cz); }
	if (m_outlineActorXZ) { updateSliceOutlineXZ(cy); }
	if (m_outlineActorYZ) { updateSliceOutlineYZ(cx); }

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

		// Show orthogonal imageSlice actors (static slices inside the 3D view)
		if (m_imageSliceYZ) m_imageSliceYZ->VisibilityOn();
		if (m_imageSliceXZ) m_imageSliceXZ->VisibilityOn();
		if (m_imageSliceXY) m_imageSliceXY->VisibilityOn();

		if (m_outlineActorYZ) m_outlineActorYZ->VisibilityOn();
		if (m_outlineActorXZ) m_outlineActorXZ->VisibilityOn();
		if (m_outlineActorXY) m_outlineActorXY->VisibilityOn();

	}
	else {
		// Hide the orthogonal image slice actors
		if (m_imageSliceYZ) m_imageSliceYZ->VisibilityOff();
		if (m_imageSliceXZ) m_imageSliceXZ->VisibilityOff();
		if (m_imageSliceXY) m_imageSliceXY->VisibilityOff();

		if (m_outlineActorYZ) m_outlineActorYZ->VisibilityOff();
		if (m_outlineActorXZ) m_outlineActorXZ->VisibilityOff();
		if (m_outlineActorXY) m_outlineActorXY->VisibilityOff();

		if (m_volume && !m_renderer->HasViewProp(m_volume))
			m_renderer->AddVolume(m_volume);
	}

	// Ensure the title and menu reflect the current mode so external UI (checkbox/menu) stays in sync.
	setTitle(m_slicePlanesVisible ? QStringLiteral("Slice Planes") : QStringLiteral("Volume"));

	if (modified)
		emit slicePlanesVisibleChanged(m_slicePlanesVisible);

	render();
}

void VolumeView::setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax)
{
	if (!m_mapper || !m_imageData) return;

	// Clamp indices to extent and ensure lo <= hi and non-degenerate
	int extent[6] = { 0,0,0,0,0,0 };
	m_imageData->GetExtent(extent); // {x0,x1,y0,y1,z0,z1}

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

	clampNormalize(xMin, xMax, extent[0], extent[1]);
	clampNormalize(yMin, yMax, extent[2], extent[3]);
	clampNormalize(zMin, zMax, extent[4], extent[5]);

	// Convert voxel (continuous index) -> physical/world coordinates
	double idx[3], physMin[3], physMax[3], physPt[3];

	// X min/max
	idx[0] = static_cast<double>(xMin); idx[1] = 0.0; idx[2] = 0.0;
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		// fallback to origin + spacing calculation if TransformContinuousIndexToPhysicalPoint is not appropriate
		double origin[3], spacing[3];
		m_imageData->GetOrigin(origin);
		m_imageData->GetSpacing(spacing);
		physMin[0] = origin[0] + idx[0] * spacing[0];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMin[0] = physPt[0];
	}
	idx[0] = static_cast<double>(xMax);
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		double origin[3], spacing[3];
		m_imageData->GetOrigin(origin);
		m_imageData->GetSpacing(spacing);
		physMax[0] = origin[0] + idx[0] * spacing[0];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMax[0] = physPt[0];
	}

	// Y min/max
	idx[0] = 0.0; idx[1] = static_cast<double>(yMin); idx[2] = 0.0;
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		double origin[3], spacing[3];
		m_imageData->GetOrigin(origin);
		m_imageData->GetSpacing(spacing);
		physMin[1] = origin[1] + idx[1] * spacing[1];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMin[1] = physPt[1];
	}
	idx[1] = static_cast<double>(yMax);
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		double origin[3], spacing[3];
		m_imageData->GetOrigin(origin);
		m_imageData->GetSpacing(spacing);
		physMax[1] = origin[1] + idx[1] * spacing[1];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMax[1] = physPt[1];
	}

	// Z min/max
	idx[0] = 0.0; idx[1] = 0.0; idx[2] = static_cast<double>(zMin);
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		double origin[3], spacing[3];
		m_imageData->GetOrigin(origin);
		m_imageData->GetSpacing(spacing);
		physMin[2] = origin[2] + idx[2] * spacing[2];
	}
	else {
		m_imageData->TransformContinuousIndexToPhysicalPoint(idx, physPt);
		physMin[2] = physPt[2];
	}
	idx[2] = static_cast<double>(zMax);
	if (m_imageData->HasAnyGhostCells() || m_imageData->GetNumberOfPoints() == 0) {
		double origin[3], spacing[3];
		m_imageData->GetOrigin(origin);
		m_imageData->GetSpacing(spacing);
		physMax[2] = origin[2] + idx[2] * spacing[2];
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
	// Guard
	if (!m_imageData) return;

	// Compute native lower/upper and map using shift/scale (same mapping as SliceView)
	const double lowerNative = level - 0.5 * std::fabs(window);
	const double upperNative = level + 0.5 * std::fabs(window);
	const double lowerMapped = (lowerNative + m_scalarShift) * m_scalarScale;
	const double upperMapped = (upperNative + m_scalarShift) * m_scalarScale;
	const double mappedWindow = std::max(upperMapped - lowerMapped, 1.0);
	const double mappedLevel = 0.5 * (upperMapped + lowerMapped);

	// Apply to each orthogonal image slice actor's property (if present)
	if (m_imageSliceYZ && m_imageSliceYZ->GetProperty()) {
		m_imageSliceYZ->GetProperty()->SetColorWindow(mappedWindow);
		m_imageSliceYZ->GetProperty()->SetColorLevel(mappedLevel);
	}
	if (m_imageSliceXZ && m_imageSliceXZ->GetProperty()) {
		m_imageSliceXZ->GetProperty()->SetColorWindow(mappedWindow);
		m_imageSliceXZ->GetProperty()->SetColorLevel(mappedLevel);
	}
	if (m_imageSliceXY && m_imageSliceXY->GetProperty()) {
		m_imageSliceXY->GetProperty()->SetColorWindow(mappedWindow);
		m_imageSliceXY->GetProperty()->SetColorLevel(mappedLevel);
	}

	// Render to reflect the change
	render();
}

void VolumeView::createSliceOutlineActors()
{
	// YZ outline
	m_outlinePolyYZ = vtkSmartPointer<vtkPolyData>::New();
	m_outlineMapperYZ = vtkSmartPointer<vtkPolyDataMapper>::New();
	m_outlineMapperYZ->SetInputData(m_outlinePolyYZ);
	m_outlineActorYZ = vtkSmartPointer<vtkActor>::New();
	m_outlineActorYZ->SetMapper(m_outlineMapperYZ);

	auto yzProp = m_outlineActorYZ->GetProperty();
	yzProp->SetRepresentationToWireframe();
	yzProp->SetColor(1.0, 0.0, 0.0); // red
	yzProp->SetLineWidth(2.0);
	yzProp->SetLighting(false);
	yzProp->SetSpecular(0.0);
	yzProp->SetDiffuse(0.0);
	yzProp->SetAmbient(1.0);

	m_outlineActorYZ->PickableOff();
	if (m_renderer && !m_renderer->HasViewProp(m_outlineActorYZ))
		m_renderer->AddActor(m_outlineActorYZ);

	m_outlineActorYZ->VisibilityOff();

	// XZ outline
	m_outlinePolyXZ = vtkSmartPointer<vtkPolyData>::New();
	m_outlineMapperXZ = vtkSmartPointer<vtkPolyDataMapper>::New();
	m_outlineMapperXZ->SetInputData(m_outlinePolyXZ);
	m_outlineActorXZ = vtkSmartPointer<vtkActor>::New();
	m_outlineActorXZ->SetMapper(m_outlineMapperXZ);

	auto xzProp = m_outlineActorXZ->GetProperty();
	xzProp->DeepCopy(yzProp); // copy style from YZ
	xzProp->SetColor(0.0, 1.0, 0.0); // green

	m_outlineActorXZ->PickableOff();
	if (m_renderer && !m_renderer->HasViewProp(m_outlineActorXZ))
		m_renderer->AddActor(m_outlineActorXZ);
	m_outlineActorXZ->VisibilityOff();

	// XY outline
	m_outlinePolyXY = vtkSmartPointer<vtkPolyData>::New();
	m_outlineMapperXY = vtkSmartPointer<vtkPolyDataMapper>::New();
	m_outlineMapperXY->SetInputData(m_outlinePolyXY);
	m_outlineActorXY = vtkSmartPointer<vtkActor>::New();
	m_outlineActorXY->SetMapper(m_outlineMapperXY);

	auto xyProp = m_outlineActorXY->GetProperty();
	xyProp->DeepCopy(yzProp); // copy style from YZ
	xyProp->SetColor(0.0, 0.0, 1.0); // blue

	m_outlineActorXY->PickableOff();
	if (m_renderer && !m_renderer->HasViewProp(m_outlineActorXY))
		m_renderer->AddActor(m_outlineActorXY);
	m_outlineActorXY->VisibilityOff();
}

// Build/update a 2D rectangular polyline in world coordinates for X-normal plane (YZ) at slice index cx.
void VolumeView::updateSliceOutlineYZ(int cx)
{
	if (!m_imageData || !m_outlinePolyYZ) return;

	int extent[6]; m_imageData->GetExtent(extent);
	double origin[3]; m_imageData->GetOrigin(origin);
	double spacing[3]; m_imageData->GetSpacing(spacing);

	// clamp cx
	cx = std::clamp(cx, extent[0], extent[1]);
	const double wx = origin[0] + spacing[0] * static_cast<double>(cx);
	const double y0 = origin[1] + spacing[1] * extent[2];
	const double y1 = origin[1] + spacing[1] * extent[3];
	const double z0 = origin[2] + spacing[2] * extent[4];
	const double z1 = origin[2] + spacing[2] * extent[5];

	// Reuse existing points & lines if present, otherwise create and attach them once.
	vtkPoints* pts = m_outlinePolyYZ->GetPoints();
	if (!pts) {
		vtkSmartPointer<vtkPoints> ptsNew = vtkSmartPointer<vtkPoints>::New();
		ptsNew->SetNumberOfPoints(5);
		m_outlinePolyYZ->SetPoints(ptsNew);
		pts = m_outlinePolyYZ->GetPoints();
	}
	// Ensure there is a line cell array (create once)
	if (!m_outlinePolyYZ->GetLines() || m_outlinePolyYZ->GetNumberOfCells() == 0) {
		vtkSmartPointer<vtkCellArray> lines = vtkSmartPointer<vtkCellArray>::New();
		vtkIdType ids[5] = { 0,1,2,3,4 };
		lines->InsertNextCell(5, ids);
		m_outlinePolyYZ->SetLines(lines);
	}

	// Update points in-place to avoid reallocations
	pts->SetPoint(0, wx, y0, z0);
	pts->SetPoint(1, wx, y1, z0);
	pts->SetPoint(2, wx, y1, z1);
	pts->SetPoint(3, wx, y0, z1);
	pts->SetPoint(4, wx, y0, z0); // close

	pts->Modified();
	m_outlinePolyYZ->Modified();
}

// Build/update rectangle for Y-normal plane (XZ) at slice index cy.
void VolumeView::updateSliceOutlineXZ(int cy)
{
	if (!m_imageData || !m_outlinePolyXZ) return;

	int extent[6]; m_imageData->GetExtent(extent);
	double origin[3]; m_imageData->GetOrigin(origin);
	double spacing[3]; m_imageData->GetSpacing(spacing);

	cy = std::clamp(cy, extent[2], extent[3]);
	const double wy = origin[1] + spacing[1] * static_cast<double>(cy);
	const double x0 = origin[0] + spacing[0] * extent[0];
	const double x1 = origin[0] + spacing[0] * extent[1];
	const double z0 = origin[2] + spacing[2] * extent[4];
	const double z1 = origin[2] + spacing[2] * extent[5];

	vtkPoints* pts = m_outlinePolyXZ->GetPoints();
	if (!pts) {
		vtkSmartPointer<vtkPoints> ptsNew = vtkSmartPointer<vtkPoints>::New();
		ptsNew->SetNumberOfPoints(5);
		m_outlinePolyXZ->SetPoints(ptsNew);
		pts = m_outlinePolyXZ->GetPoints();
	}
	if (!m_outlinePolyXZ->GetLines() || m_outlinePolyXZ->GetNumberOfCells() == 0) {
		vtkSmartPointer<vtkCellArray> lines = vtkSmartPointer<vtkCellArray>::New();
		vtkIdType ids[5] = { 0,1,2,3,4 };
		lines->InsertNextCell(5, ids);
		m_outlinePolyXZ->SetLines(lines);
	}

	pts->SetPoint(0, x0, wy, z0);
	pts->SetPoint(1, x1, wy, z0);
	pts->SetPoint(2, x1, wy, z1);
	pts->SetPoint(3, x0, wy, z1);
	pts->SetPoint(4, x0, wy, z0);

	pts->Modified();
	m_outlinePolyXZ->Modified();
}

// Build/update rectangle for Z-normal plane (XY) at slice index cz.
void VolumeView::updateSliceOutlineXY(int cz)
{
	if (!m_imageData || !m_outlinePolyXY) return;

	int extent[6]; m_imageData->GetExtent(extent);
	double origin[3]; m_imageData->GetOrigin(origin);
	double spacing[3]; m_imageData->GetSpacing(spacing);

	cz = std::clamp(cz, extent[4], extent[5]);
	const double wz = origin[2] + spacing[2] * static_cast<double>(cz);
	const double x0 = origin[0] + spacing[0] * extent[0];
	const double x1 = origin[0] + spacing[0] * extent[1];
	const double y0 = origin[1] + spacing[1] * extent[2];
	const double y1 = origin[1] + spacing[1] * extent[3];

	vtkPoints* pts = m_outlinePolyXY->GetPoints();
	if (!pts) {
		vtkSmartPointer<vtkPoints> ptsNew = vtkSmartPointer<vtkPoints>::New();
		ptsNew->SetNumberOfPoints(5);
		m_outlinePolyXY->SetPoints(ptsNew);
		pts = m_outlinePolyXY->GetPoints();
	}
	if (!m_outlinePolyXY->GetLines() || m_outlinePolyXY->GetNumberOfCells() == 0) {
		vtkSmartPointer<vtkCellArray> lines = vtkSmartPointer<vtkCellArray>::New();
		vtkIdType ids[5] = { 0,1,2,3,4 };
		lines->InsertNextCell(5, ids);
		m_outlinePolyXY->SetLines(lines);
	}

	pts->SetPoint(0, x0, y0, wz);
	pts->SetPoint(1, x1, y0, wz);
	pts->SetPoint(2, x1, y1, wz);
	pts->SetPoint(3, x0, y1, wz);
	pts->SetPoint(4, x0, y0, wz);

	pts->Modified();
	m_outlinePolyXY->Modified();
}

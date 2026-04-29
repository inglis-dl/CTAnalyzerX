#include "SliceView.h"
#include "ui_SliceView.h"
#include "SunkenSliderStyle.h"
#include "MenuButton.h"
#include "vtkImageSlicePointPlacer.h"
#include "vtkSliceOutlineSource.h"

#include <QAction>
#include <QMenu>
#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QIntValidator>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QDebug>
#include <QThread>

#include <cmath> // added for std::lround

#include <vtkActor.h>
#include <vtkAlgorithmOutput.h>
#include <vtkCamera.h>
#include <vtkCommand.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include "vtkImageOrthoPlanes.h"
#include <vtkImageProperty.h>
#include <vtkImageShiftScale.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkInformation.h>
#include <vtkInteractorStyleImage.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>


SliceView::SliceView(QWidget* parent, ViewOrientation initialOrientation)
	: ImageFrameWidget(parent)
	, ui(new Ui::SliceView)
{
	// Install Designer UI into frame body
	auto* content = new QFrame(this);
	ui->setupUi(content);
	setSceneContent(content);

	createMenuAndActions();

	// Hide the legacy "Slice: X" label above the slider
	if (ui->labelSliceInfo) {
		ui->labelSliceInfo->clear();
		ui->labelSliceInfo->hide();
	}

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

	// Build the enhanced bottom slider bar using the existing slider
	buildSliderBar(content);

	// Use parallel projection for 2D imaging (no perspective distortion)
	if (auto* cam = m_renderer->GetActiveCamera()) {
		cam->ParallelProjectionOn();
	}

	ui->renderArea->setRenderWindow(m_renderWindow); // mount VTK window into Qt widget
	setFocusProxy(ui->renderArea);                 // keyboard/mouse go to the view
	// Make render widget focusable and able to veto app shortcuts
	ui->renderArea->setFocusPolicy(Qt::StrongFocus);
	ui->renderArea->installEventFilter(this);

	// Ensure the slice slider keeps/gets focus when clicked even if the frame gets selected (header turns blue)
	if (ui->sliderSlicePosition) {
		ui->sliderSlicePosition->setFocusPolicy(Qt::StrongFocus);
		ui->sliderSlicePosition->installEventFilter(this);
	}

	// Set interactor style after the widget created one for the renderWindow
	if (auto* iren = m_renderWindow->GetInteractor()) {
		m_interactorStyle = vtkSmartPointer<vtkInteractorStyleImage>::New();
		m_interactorStyle->SetInteractionModeToImage2D();
		m_interactorStyle->SetDefaultRenderer(m_renderer);
		m_interactorStyle->AutoAdjustCameraClippingRangeOn();
		// important: allow default WL behavior even when we observe
		m_interactorStyle->SetHandleObservers(true);
		iren->SetInteractorStyle(m_interactorStyle);
	}

	// Initialize slice mapper and image slice
	m_sliceMapper = vtkSmartPointer<vtkImageSliceMapper>::New();

	m_imageSlice = vtkSmartPointer<vtkImageSlice>::New();
	m_imageSlice->SetMapper(m_sliceMapper);

	m_imageProperty = m_imageSlice->GetProperty();
	m_imageProperty->SetInterpolationTypeToLinear();
	m_imageSlice->SetProperty(m_imageProperty);

	// Enable automatic camera-facing for the slice
	m_sliceMapper->SliceFacesCameraOff();
	m_sliceMapper->SliceAtFocalPointOff();
	m_sliceMapper->SetInputConnection(m_shiftScaleFilter->GetOutputPort());

	m_pointPlacer = vtkSmartPointer<vtkImageSlicePointPlacer>::New();
	m_pointPlacer->SetImageSliceMapper(m_sliceMapper);
	m_pointPlacer->SetImageSlice(m_imageSlice);

	m_qvtkConnection = vtkSmartPointer<vtkEventQtSlotConnect>::New();
	m_qvtkConnection->Connect(m_interactorStyle, vtkCommand::LeftButtonPressEvent,
		this, SLOT(trapSpin(vtkObject*)));

	m_windowLevelStartPosition[0] = 0;
	m_windowLevelStartPosition[1] = 0;

	m_windowLevelCurrentPosition[0] = 0;
	m_windowLevelCurrentPosition[1] = 0;

	m_windowLevelInitial[0] = 1.0; // Window
	m_windowLevelInitial[1] = 0.5; // Level

	// Wire window-level lifecycle events from the interactor style.
	if (m_interactorStyle) {
		// Reset event (already handled)
		m_qvtkConnection->Connect(m_interactorStyle, vtkCommand::ResetWindowLevelEvent,
			this, SLOT(onResetWindowLevel(vtkObject*)));

		// Interactive WL (mouse-drag updates)
		m_qvtkConnection->Connect(m_interactorStyle, vtkCommand::WindowLevelEvent,
			this, SLOT(onInteractorWindowLevel(vtkObject*)), nullptr, -1.0f);

		// Start/End lifecycle so we can update UI and store baseline on end
		m_qvtkConnection->Connect(m_interactorStyle, vtkCommand::StartWindowLevelEvent,
			this, SLOT(onInteractorStartWindowLevel(vtkObject*)), nullptr, -1.0f);

		m_qvtkConnection->Connect(m_interactorStyle, vtkCommand::EndWindowLevelEvent,
			this, SLOT(onInteractorEndWindowLevel(vtkObject*)), nullptr, -1.0f);
	}

	connect(ui->sliderSlicePosition, &QSlider::valueChanged, this, &SliceView::setSliceIndex);

	// Keep only the editor in sync when slice changes (remove "Slice:" label usage)
	connect(this, &SliceView::sliceChanged, this, [this](int value) {
		if (m_editSliceIndex) {
			const QSignalBlocker b(m_editSliceIndex);
			m_editSliceIndex->setText(QString::number(value));
		}
	});

	setTitle(orientationLabel(m_viewOrientation));

	m_outlineSource = vtkSmartPointer<vtkSliceOutlineSource>::New();
	m_outlineSource->SetSliceMapper(m_sliceMapper);
	m_outlineMapper = vtkSmartPointer<vtkPolyDataMapper>::New();
	m_outlineMapper->SetInputConnection(m_outlineSource->GetOutputPort());
	m_outlineMapper->ScalarVisibilityOff();
	m_outlineActor = vtkSmartPointer<vtkActor>::New();
	m_outlineActor->SetMapper(m_outlineMapper);
	m_outlineVisible = false;
	m_outlineActor->SetVisibility(m_outlineVisible);
	m_outlineActor->GetProperty()->SetColor(1.0, 0.0, 0.0); // red
}

void SliceView::createMenuAndActions()
{
	// Populate the menu with orientations + command items handled via MenuButton::itemSelected
	setSelectionList({
		QStringLiteral("XY"),
		QStringLiteral("YZ"),
		QStringLiteral("XZ"),
		QStringLiteral("--"),
		QStringLiteral("Rotate +90\u00B0"),
		QStringLiteral("Rotate -90\u00B0"),
		QStringLiteral("Reset Camera")
	});

	// Drive behavior entirely from MenuButton::itemSelected
	if (auto* mb = menuButton()) {
		connect(mb, &MenuButton::itemSelected, this, [this](const QString& item) {
			// Orientation selections
			if (item == QLatin1String("XY") || item == QLatin1String("YZ") || item == QLatin1String("XZ")) {
				const ViewOrientation orient = labelToOrientation(item);
				setViewOrientation(orient);
				// Ensure title shows normalized orientation label
				setTitle(orientationLabel(orient));
				return;
			}

			// Command items
			if (item == QLatin1String("Rotate +90\u00B0")) {
				rotateCamera(+90.0);
			}
			else if (item == QLatin1String("Rotate -90\u00B0")) {
				rotateCamera(-90.0);
			}
			else if (item == QLatin1String("Reset Camera")) {
				resetCamera();
			}

			// Restore title/check to the current orientation after command actions
			setTitle(orientationLabel(m_viewOrientation));
		});
	}
}

SliceView::~SliceView()
{
	delete ui;
}

void SliceView::setInterpolation(Interpolation newInterpolation)
{
	if (newInterpolation != m_interpolation) {
		m_interpolation = newInterpolation;
		switch (m_interpolation) {
			case Nearest:
			m_imageProperty->SetInterpolationTypeToNearest();
			break;
			case Linear:
			m_imageProperty->SetInterpolationTypeToLinear();
			break;
			case Cubic:
			m_imageProperty->SetInterpolationTypeToCubic();
			break;
		}
		render();
		emit interpolationChanged(m_interpolation);
	}
}

void SliceView::buildSliderBar(QWidget* rootContent)
{
	// Find the original parent layout and index BEFORE reparenting the slider
	QLayout* originalLayout = nullptr;
	int insertIndex = -1;

	if (auto* pw = ui->sliderSlicePosition->parentWidget())
		originalLayout = pw->layout();
	if (!originalLayout)
		originalLayout = rootContent->layout();

	if (originalLayout)
		insertIndex = originalLayout->indexOf(ui->sliderSlicePosition);

	// Detach the slider from its original layout (the widget stays alive)
	if (originalLayout) {
		if (auto* box = qobject_cast<QBoxLayout*>(originalLayout)) {
			if (insertIndex >= 0) {
				if (auto* item = box->takeAt(insertIndex)) delete item;
			}
			else {
				box->removeWidget(ui->sliderSlicePosition);
			}
		}
		else if (auto* grid = qobject_cast<QGridLayout*>(originalLayout)) {
			if (insertIndex >= 0) {
				if (auto* item = grid->takeAt(insertIndex)) delete item;
			}
			else {
				grid->removeWidget(ui->sliderSlicePosition);
			}
		}
		else {
			originalLayout->removeWidget(ui->sliderSlicePosition);
		}
	}

	// Build the replacement bar: [minLabel] [slider] [maxLabel] [lineEdit]
	QWidget* bar = new QWidget(rootContent);
	auto* hl = new QHBoxLayout(bar);
	hl->setContentsMargins(6, 2, 6, 2);
	hl->setSpacing(6);

	// Bracketing labels
	m_labelMinSlice = new QLabel(QStringLiteral("0"), bar);
	m_labelMaxSlice = new QLabel(QStringLiteral("0"), bar);

	// Apply custom sunken style directly on the existing slider
	{
		auto* sunkenStyle = new SunkenSliderStyle(ui->sliderSlicePosition->style());
		// ensure lifetime ties to the slider to avoid leaks
		sunkenStyle->setParent(ui->sliderSlicePosition);
		ui->sliderSlicePosition->setStyle(sunkenStyle);
	}

	m_editSliceIndex = new QLineEdit(bar);
	m_editSliceIndex->setPlaceholderText(QStringLiteral("Slice #"));
	m_editSliceIndex->setFixedWidth(80);
	m_editSliceIndex->setAlignment(Qt::AlignLeft);
	m_editSliceIndex->setValidator(new QIntValidator(0, 0, m_editSliceIndex));

	// Prefer ClickFocus so mouse clicks into it behave correctly (avoids keyboard-only grabs).
	m_editSliceIndex->setFocusPolicy(Qt::ClickFocus);

	// Jump to typed slice when the user confirms
	connect(m_editSliceIndex, &QLineEdit::editingFinished, this, &SliceView::onEditorEditingFinished);
	connect(m_editSliceIndex, &QLineEdit::returnPressed, this, &SliceView::onEditorReturnPressed);

	// Compose (add slider directly, no QFrame wrapper)
	hl->addWidget(m_labelMinSlice, 0, Qt::AlignVCenter);
	hl->addWidget(ui->sliderSlicePosition, 1);
	hl->addWidget(m_labelMaxSlice, 0, Qt::AlignVCenter);
	hl->addWidget(m_editSliceIndex, 0, Qt::AlignVCenter);

	// 4) Insert the bar at the original slider spot
	if (originalLayout) {
		if (auto* box = qobject_cast<QBoxLayout*>(originalLayout)) {
			if (insertIndex >= 0) box->insertWidget(insertIndex, bar);
			else box->addWidget(bar);
		}
		else if (auto* grid = qobject_cast<QGridLayout*>(originalLayout)) {
			if (insertIndex >= 0) {
				int r, c, rs, cs;
				grid->getItemPosition(insertIndex, &r, &c, &rs, &cs);
				grid->addWidget(bar, r, c, rs, cs);
			}
			else {
				grid->addWidget(bar, grid->rowCount(), 0, 1, grid->columnCount() > 0 ? grid->columnCount() : 1);
			}
		}
		else {
			originalLayout->addWidget(bar);
		}
	}
	else {
		// Fallback
		if (auto* vb = qobject_cast<QVBoxLayout*>(rootContent->layout()))
			vb->addWidget(bar);
		else
			bar->setParent(rootContent);
	}
}

// SliceView destructor and other methods remain unchanged...

void SliceView::resetCamera()
{
	if (!m_renderer) return;

	// If no image yet, fall back to VTK default reset
	if (!m_imageData) {
		m_renderer->ResetCamera();
		render();
		return;
	}

	// Remember current slice so we don't jump after re-orthogonalizing
	const int keepSlice = m_currentSlice;

	// Clear any transforms that could emulate flips/rotations
	if (m_imageSlice) {
		m_imageSlice->SetOrientation(0.0, 0.0, 0.0);
		m_imageSlice->SetScale(1.0, 1.0, 1.0);
		m_imageSlice->SetUserTransform(nullptr);
		m_imageSlice->SetUserMatrix(nullptr);
	}

	// Rebuild an orthogonal camera aligned with current orientation
	updateCamera();

	// Restore the previously selected slice (clamped to valid range)
	setSliceIndex(std::clamp(keepSlice, m_minSlice, m_maxSlice));

	// Ensure clipping is sane and render
	m_renderer->ResetCameraClippingRange();
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
	if (!m_renderer) return;
	if (auto* cam = m_renderer->GetActiveCamera()) {
		cam->Roll(degrees);
		render();
	}
}

void SliceView::updateData()
{
	if (!m_imageData) return;

	// Compute mapping and connect the shared filter
	computeShiftScaleFromInput();
	cacheImageGeometry();

	if (m_requestedCroppingRegion) {
		m_requestedCroppingRegion[0] = m_extent[0];
		m_requestedCroppingRegion[1] = m_extent[1];
		m_requestedCroppingRegion[2] = m_extent[2];
		m_requestedCroppingRegion[3] = m_extent[3];
		m_requestedCroppingRegion[4] = m_extent[4];
		m_requestedCroppingRegion[5] = m_extent[5];

	}
	m_requestedCroppingEnabled = false;

	// Ensure mapper orientation matches current view as soon as input exists
	switch (m_viewOrientation) {
		case VIEW_ORIENTATION_YZ: m_sliceMapper->SetOrientationToX(); break;
		case VIEW_ORIENTATION_XZ: m_sliceMapper->SetOrientationToY(); break;
		case VIEW_ORIENTATION_XY:
		default:                  m_sliceMapper->SetOrientationToZ(); break;
	}

	if (!m_imageInitialized) {
		m_renderer->AddViewProp(m_imageSlice);
		m_imageSlice->PickableOn();
		if (!m_renderer->HasViewProp(m_outlineActor)) {
			m_renderer->AddActor(m_outlineActor);
		}
		m_imageInitialized = true;
	}

	// Compute a native-domain baseline WL (trim 1% like VolumeView) and retain it	
	const double diff = m_scalarRangeMax - m_scalarRangeMin;
	const double lb = diff > 0.0 ? (m_scalarRangeMin + 0.01 * diff) : m_scalarRangeMin;
	const double ub = diff > 0.0 ? (m_scalarRangeMax - 0.01 * diff) : m_scalarRangeMax;
	const double baseWindowNative = std::max(ub - lb, 1.0);
	const double baseLevelNative = 0.5 * (ub + lb);
	setBaselineWindowLevel(baseWindowNative, baseLevelNative);

	// Store the original baseline computed from the input image.
	// This original baseline must not be overwritten by interactive WL
	// and will be used by resetWindowLevel() until the next setImageData().
	m_originalBaselineValid = true;
	m_originalBaselineWindowNative = baseWindowNative;
	m_originalBaselineLevelNative = baseLevelNative;

	// Map baseline to the post-shift/scale domain used by vtkImageProperty
	const double lowerNative = baseLevelNative - 0.5 * baseWindowNative;
	const double upperNative = baseLevelNative + 0.5 * baseWindowNative;
	const double lowerMapped = (lowerNative + m_scalarShift) * m_scalarScale;
	const double upperMapped = (upperNative + m_scalarShift) * m_scalarScale;
	const double mappedWindow = std::max(upperMapped - lowerMapped, 1.0);
	const double mappedLevel = 0.5 * (upperMapped + lowerMapped);

	m_imageProperty->SetColorWindow(mappedWindow);
	m_imageProperty->SetColorLevel(mappedLevel);

	// Emit native-domain signal so UI controls reflect the newly loaded image.
	emit windowLevelChanged(baseWindowNative, baseLevelNative);

	// Set camera and show a valid slice immediately (center)
	updateCamera();

	// current slice is set by camera update, so just refresh range and pick center
	updateSliceRange();

	// set the slice mapper slice
	setSliceIndex(m_currentSlice);
}

void SliceView::updateCamera() {
	if (!m_imageData)	return;

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
	bounds[2 * u] = m_origin[u] + m_spacing[u] * m_extent[2 * u];
	bounds[2 * u + 1] = m_origin[u] + m_spacing[u] * m_extent[2 * u + 1];
	bounds[2 * v] = m_origin[v] + m_spacing[v] * m_extent[2 * v];
	bounds[2 * v + 1] = m_origin[v] + m_spacing[v] * m_extent[2 * v + 1];
	bounds[2 * w] = m_origin[w] + m_spacing[w] * m_extent[2 * w];
	bounds[2 * w + 1] = bounds[2 * w]; // zero thickness in view direction

	double fpt[3];
	double pos[3];
	double vup[3] = { 0.0, 0.0, 0.0 };
	double vpn[3] = { 0.0, 0.0, 0.0 };
	vup[v] = 1.0;  // up is the second in-plane axis
	vpn[w] = 1.0;  // look along the view-normal axis

	fpt[u] = pos[u] = m_origin[u] + 0.5 * m_spacing[u] * (m_extent[2 * u] + m_extent[2 * u + 1]);
	fpt[v] = pos[v] = m_origin[v] + 0.5 * m_spacing[v] * (m_extent[2 * v] + m_extent[2 * v + 1]);
	fpt[w] = m_origin[w] + m_spacing[w] * (1 == w ? m_extent[2 * w + 1] : m_extent[2 * w]);
	pos[w] = fpt[w] + vpn[w] * m_spacing[w];

	auto camera = m_renderer->GetActiveCamera();
	camera->ParallelProjectionOn(); // ensure 2D projection
	camera->SetFocalPoint(fpt);
	camera->SetPosition(pos);
	camera->SetViewUp(vup);
	camera->OrthogonalizeViewUp();  // guard against accumulated roll

	// Fit the slice to the viewport and set sensible distance/scale
	m_renderer->ResetCamera(bounds);
	m_renderer->ResetCameraClippingRange(bounds);

	// Move current slice to the start of the axis (will be centered later)
	m_currentSlice = static_cast<int>((m_extent[2 * w] + m_extent[2 * w + 1]) / 2);
}

void SliceView::setViewOrientation(ImageFrameWidget::ViewOrientation orientation)
{
	if (m_viewOrientation == orientation)
		return;

	// First update state
	m_viewOrientation = orientation;

	// Now keep title and menu state in sync with the new orientation
	setTitle(orientationLabel(m_viewOrientation));

	// If no image/pipeline yet, just broadcast and return (avoid VTK errors).
	if (!m_imageData || !m_sliceMapper || m_sliceMapper->GetNumberOfInputConnections(0) == 0) {
		notifyViewOrientationChanged(); // base helper
		return;
	}

	// Update mapper orientation for the selected plane
	switch (m_viewOrientation) {
		case VIEW_ORIENTATION_YZ: m_sliceMapper->SetOrientationToX(); break; // z-y plane x normal
		case VIEW_ORIENTATION_XZ: m_sliceMapper->SetOrientationToY(); break; // x-z plane y normal
		case VIEW_ORIENTATION_XY:
		default: m_sliceMapper->SetOrientationToZ(); break; // x-y plane z normal
	}

	// Recompute slice range and camera, then pick a visible slice (center)

	updateCamera();
	updateSliceRange();
	setSliceIndex((m_minSlice + m_maxSlice) / 2); // also triggers render()

	notifyViewOrientationChanged(); // base helper
}

void SliceView::updateSliceRange() {
	// Guard against mapper without input to avoid VTK errors
	if (!m_sliceMapper || m_sliceMapper->GetNumberOfInputConnections(0) == 0)
		return;

	m_minSlice = m_sliceMapper->GetSliceNumberMinValue();
	m_maxSlice = m_sliceMapper->GetSliceNumberMaxValue();

	// Ensure current slice is inside the recovered/valid range
	m_currentSlice = std::clamp(m_currentSlice, m_minSlice, m_maxSlice);

	ui->sliderSlicePosition->setMinimum(m_minSlice);
	ui->sliderSlicePosition->setMaximum(m_maxSlice);

	// NEW: keep bracket labels in sync with the computed range
	if (m_labelMinSlice) {
		m_labelMinSlice->setText(QString::number(m_minSlice));
	}
	if (m_labelMaxSlice) {
		m_labelMaxSlice->setText(QString::number(m_maxSlice));
	}

	// Update editor range/text
	if (m_editSliceIndex) {
		const QValidator* val = m_editSliceIndex->validator();
		QIntValidator* iv = qobject_cast<QIntValidator*>(const_cast<QValidator*>(val));
		if (iv) {
			iv->setBottom(m_minSlice);
			iv->setTop(m_maxSlice);
		}
		const QSignalBlocker b(m_editSliceIndex);
		m_editSliceIndex->setText(QString::number(m_currentSlice));
	}
}

void SliceView::updateSlice() {
	if (!m_imageData) return;

	m_sliceMapper->SetSliceNumber(m_currentSlice);

	// Push the new slice index to the linked ortho-plane for the axis that
	// matches this view's orientation, preserving the other two axes unchanged.
	if (m_linkedOrthoPlanes)
	{
		int cur[3] = { 0, 0, 0 };
		m_linkedOrthoPlanes->GetSliceNumbers(cur);

		switch (m_viewOrientation)
		{
			case VIEW_ORIENTATION_YZ: cur[0] = m_currentSlice; break; // X-normal plane
			case VIEW_ORIENTATION_XZ: cur[1] = m_currentSlice; break; // Y-normal plane
			case VIEW_ORIENTATION_XY:
			default:                  cur[2] = m_currentSlice; break; // Z-normal plane
		}

		m_linkedOrthoPlanes->SetSliceNumbers(cur[0], cur[1], cur[2]);
		m_linkedOrthoPlanes->Update();
	}

	int u = 0, v = 1, w = m_viewOrientation;
	switch (w)
	{
		case 0: u = 1; v = 2; break;
		case 1: u = 0; v = 2; break;
		case 2: u = 0; v = 1; break;
	}

	auto cam = m_renderer->GetActiveCamera();
	if (cam)
	{
		double fpt[3];
		cam->GetFocalPoint(fpt);
		fpt[w] = m_origin[w] + m_spacing[w] * m_currentSlice;

		const double* vpn = cam->GetViewPlaneNormal();
		const double d = cam->GetDistance();
		cam->SetFocalPoint(fpt);

		double pos[3];
		pos[u] = fpt[u];
		pos[v] = fpt[v];
		pos[w] = fpt[w] + d * vpn[w];
		cam->SetPosition(pos);
	}

	m_renderer->ResetCameraClippingRange(); // ensure slice is not clipped
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

	// Sync slider
	{
		const QSignalBlocker b(ui->sliderSlicePosition);
		ui->sliderSlicePosition->setValue(m_currentSlice);
	}
	// Sync editor
	if (m_editSliceIndex) {
		const QSignalBlocker b(m_editSliceIndex);
		m_editSliceIndex->setText(QString::number(m_currentSlice));
	}

	// updates the slice mapper
	// updates the camera based on slice position
	updateSlice();

	emit sliceChanged(m_currentSlice);
}

int SliceView::getSliceIndex() const {
	return m_currentSlice;
}

void SliceView::trapSpin(vtkObject* obj)
{
	auto style = vtkInteractorStyleImage::SafeDownCast(obj);
	if (style->GetInteractor()->GetControlKey())
		return;

	style->OnLeftButtonDown();
}

// Allow the render widget to prevent QShortcut from stealing VTK keys
bool SliceView::eventFilter(QObject* watched, QEvent* event)
{
	// Ensure the slider retains focus when clicked and also selects this frame so the title highlights.
	if (watched == ui->sliderSlicePosition) {
		switch (event->type()) {
			case QEvent::MouseButtonPress:
			case QEvent::MouseButtonDblClick: {
				// Select this frame so title highlights and interactor gating switches here.
				if (!isSelected()) {
					setSelected(true);
				}
				// Give focus to the slider for immediate drag. Do NOT queue a delayed focus restore.
				if (!ui->sliderSlicePosition->hasFocus()) {
					ui->sliderSlicePosition->setFocus(Qt::MouseFocusReason);
				}
				break;
			}
			case QEvent::FocusIn: {
				// If slider gains focus (via click or Tab), ensure this frame is selected.
				if (!isSelected()) {
					setSelected(true);
				}
				break;
			}
			case QEvent::FocusOut: {
				break;
			}
			default:
			break;
		}
		return false;
	}

	if (watched == ui->renderArea && event->type() == QEvent::ShortcutOverride) {
		// Allow VTK keys to be handled when either:
		// - interaction is not restricted to selection, or
		// - this frame is selected, or
		// - the render widget itself currently has keyboard focus.
		if (!restrictInteractionToSelection() || isSelected() ||
			(ui->renderArea && ui->renderArea->hasFocus())) {
			auto* ke = reinterpret_cast<QKeyEvent*>(event);
			const Qt::KeyboardModifiers mods = ke->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
			if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
				switch (ke->key()) {
					case Qt::Key_R:
					case Qt::Key_F:
					case Qt::Key_X:
					case Qt::Key_Y:
					case Qt::Key_Z:
					ke->accept();
					return true;
					default:
					break;
				}
			}
		}
	}
	return SelectionFrameWidget::eventFilter(watched, event);
}

void SliceView::setWindowLevelNative(double window, double level)
{
	if (!m_imageData) return;

	// Compute native lower/upper and map using shift/scale (same mapping as in setImageData/resetWindowLevel)
	const double lowerNative = level - 0.5 * std::fabs(window);
	const double upperNative = level + 0.5 * std::fabs(window);
	const double lowerMapped = (lowerNative + m_scalarShift) * m_scalarScale;
	const double upperMapped = (upperNative + m_scalarShift) * m_scalarScale;
	const double mappedWindow = std::max(upperMapped - lowerMapped, 1.0);
	const double mappedLevel = 0.5 * (upperMapped + lowerMapped);

	if (m_imageProperty) {
		m_imageProperty->SetColorWindow(mappedWindow);
		m_imageProperty->SetColorLevel(mappedLevel);
	}

	// Update interactor style baseline so plain 'r' will restore this WL
	if (auto* iren = m_renderWindow->GetInteractor()) {
		if (auto* style = vtkInteractorStyleImage::SafeDownCast(iren->GetInteractorStyle())) {
			style->SetDefaultRenderer(m_renderer);
			style->SetCurrentRenderer(m_renderer);
			style->StartWindowLevel();
			style->EndWindowLevel();
		}
	}

	render();
	emit windowLevelChanged(window, level);
}

void SliceView::resetWindowLevel()
{
	// Apply retained baseline: convert native baseline -> mapped domain in base class
	if (!m_imageData || !m_imageProperty) return;

	const auto [windowMapped, levelMapped] = baselineMapped();
	if (!std::isfinite(windowMapped) || !std::isfinite(levelMapped)) return;

	// Apply mapped window/level to the image property (mapped domain)
	m_imageProperty->SetColorWindow(windowMapped);
	m_imageProperty->SetColorLevel(levelMapped);

	// Ensure interactor style uses same baseline so plain 'r' restores it
	updateInteractorWindowLevelBaseline();

	// Refresh display
	render();

	// Also emit native-domain window/level so controllers/listeners update.
	// mapped -> native conversion: nativeWindow = mappedWindow / scale;
	// nativeLevel = (mappedLevel / scale) - shift;
	if (std::isfinite(m_scalarScale) && m_scalarScale != 0.0) {
		const double nativeWindow = std::max(windowMapped / m_scalarScale, 1.0);
		const double nativeLevel = (levelMapped / m_scalarScale) - m_scalarShift;
		emit windowLevelChanged(nativeWindow, nativeLevel);
	}
}

void SliceView::updateInteractorWindowLevelBaseline()
{
	// Update interactor style baseline to match the current image property WL
	if (!m_imageData || !m_imageProperty || !m_interactorStyle) return;

	m_interactorStyle->SetDefaultRenderer(m_renderer);
	m_interactorStyle->SetCurrentRenderer(m_renderer);
	m_interactorStyle->SetCurrentImageNumber(-1);
	m_interactorStyle->StartWindowLevel();
	m_interactorStyle->EndWindowLevel();
}

void SliceView::onResetWindowLevel(vtkObject* /*obj*/)
{
	// The user pressed 'r' in this slice. Emit a request signal so the Lightbox/Controller
	// can perform a single coordinated reset for all views. This avoids this slice's
	// programmatic baseline update generating windowLevelChanged and propagating to siblings.
	emit requestResetWindowLevel();
}

void SliceView::onInteractorWindowLevel(vtkObject* caller)
{
	// caller is the vtkInteractorStyleImage that invoked the event
	auto* style = vtkInteractorStyleImage::SafeDownCast(caller);
	if (!style) return;

	// Need interactor and current image property
	auto* iren = style->GetInteractor();
	vtkImageProperty* prop = style->GetCurrentImageProperty();
	if (!iren || !prop || !m_imageData) return;

	// Get viewport size (use render window size as a robust fallback)
	int size[2] = { 1, 1 };
	if (iren->GetRenderWindow()) {
		const int* s = iren->GetRenderWindow()->GetSize();
		if (s) { size[0] = s[0]; size[1] = s[1]; }
	}

	m_windowLevelCurrentPosition[0] = style->GetWindowLevelCurrentPosition()[0];
	m_windowLevelCurrentPosition[1] = style->GetWindowLevelCurrentPosition()[1];

	double window = m_windowLevelInitial[0];
	double level = m_windowLevelInitial[1];

	// Compute normalized delta

	double dx =
		(m_windowLevelCurrentPosition[0] - m_windowLevelStartPosition[0]) * 4.0 / size[0];
	double dy =
		(m_windowLevelStartPosition[1] - m_windowLevelCurrentPosition[1]) * 4.0 / size[1];

	// Scale by current values

	if (fabs(window) > 0.01)
	{
		dx = dx * window;
	}
	else
	{
		dx = dx * (window < 0 ? -0.01 : 0.01);
	}
	if (fabs(level) > 0.01)
	{
		dy = dy * level;
	}
	else
	{
		dy = dy * (level < 0 ? -0.01 : 0.01);
	}

	// Abs so that direction does not flip

	if (window < 0.0)
	{
		dx = -1 * dx;
	}
	if (level < 0.0)
	{
		dy = -1 * dy;
	}

	// Compute new window level

	double newWindow = dx + window;
	double newLevel = level - dy;

	if (fabs(newWindow) < 0.01)
	{
		newWindow = 0.01 * (newWindow < 0 ? -1 : 1);
	}
	if (fabs(newLevel) < 0.01)
	{
		newLevel = 0.01 * (newLevel < 0 ? -1 : 1);
	}

	newLevel = std::clamp(newLevel, m_mappedDataMin, m_mappedDataMax);

	// Apply mapped-domain change to the image property (we must do this because style did not)
	prop->SetColorWindow(newWindow);
	prop->SetColorLevel(newLevel);
	iren->Render();

	// Convert mapped -> native domain (inverse of (native + shift) * scale)
	const double lowerMapped = newLevel - 0.5 * std::fabs(newWindow);
	const double upperMapped = newLevel + 0.5 * std::fabs(newWindow);
	const double lowerNative = (lowerMapped / m_scalarScale) - m_scalarShift;
	const double upperNative = (upperMapped / m_scalarScale) - m_scalarShift;
	const double nativeWindow = std::max(upperNative - lowerNative, 1.0);
	const double nativeLevel = 0.5 * (upperNative + lowerNative);

	// Emit native-domain signal for bridge-controller
	emit windowLevelChanged(nativeWindow, nativeLevel);
}

void SliceView::onInteractorStartWindowLevel(vtkObject* caller)
{
	// Called when a WL interaction starts. Read current mapped WL and update UI.
	auto* style = vtkInteractorStyleImage::SafeDownCast(caller);
	if (!style) return;

	vtkImageProperty* prop = style->GetCurrentImageProperty();
	if (!prop || !m_imageData) return;

	m_windowLevelInitial[0] = prop->GetColorWindow();
	m_windowLevelInitial[1] = prop->GetColorLevel();

	m_windowLevelStartPosition[0] = style->GetWindowLevelStartPosition()[0];
	m_windowLevelStartPosition[1] = style->GetWindowLevelStartPosition()[1];
}

void SliceView::onInteractorEndWindowLevel(vtkObject* caller)
{
	// Called when WL interaction ends. Convert mapped->native, store baseline and emit final.
	auto* style = vtkInteractorStyleImage::SafeDownCast(caller);
	if (!style) return;

	vtkImageProperty* prop = style->GetCurrentImageProperty();
	if (!prop || !m_imageData) return;

	const double mappedWindow = prop->GetColorWindow();
	const double mappedLevel = prop->GetColorLevel();

	const double lowerMapped = mappedLevel - 0.5 * std::fabs(mappedWindow);
	const double upperMapped = mappedLevel + 0.5 * std::fabs(mappedWindow);
	const double lowerNative = (lowerMapped / m_scalarScale) - m_scalarShift;
	const double upperNative = (upperMapped / m_scalarScale) - m_scalarShift;
	const double nativeWindow = std::max(upperNative - lowerNative, 1.0);
	const double nativeLevel = 0.5 * (upperNative + lowerNative);

	// DO NOT overwrite the original baseline here.
	// We still emit the interactive result so controllers/bridges can react,
	// but the retained baseline used by resetWindowLevel() must remain the
	// original values computed in setImageData().

	emit windowLevelChanged(nativeWindow, nativeLevel);
}

void SliceView::onEditorEditingFinished()
{
	if (!m_editSliceIndex) return;

	bool ok = false;
	const int v = m_editSliceIndex->text().toInt(&ok);
	if (ok) {
		setSliceIndex(v);
	}
	else {
		// restore current valid value
		const QSignalBlocker b(m_editSliceIndex);
		m_editSliceIndex->setText(QString::number(m_currentSlice));
	}
}

void SliceView::onEditorReturnPressed()
{
	if (!m_editSliceIndex) return;

	bool ok = false;
	const int v = m_editSliceIndex->text().toInt(&ok);
	if (ok) setSliceIndex(v);
}

// new method: install a shared vtkImageProperty (sharedProp may be the same instance across views)
void SliceView::setSharedImageProperty(vtkImageProperty* sharedProp)
{
	if (!sharedProp || !m_imageSlice)
		return;

	// Ensure execution on GUI thread. If we're called from another thread,
	// re-post the call to the object's thread (queued) and return.
	if (QThread::currentThread() != this->thread()) {
		vtkImageProperty* prop = sharedProp;
		QMetaObject::invokeMethod(
			this,
			[this, prop]() { this->setSharedImageProperty(prop); },
			Qt::QueuedConnection);
		return;
	}

	// Idempotent: if this view already uses the requested property, do nothing.
	vtkImageProperty* safeProp = vtkImageProperty::SafeDownCast(sharedProp);
	if (m_imageProperty == safeProp)
		return;

	// Install the shared property (atomic with respect to this object since we're on GUI thread)
	m_imageSlice->SetProperty(sharedProp);
	m_imageProperty = safeProp;

	// Update the interactor baseline synchronously (required for WL baseline correctness).
	updateInteractorWindowLevelBaseline();

	// Emit native-domain window/level so UI controls reflect the newly installed property.
	// Convert mapped (vtkImageProperty) -> native domain (inverse of (native + shift) * scale).
	if (m_imageProperty) {
		const double mappedWindow = m_imageProperty->GetColorWindow();
		const double mappedLevel = m_imageProperty->GetColorLevel();
		const double lowerMapped = mappedLevel - 0.5 * std::fabs(mappedWindow);
		const double upperMapped = mappedLevel + 0.5 * std::fabs(mappedWindow);
		const double lowerNative = (lowerMapped / m_scalarScale) - m_scalarShift;
		const double upperNative = (upperMapped / m_scalarScale) - m_scalarShift;
		const double nativeWindow = std::max(upperNative - lowerNative, 1.0);
		const double nativeLevel = 0.5 * (upperNative + lowerNative);
		emit windowLevelChanged(nativeWindow, nativeLevel);
	}

	// Defer render to the event loop to avoid nested / re-entrant rendering and ordering races
	// between multiple views that may also be changing the same property.
	QTimer::singleShot(0, this, [this]() { this->render(); });
}

void SliceView::captureDerivedViewState()
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

	// Build a continuous index from the camera focal point (preserve in-plane centering)
	// and replace the view-normal component with the current slice index so the saved
	// world point corresponds to the visible slice location.
	double contIdx[3] = { 0.0, 0.0, 0.0 };
	if (m_renderer && m_renderer->GetActiveCamera()) {
		double focal[3];
		m_renderer->GetActiveCamera()->GetFocalPoint(focal);
		// Transform physical focal -> continuous index
		m_imageData->TransformPhysicalPointToContinuousIndex(focal, contIdx);
	}
	else {
		// Fallback: use the center indices of the current extent
		contIdx[0] = 0.5 * (m_extent[0] + m_extent[1]);
		contIdx[1] = 0.5 * (m_extent[2] + m_extent[3]);
		contIdx[2] = 0.5 * (m_extent[4] + m_extent[5]);
	}

	// Overwrite the view-normal component with the discrete slice index
	const int w = static_cast<int>(m_viewOrientation);
	contIdx[w] = static_cast<double>(m_currentSlice);

	// Convert continuous index -> physical/world coordinate and store
	double savedWorld[3] = { 0.0, 0.0, 0.0 };
	m_imageData->TransformContinuousIndexToPhysicalPoint(contIdx, savedWorld);
	m_savedSliceWorld[0] = savedWorld[0];
	m_savedSliceWorld[1] = savedWorld[1];
	m_savedSliceWorld[2] = savedWorld[2];

	// Save mapped WL from the image property (if present)
	if (m_imageProperty) {
		m_savedMappedWindow = m_imageProperty->GetColorWindow();
		m_savedMappedLevel = m_imageProperty->GetColorLevel();
	}
	else {
		m_savedMappedWindow = std::numeric_limits<double>::quiet_NaN();
		m_savedMappedLevel = std::numeric_limits<double>::quiet_NaN();
	}

	m_hasSavedState = true;
}

void SliceView::restoreDerivedViewState()
{
	if (!m_hasSavedState) return;

	// Ensure mapper ranges are up-to-date to compute valid min/max slice indices
	if (m_sliceMapper) m_sliceMapper->Update();
	updateSliceRange();

	// Convert saved world point -> continuous index, then round to nearest slice index
	const int w = static_cast<int>(m_viewOrientation);
	double contIdx[3] = { 0.0, 0.0, 0.0 };
	m_imageData->TransformPhysicalPointToContinuousIndex(m_savedSliceWorld, contIdx);

	// Round nearest and clamp
	int restoredIndex = static_cast<int>(std::lround(contIdx[w]));
	restoredIndex = std::clamp(restoredIndex, m_minSlice, m_maxSlice);

	// Restore the slice index (this updates camera focal/position properly)
	setSliceIndex(restoredIndex);

	// Restore mapped WL to the image property and update interactor baseline
	if (m_imageProperty && std::isfinite(m_savedMappedWindow) && std::isfinite(m_savedMappedLevel)) {
		m_imageProperty->SetColorWindow(m_savedMappedWindow);
		m_imageProperty->SetColorLevel(m_savedMappedLevel);
		updateInteractorWindowLevelBaseline();
	}

	// Restore camera orientation/roll if we captured it.
	// Preserve the current focal point (set by setSliceIndex) but adopt the saved
	// direction-of-projection and view-up so rotation is preserved across geometry changes.
	if (m_savedCamera && m_renderer) {
		if (auto* cam = m_renderer->GetActiveCamera()) {
			double savedDOP[3]; m_savedCamera->GetDirectionOfProjection(savedDOP);
			double savedUp[3];  m_savedCamera->GetViewUp(savedUp);

			// Keep focal point that setSliceIndex established
			double curFpt[3]; cam->GetFocalPoint(curFpt);

			// Use saved distance if reasonable, otherwise keep current distance
			double dist = m_savedCamera->GetDistance();
			if (!(dist > 0.0)) dist = cam->GetDistance();

			// Position camera along -savedDOP so it looks at the current focal point
			cam->SetFocalPoint(curFpt);
			cam->SetPosition(curFpt[0] - savedDOP[0] * dist,
							 curFpt[1] - savedDOP[1] * dist,
							 curFpt[2] - savedDOP[2] * dist);
			cam->SetViewUp(savedUp);
			cam->OrthogonalizeViewUp();
			m_renderer->ResetCameraClippingRange();
		}
	}

	// Trigger a render to reflect restored state
	render();

	// Clear saved flag
	m_hasSavedState = false;
}

void SliceView::setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax)
{
	// Ensure we execute on GUI thread; queue if called from another thread.
	if (QThread::currentThread() != this->thread()) {
		QMetaObject::invokeMethod(this, "setCroppingRegion", Qt::QueuedConnection,
								  Q_ARG(int, xMin), Q_ARG(int, xMax), Q_ARG(int, yMin),
								  Q_ARG(int, yMax), Q_ARG(int, zMin), Q_ARG(int, zMax));
		return;
	}

	// Basic guards
	if (!m_sliceMapper || !m_imageData) return;

	// Normalize & clamp requested region to current image extents.
	auto clampNormalize = [](int& lo, int& hi, int loB, int hiB) {
		lo = std::clamp(lo, loB, hiB);
		hi = std::clamp(hi, loB, hiB);
		if (lo > hi) std::swap(lo, hi);
		if (lo == hi) {
			if (hi < hiB) ++hi;
			else if (lo > loB) --lo;
		}
		};
	clampNormalize(xMin, xMax, m_extent[0], m_extent[1]);
	clampNormalize(yMin, yMax, m_extent[2], m_extent[3]);
	clampNormalize(zMin, zMax, m_extent[4], m_extent[5]);

	// Store the requested region (non-destructive even when outline is hidden)
	m_requestedCroppingRegion[0] = xMin;
	m_requestedCroppingRegion[1] = xMax;
	m_requestedCroppingRegion[2] = yMin;
	m_requestedCroppingRegion[3] = yMax;
	m_requestedCroppingRegion[4] = zMin;
	m_requestedCroppingRegion[5] = zMax;
	m_requestedCroppingEnabled = true;

	// Apply immediately only if outline visible (otherwise cached for later)
	if (m_outlineVisible) {
		m_sliceMapper->SetCropping(true);
		m_sliceMapper->SetCroppingRegion(m_requestedCroppingRegion);
		m_sliceMapper->Update();

		if (m_outlineSource) {
			m_outlineSource->Modified();
			m_outlineSource->Update();
		}
		// Refresh display
		render();
	}
}

void SliceView::setOutlineVisible(bool visible)
{
	// Ensure we execute on GUI thread; queue if called from another thread.
	if (QThread::currentThread() != this->thread()) {
		QMetaObject::invokeMethod(this, "setOutlineVisible", Qt::QueuedConnection, Q_ARG(bool, visible));
		return;
	}

	if (!m_outlineActor || !m_sliceMapper) {
		// Still set the boolean so callers get consistent state even if actor/mapper missing.
		m_outlineVisible = visible;
		return;
	}

	// Toggle actor visibility first (fast)
	m_outlineActor->SetVisibility(visible);
	m_outlineVisible = visible;

	// When showing the outline, re-apply any previously requested cropping region.
	// When hiding, disable cropping but preserve the requested region in memory.
	if (visible) {
		if (m_requestedCroppingEnabled) {
			m_sliceMapper->SetCropping(true);
			m_sliceMapper->SetCroppingRegion(m_requestedCroppingRegion);
		}
		else {
			// No explicit region requested -> keep cropping disabled to avoid surprising clipping.
			m_sliceMapper->SetCropping(false);
		}
		// Update outline geometry now that cropping state is correct.
		if (m_outlineSource) {
			m_outlineSource->Modified();
			m_outlineSource->Update();
		}
	}
	else {
		// Hide outline: disable cropping but do NOT overwrite the cached requested region.
		m_sliceMapper->SetCropping(false);
	}

	// Ensure mapper updates and refresh display.
	m_sliceMapper->Update();
	render();
}

void SliceView::setOutlineColor(const QColor& color)
{
	if (!m_outlineActor) return;

	// Normalize QColor to vtk prop range [0..1]
	const double r = color.redF();
	const double g = color.greenF();
	const double b = color.blueF();

	vtkProperty* prop = m_outlineActor->GetProperty();
	if (prop) {
		prop->SetColor(r, g, b);
	}
	// update stored value and notify listeners if changed
	if (m_outlineColor != color) {
		m_outlineColor = color;
		emit outlineColorChanged(m_outlineColor);
	}
	// If visible, refresh rendering to show change immediately
	if (m_outlineVisible) render();
}

vtkImageSlicePointPlacer* SliceView::pointPlacer() const
{
	return m_pointPlacer;
}

void SliceView::setOrthoPlanes(vtkSmartPointer<vtkImageOrthoPlanes> planes)
{
	m_linkedOrthoPlanes = planes;
}
#include "SliceView.h"
#include "ui_SliceView.h"
#include "SunkenSliderStyle.h"
#include "MenuButton.h"

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

#include <vtkRenderWindow.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkInteractorStyleImage.h>
#include <vtkImageData.h>
#include <vtkCamera.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkInformation.h>
#include <vtkImageProperty.h>
#include <vtkEventQtSlotConnect.h>

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

	// Set interactor style after the widget created one for the renderWindow
	if (auto* iren = m_renderWindow->GetInteractor()) {
		interactorStyle = vtkSmartPointer<vtkInteractorStyleImage>::New();
		interactorStyle->SetInteractionModeToImage2D();
		interactorStyle->SetDefaultRenderer(m_renderer);
		interactorStyle->AutoAdjustCameraClippingRangeOn();
		iren->SetInteractorStyle(interactorStyle);
	}

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

	this->qvtkConnection = vtkSmartPointer<vtkEventQtSlotConnect>::New();
	this->qvtkConnection->Connect(interactorStyle, vtkCommand::LeftButtonPressEvent,
		this, SLOT(trapSpin(vtkObject*)));

	// Also make sure the style operates on your renderer
	interactorStyle->SetDefaultRenderer(m_renderer);
	interactorStyle->SetCurrentRenderer(m_renderer);

	//editor
	connect(ui->sliderSlicePosition, &QSlider::valueChanged, this, &SliceView::setSliceIndex);

	// Keep only the editor in sync when slice changes (remove "Slice:" label usage)
	connect(this, &SliceView::sliceChanged, this, [this](int value) {
		if (m_editSliceIndex) {
			const QSignalBlocker b(m_editSliceIndex);
			m_editSliceIndex->setText(QString::number(value));
		}
	});

	updateCamera();

	// Apply initial orientation via base API (keeps menu/actions synced)
	switch (initialOrientation) {
		case VIEW_ORIENTATION_YZ: setViewOrientationToYZ(); break;
		case VIEW_ORIENTATION_XZ: setViewOrientationToXZ(); break;
		case VIEW_ORIENTATION_XY:
		default: setViewOrientationToXY(); break;
	}
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
			imageProperty->SetInterpolationTypeToNearest();
			break;
			case Linear:
			imageProperty->SetInterpolationTypeToLinear();
			break;
			case Cubic:
			imageProperty->SetInterpolationTypeToCubic();
			break;
		}
		render();
		emit interpolationChanged(m_interpolation);
	}
}

void SliceView::buildSliderBar(QWidget* rootContent)
{
	// 1) Find the original parent layout and index BEFORE reparenting the slider
	QLayout* originalLayout = nullptr;
	int insertIndex = -1;

	if (auto* pw = ui->sliderSlicePosition->parentWidget())
		originalLayout = pw->layout();
	if (!originalLayout)
		originalLayout = rootContent->layout();

	if (originalLayout)
		insertIndex = originalLayout->indexOf(ui->sliderSlicePosition);

	// 2) Detach the slider from its original layout (the widget stays alive)
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

	// 3) Build the replacement bar: [minLabel] [slider] [maxLabel] [lineEdit]
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

	// Jump to typed slice when the user confirms
	connect(m_editSliceIndex, &QLineEdit::editingFinished, this, [this]() {
		bool ok = false;
		const int v = m_editSliceIndex->text().toInt(&ok);
		if (ok) setSliceIndex(v);
		else m_editSliceIndex->setText(QString::number(m_currentSlice));
	});
	connect(m_editSliceIndex, &QLineEdit::returnPressed, this, [this]() {
		bool ok = false;
		const int v = m_editSliceIndex->text().toInt(&ok);
		if (ok) setSliceIndex(v);
	});

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
	if (imageSlice) {
		imageSlice->SetOrientation(0.0, 0.0, 0.0);
		imageSlice->SetScale(1.0, 1.0, 1.0);
		imageSlice->SetUserTransform(nullptr);
		imageSlice->SetUserMatrix(nullptr);
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

void SliceView::setImageData(vtkImageData* image) {
	if (!image) return;

	m_imageData = image;

	// Compute mapping and connect the shared filter
	computeShiftScaleFromInput(image);

	// Set the input image to the shift/scale filter
	shiftScaleFilter->SetInputData(m_imageData);
	shiftScaleFilter->Update();

	// feed the slice mapper directly from the 16-bit shift/scale output
	sliceMapper->SetInputConnection(shiftScaleFilter->GetOutputPort());

	// Ensure mapper orientation matches current view as soon as input exists
	switch (m_viewOrientation) {
		case VIEW_ORIENTATION_YZ: sliceMapper->SetOrientationToX(); break; // z-y plane x normal
		case VIEW_ORIENTATION_XZ: sliceMapper->SetOrientationToY(); break; // x-z plane y normal
		case VIEW_ORIENTATION_XY:
		default:                  sliceMapper->SetOrientationToZ(); break; // x-y plane z normal
	}

	// Add imageActor to renderer only the first time a valid image is set
	if (!m_imageInitialized) {
		m_renderer->AddViewProp(imageSlice);
		imageSlice->PickableOn();      // ensure VTK can pick it for WL
		m_imageInitialized = true;
	}

	const double diff = (m_scalarRangeMax + m_scalarShift) * m_scalarScale
		- (m_scalarRangeMin + m_scalarShift) * m_scalarScale;

	const double baseWindow = diff > 0.0 ? diff : 1.0;
	const double baseLevel = baseWindow * 0.5;
	imageProperty->SetColorWindow(baseWindow);
	imageProperty->SetColorLevel(baseLevel);

	updateSliceRange();

	// Set camera and show a valid slice immediately (center)
	updateCamera();
	setSliceIndex((m_minSlice + m_maxSlice) / 2);

	// Prime the interactor style so 'r' resets to this baseline WL.
	if (auto* iren = m_renderWindow->GetInteractor()) {
		if (auto* style = vtkInteractorStyleImage::SafeDownCast(iren->GetInteractorStyle())) {
			// Ensure the style can find the image and property
			style->SetDefaultRenderer(m_renderer);
			style->SetCurrentRenderer(m_renderer);
			// This captures imageProperty->GetColorWindow/Level() into WindowLevelInitial
			style->StartWindowLevel();
			style->EndWindowLevel();
		}
	}
}

void SliceView::updateCamera() {
	if (!m_imageData)	return;

	const double* origin = m_imageData->GetOrigin();
	const double* spacing = m_imageData->GetSpacing();
	int extent[6];
	m_imageData->GetExtent(extent);

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
	m_currentSlice = static_cast<int>(extent[2 * w]);
}

void SliceView::setViewOrientation(ImageFrameWidget::ViewOrientation orientation)
{
	if (m_viewOrientation == orientation)
		return;

	m_viewOrientation = orientation;

	// If no image/pipeline yet, just broadcast and return (avoid VTK errors).
	if (!m_imageData || !sliceMapper || sliceMapper->GetNumberOfInputConnections(0) == 0) {
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

	sliceMapper->SetSliceNumber(m_currentSlice);
	sliceMapper->Update();

	int u = 0, v = 1, w = m_viewOrientation;
	switch (w)
	{
		case 0: u = 1; v = 2; break;
		case 1: u = 0; v = 2; break;
		case 2: u = 0; v = 1; break;
	}

	const double* origin = m_imageData->GetOrigin();
	const double* spacing = m_imageData->GetSpacing();

	auto cam = m_renderer->GetActiveCamera();
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
	if (watched == ui->renderArea && event->type() == QEvent::ShortcutOverride) {
		// Only capture keys for VTK when this frame is selected, unless restriction is disabled
		if (!restrictInteractionToSelection() || isSelected()) {
			auto* ke = reinterpret_cast<QKeyEvent*>(event);

			// Accept when no modifiers or Shift only; let Ctrl/Alt/Meta fall through to app shortcuts
			const Qt::KeyboardModifiers mods = ke->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
			if (mods == Qt::NoModifier || mods == Qt::ShiftModifier) {
				switch (ke->key()) {
					case Qt::Key_R: // Reset window/level in vtkInteractorStyleImage::OnChar
					case Qt::Key_F: // Fly-to
					case Qt::Key_X: // Set orientation
					case Qt::Key_Y:
					case Qt::Key_Z:
					ke->accept();
					return true; // stop shortcut processing; deliver to KeyPress/VTK
					default:
					break;
				}
			}
		}
	}
	return SelectionFrameWidget::eventFilter(watched, event);
}
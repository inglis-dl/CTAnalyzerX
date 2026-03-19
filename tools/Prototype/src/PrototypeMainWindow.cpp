#include "PrototypeMainWindow.h"
#include "PrototypeHelpers.h"
#include "ui_MainWindow.h"

#include "VolumeView.h"
#include "ImageLoader.h"
#include "JsonUtils.h"

#include <vtkBoundingBox.h>
#include <vtkCamera.h>
#include <vtkColorTransferFunction.h>
#include <vtkDataArray.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkExtractVOI.h>
#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPointData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkScalarBarActor.h>
#include <vtkSmartPointer.h>

#include <QAction>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PrototypeMainWindow
// ---------------------------------------------------------------------------

PrototypeMainWindow::PrototypeMainWindow(QWidget* parent)
	: QMainWindow(parent)
	, ui(new Ui::MainWindow)
{
	ui->setupUi(this);

	// Progress bar (permanent widget on status bar, hidden until loading starts)
	m_progressBar = new QProgressBar(this);
	m_progressBar->setRange(0, 100);
	m_progressBar->setValue(0);
	m_progressBar->setVisible(false);
	statusBar()->addPermanentWidget(m_progressBar);

	// ------------------------------------------------------------------
	// Toolbar button order: Reslice | Landmark | Regions | Regions Alt | Clean | Outline | Restart
	// ------------------------------------------------------------------

	// "Reslice" toolbar button: reslice the volume aligned to the PCA axes
	m_actReslice = new QAction(tr("Reslice"), this);
	m_actReslice->setToolTip(tr("Reslice the volume aligned to the PCA principal axes"));
	ui->toolBar->addAction(m_actReslice);
	connect(m_actReslice, &QAction::triggered, this, &PrototypeMainWindow::onReslice);

	// "Landmark" toolbar button: searches along each PCA axis for surface transitions
	m_actLandmark = new QAction(tr("Landmark"), this);
	m_actLandmark->setToolTip(tr("Find surface landmark points along the PCA axes"));
	ui->toolBar->addAction(m_actLandmark);
	connect(m_actLandmark, &QAction::triggered, this, &PrototypeMainWindow::onLandmark);

	// "Regions" toolbar button: threshold + seeded BFS island segmentation
	m_actRegions = new QAction(tr("Regions"), this);
	m_actRegions->setToolTip(tr("Segment bone islands from the resliced volume using landmark seeds"));
	ui->toolBar->addAction(m_actRegions);
	connect(m_actRegions, &QAction::triggered, this, &PrototypeMainWindow::onRegions);

	// "Regions Alt" toolbar button: morphological pipeline (smooth ? erode ? dilate ? connectivity)
	m_actRegionsAlt = new QAction(tr("Regions Alt"), this);
	m_actRegionsAlt->setToolTip(tr("Segment bone islands using the morphological pipeline "
		"(Gaussian smooth ? erode ? dilate ? seeded connectivity)"));
	ui->toolBar->addAction(m_actRegionsAlt);
	connect(m_actRegionsAlt, &QAction::triggered, this, &PrototypeMainWindow::onRegionsAlt);

	// "Clean" toolbar button: post-segmentation clean step.
	// Enabled only after segmentation completes AND at least 8 Reslice operations
	// have been performed in the current session (m_resliceCount >= 8).
	m_actClean = new QAction(tr("Clean"), this);
	m_actClean->setToolTip(tr("Run the post-segmentation clean step "
		"(available after 8 or more Reslice operations)"));
	ui->toolBar->addAction(m_actClean);
	connect(m_actClean, &QAction::triggered, this, &PrototypeMainWindow::onClean);

	// "Outline" toggle toolbar button: shows/hides the VolumeView bounding-box outline actor.
	// The action is checkable so it renders in a depressed state while the outline is visible
	// and returns to the normal raised state when unchecked.
	m_actOutline = new QAction(tr("Outline"), this);
	m_actOutline->setToolTip(tr("Toggle the volume bounding-box outline"));
	m_actOutline->setCheckable(true);
	m_actOutline->setChecked(false); // outline is hidden by default (matches VolumeView default)
	ui->toolBar->addAction(m_actOutline);
	connect(m_actOutline, &QAction::toggled, this, &PrototypeMainWindow::onOutlineToggled);

	// "Restart" toolbar button: revert to the original image and reset the workflow.
	// Always enabled — Restart can be applied at any workflow step.
	m_actRestart = new QAction(tr("Restart"), this);
	m_actRestart->setToolTip(tr("Revert to the original image and reset the workflow to the start"));
	ui->toolBar->addAction(m_actRestart);
	connect(m_actRestart, &QAction::triggered, this, &PrototypeMainWindow::onRestart);

	// Apply the initial workflow step state: only Reslice enabled at startup.
	// Restart is always enabled regardless of step; set it explicitly here.
	m_actRestart->setEnabled(true);
	setWorkflowStep(WorkflowStep::Idle);

	// ImageLoader + VTK event wiring
	m_imageLoader = vtkSmartPointer<ImageLoader>::New();
	m_vtkConnections = vtkSmartPointer<vtkEventQtSlotConnect>::New();

	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::StartEvent,
		this, SLOT(onVtkStartEvent()));

	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::EndEvent,
		this, SLOT(onVtkEndEvent()));

	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::ProgressEvent,
		this, SLOT(onVtkProgressEvent()));
}

PrototypeMainWindow::~PrototypeMainWindow()
{
	delete ui;
}

// ---------------------------------------------------------------------------
// Window close — flush the JSON cache to the prototype sidecar
// ---------------------------------------------------------------------------

void PrototypeMainWindow::closeEvent(QCloseEvent* event)
{
	writePrototypeSidecar();
	QMainWindow::closeEvent(event);
}

// ---------------------------------------------------------------------------
// Workflow step state machine
// ---------------------------------------------------------------------------

void PrototypeMainWindow::setWorkflowStep(WorkflowStep step)
{
	m_workflowStep = step;

	// Determine enabled state for each step button based on the current step.
	// Restart is always enabled and managed independently.
	//
	// Workflow routes:
	//   A) Idle ? Resliced ? Landmarked ? Segmented  (via Regions)
	//   B) Idle ? Resliced ? Landmarked ? Segmented  (via Regions Alt)
	//
	// At each step exactly one (or two, at Landmarked) button is enabled:
	//   Idle       : Reslice=on,  Landmark=off, Regions=off, RegionsAlt=off
	//   Resliced   : Reslice=off, Landmark=on,  Regions=off, RegionsAlt=off
	//   Landmarked : Reslice=off, Landmark=off, Regions=on,  RegionsAlt=on
	//   Segmented  : Reslice=off, Landmark=off, Regions=off, RegionsAlt=off

	const bool atIdle = (step == WorkflowStep::Idle);
	const bool atResliced = (step == WorkflowStep::Resliced);
	const bool atLandmarked = (step == WorkflowStep::Landmarked);

	m_actReslice->setEnabled(atIdle);
	m_actLandmark->setEnabled(atResliced);
	m_actRegions->setEnabled(atLandmarked);
	m_actRegionsAlt->setEnabled(atLandmarked);

	// Restart is always enabled — it can be applied at any time.
	m_actRestart->setEnabled(true);
}

// ---------------------------------------------------------------------------
// Progress slots
// ---------------------------------------------------------------------------

void PrototypeMainWindow::showProgressStart()
{
	m_progressBar->setValue(0);
	m_progressBar->setVisible(true);
	m_progressBar->setEnabled(true);
}

void PrototypeMainWindow::showProgressValue(int percent)
{
	m_progressBar->setValue(percent);
	m_progressBar->setVisible(true);
}

void PrototypeMainWindow::showProgressEnd()
{
	m_progressBar->setValue(100);
	m_progressBar->setVisible(false);
}

void PrototypeMainWindow::onVtkStartEvent()
{
	if (QThread::currentThread() == this->thread())
	{
		showProgressStart();
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else
	{
		QMetaObject::invokeMethod(this, "showProgressStart", Qt::QueuedConnection);
	}
}

void PrototypeMainWindow::onVtkEndEvent()
{
	if (QThread::currentThread() == this->thread())
	{
		showProgressEnd();
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else
	{
		QMetaObject::invokeMethod(this, "showProgressEnd", Qt::QueuedConnection);
	}
}

void PrototypeMainWindow::onVtkProgressEvent()
{
	if (!m_imageLoader) return;

	const int value = static_cast<int>(
		std::clamp(m_imageLoader->GetProgress(), 0.0, 1.0) * 100.0);

	if (QThread::currentThread() == this->thread())
	{
		showProgressValue(value);
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else
	{
		QMetaObject::invokeMethod(this, "showProgressValue",
			Qt::QueuedConnection, Q_ARG(int, value));
	}
}

// ---------------------------------------------------------------------------
// PCA overlay management
// ---------------------------------------------------------------------------

void PrototypeMainWindow::clearPcaOverlay()
{
	if (!ui || !ui->volumeView)
		return;

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	for (auto& a : m_axisActors)
	{
		if (a) { ren->RemoveActor(a); a = nullptr; }
	}
	for (auto& a : m_tipActors)
	{
		if (a) { ren->RemoveActor(a); a = nullptr; }
	}
	for (auto& a : m_ringActors)
	{
		if (a) { ren->RemoveActor(a); a = nullptr; }
	}
}

// ---------------------------------------------------------------------------
// Island actor management
// ---------------------------------------------------------------------------

void PrototypeMainWindow::clearIslandActors()
{
	if (!ui || !ui->volumeView)
		return;

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	// Remove surface actors
	for (auto& a : m_islandActors)
	{
		if (a) ren->RemoveActor(a);
	}
	m_islandActors.clear();

	// Remove the scalar bar when it exists
	if (m_islandScalarBar)
	{
		ren->RemoveActor(m_islandScalarBar);
		m_islandScalarBar = nullptr;
	}
}

// ---------------------------------------------------------------------------
// PCA JSON serialisation helper
// ---------------------------------------------------------------------------

// static
QJsonObject PrototypeMainWindow::pcaResultToJson(const PrototypeHelpers::PcaResult& pca)
{
	auto packVec3 = [](const double v[3]) -> QJsonArray
		{
			return QJsonArray{ v[0], v[1], v[2] };
		};

	// Axes: array of 3 objects, one per principal axis
	QJsonArray axesArray;
	for (int i = 0; i < 3; ++i)
	{
		QJsonObject axisObj;
		axisObj[QStringLiteral("index")] = i;
		axisObj[QStringLiteral("eigenvalue")] = pca.eigenvalues[i];
		axisObj[QStringLiteral("direction")] = packVec3(pca.axes[i]);
		axesArray.append(axisObj);
	}

	QJsonObject obj;
	obj[QStringLiteral("centroid")] = packVec3(pca.centroid);
	obj[QStringLiteral("circumRadius")] = pca.circumRadius;
	obj[QStringLiteral("axes")] = axesArray;
	return obj;
}

// ---------------------------------------------------------------------------
// Prototype sidecar output path + write
// ---------------------------------------------------------------------------

QString PrototypeMainWindow::prototypeOutputPath() const
{
	if (m_sidecarPath.isEmpty() || m_cropPath.isEmpty())
		return {};

	// Derive the output filename from the crop image basename:
	//   <crop_basename>_prototype.json
	const QString cropBaseName = QFileInfo(m_cropPath).completeBaseName();
	const QString outputName = cropBaseName + QStringLiteral("_prototype.json");

	// Place the file in the same directory as the source sidecar.
	const QString sidecarDir = QFileInfo(m_sidecarPath).absolutePath();
	return QDir(sidecarDir).filePath(outputName);
}

bool PrototypeMainWindow::writePrototypeSidecar() const
{
	// Nothing to write if no landmark run has completed yet.
	if (m_landmarkJson.isEmpty())
	{
		qDebug("writePrototypeSidecar: no landmark data to write; skipping.");
		return true;
	}

	const QString outputPath = prototypeOutputPath();
	if (outputPath.isEmpty())
	{
		qWarning("writePrototypeSidecar: cannot determine output path "
				 "(sidecar or crop path not set); skipping.");
		return false;
	}

	QFile f(outputPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		qWarning("writePrototypeSidecar: failed to open '%s' for writing: %s",
				 qUtf8Printable(outputPath),
				 qUtf8Printable(f.errorString()));
		return false;
	}

	const QByteArray json = QJsonDocument(m_landmarkJson).toJson(QJsonDocument::Indented);
	f.write(json);
	f.close();

	qDebug("writePrototypeSidecar: wrote %lld bytes to '%s'",
		   static_cast<long long>(json.size()),
		   qUtf8Printable(outputPath));
	return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void PrototypeMainWindow::loadFromSidecar(const QString& sidecarPath)
{
	const QJsonObject sidecar = PrototypeHelpers::readJsonObjectFileOrThrow(sidecarPath);

	const QString cropPath = PrototypeHelpers::cropPathFromSidecarOrThrow(sidecar);
	qDebug("Project:   %s", qUtf8Printable(sidecarPath));
	qDebug("Crop path: %s", qUtf8Printable(cropPath));

	m_threshold = PrototypeHelpers::thresholdFromSidecar(sidecar);

	if (!QFileInfo::exists(cropPath))
	{
		throw std::runtime_error(
			("Error: crop output does not exist:\n" + cropPath).toStdString());
	}

	// Optional: exercise canonical sidecar mapping logic.
	const QJsonObject canonical = JsonUtils::readJsonSidecar(cropPath);
	if (!canonical.isEmpty())
		qDebug("JsonUtils canonical sidecar found for crop path.");

	if (!ImageLoader::CanReadFile(cropPath))
		throw std::runtime_error(("Unsupported or unreadable file: " + cropPath).toStdString());

	// Cache the paths so closeEvent() can derive the prototype output filename.
	m_sidecarPath = QFileInfo(sidecarPath).absoluteFilePath();
	m_cropPath = cropPath;

	m_imageLoader->SetInputPath(cropPath);
	m_imageLoader->SetImageType(ImageLoader::ImageType::NIFTI);
	m_imageLoader->Update();

	vtkSmartPointer<vtkImageData> out = m_imageLoader->GetOutput();
	if (!out)
		throw std::runtime_error(("ImageLoader returned null output for: " + cropPath).toStdString());

	const int* dims = out->GetDimensions();
	if (dims[0] <= 1 || dims[1] <= 1 || dims[2] <= 1)
		throw std::runtime_error(("ImageLoader produced invalid volume dimensions for: " + cropPath).toStdString());

	// Keep a deep copy of the original image so Restart can restore it without
	// re-reading from disk.
	m_originalImage = vtkSmartPointer<vtkImageData>::New();
	m_originalImage->DeepCopy(out);

	setImage(out);

	ui->volumeView->renderer()->ResetCamera();
	ui->volumeView->renderer()->ResetCameraClippingRange();
	ui->volumeView->renderWindow()->Render();

	// A freshly loaded image starts the workflow at Idle (only Reslice enabled).
	setWorkflowStep(WorkflowStep::Idle);
}

void PrototypeMainWindow::setImage(vtkImageData* image)
{
	if (!ui || !ui->volumeView)
		return;

	// Remove any PCA overlay from a previous image.
	clearPcaOverlay();

	// Cache raw pointer for use by onLandmark() (lifetime owned by m_imageLoader pipeline).
	m_image = image;

	// Invalidate any previously cached PCA result and landmark data.
	m_pca.valid = false;
	m_landmarkResult = QJsonObject{};

	ui->volumeView->setImageData(image);
	ui->volumeView->updateData();

	// The outline actor is hidden by VolumeView on every updateData() call;
	// keep the toolbar toggle button in sync with that reset.
	if (m_actOutline)
	{
		// Block the toggled() signal while we programmatically reset the checked
		// state so onOutlineToggled() is not re-entered for this housekeeping change.
		const QSignalBlocker blocker(m_actOutline);
		m_actOutline->setChecked(false);
	}

	// Determine window/level from the image and the cached sidecar threshold.
	// level  = threshold (falls back to scalar range midpoint if not present)
	// window = 2 × overall standard deviation (from computeScalarThresholdStats)
	//
	// computeScalarThresholdStats() is called unconditionally here so that
	// m_imageStats is always populated for downstream steps (e.g. Clean),
	// regardless of whether a finite threshold is available.  When no threshold
	// is present the foreground/background partition keys will reflect a
	// degenerate split (all voxels treated as background), which is acceptable
	// because the Clean step is gated on a finite threshold being available.
	if (!image)
		return;


	// Compute threshold-partitioned statistics once and cache for all consumers.
	// Pass the threshold only when it is finite; computeScalarThresholdStats
	// handles a NaN threshold gracefully (all voxels fall into background).
	const double effectiveThreshold = std::isfinite(m_threshold)
		? m_threshold
		: std::numeric_limits<double>::quiet_NaN();

	m_imageStats = PrototypeHelpers::computeScalarThresholdStats(image, effectiveThreshold);

	qDebug("setImage: imageStats — mean=%.4f  stdDev=%.4f  "
		   "meanFg=%.4f  stdDevFg=%.4f  meanBg=%.4f  stdDevBg=%.4f",
		   m_imageStats.value(QStringLiteral("mean")).toDouble(),
		   m_imageStats.value(QStringLiteral("stdDev")).toDouble(),
		   m_imageStats.value(QStringLiteral("meanFg")).toDouble(),
		   m_imageStats.value(QStringLiteral("stdDevFg")).toDouble(),
		   m_imageStats.value(QStringLiteral("meanBg")).toDouble(),
		   m_imageStats.value(QStringLiteral("stdDevBg")).toDouble());

	const double scalarRange[2] = {
		m_imageStats.value(QStringLiteral("min")).toDouble(0.0),
		m_imageStats.value(QStringLiteral("max")).toDouble(255.0) };

	// level: threshold when finite, otherwise midpoint of the scalar range.
	// window: 2 × whole-volume standard deviation sourced from m_imageStats.
	const double level = std::isfinite(m_threshold)
		? m_threshold
		: 0.5 * (scalarRange[0] + scalarRange[1]);

	const double window = 2.0 * m_imageStats.value(QStringLiteral("stdDev")).toDouble(1.0);

	ui->volumeView->setColorWindowLevel(window, level);


	// ------------------------------------------------------------------
	// PCA overlay: only when a finite threshold is available
	// ------------------------------------------------------------------
	if (!std::isfinite(m_threshold))
	{
		qDebug("setImage: no finite threshold - PCA overlay skipped.");
		return;
	}

	// Progress callback: updates the status-bar progress bar and pumps
	// paint events so the bar repaints between the two expensive passes.
	// The PCA occupies the [0..100] range independently of the VTK load.
	showProgressStart();
	const auto pcaProgress = [this](int percent)
		{
			showProgressValue(percent);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		};

	const bool ok = PrototypeHelpers::computePca(image, m_threshold, m_pca, pcaProgress);
	showProgressEnd();

	if (!ok)
		return;

	// ------------------------------------------------------------------
	// Cache the PCA result to JSON.
	//
	// m_originalPcaJson is written ONLY on the first (original) load, i.e.
	// when no resliced image exists yet.  This ensures callers can always
	// retrieve the pre-reslice PCA as the definitive starting point even
	// after multiple reslice passes have been performed.
	//
	// m_reslicedPcaJson is written by onReslice() after this function
	// returns, so we do not touch it here.
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		m_originalPcaJson = pcaResultToJson(m_pca);
		qDebug("setImage: original PCA JSON cached:\n%s",
			   qUtf8Printable(QJsonDocument(m_originalPcaJson).toJson(QJsonDocument::Indented)));
	}

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	// Sphere glyph radius = 8 % of the circumsphere radius (4× the original 2 % size)
	const double glyphR = 0.08 * m_pca.circumRadius;

	// Axis colours: R=axis0 (largest variance), G=axis1, B=axis2
	const double axisColors[3][3] = {
		{ 1.0, 0.2, 0.2 },  // axis 0 - red
		{ 0.2, 1.0, 0.2 },  // axis 1 - green
		{ 0.2, 0.2, 1.0 },  // axis 2 - blue
	};

	for (int i = 0; i < 3; ++i)
	{
		const double* col = axisColors[i];
		const double  R = m_pca.circumRadius;

		// Tip points along +axis and -axis
		double tipPos[3], tipNeg[3];
		for (int d = 0; d < 3; ++d)
		{
			tipPos[d] = m_pca.centroid[d] + R * m_pca.axes[i][d];
			tipNeg[d] = m_pca.centroid[d] - R * m_pca.axes[i][d];
		}

		// Shaft from -tip to +tip
		m_axisActors[i] = PrototypeHelpers::makeLineActor(tipNeg, tipPos, col[0], col[1], col[2], 2.5);
		ren->AddActor(m_axisActors[i]);

		// Sphere glyphs at both ends (4× the original 2 % size)
		m_tipActors[static_cast<std::size_t>(i * 2)] = PrototypeHelpers::makeSphereActor(tipPos, glyphR, col[0], col[1], col[2]);
		m_tipActors[static_cast<std::size_t>(i * 2 + 1)] = PrototypeHelpers::makeSphereActor(tipNeg, glyphR, col[0], col[1], col[2]);
		ren->AddActor(m_tipActors[static_cast<std::size_t>(i * 2)]);
		ren->AddActor(m_tipActors[static_cast<std::size_t>(i * 2 + 1)]);

		// Ring i:
		//   - centre  : PCA centroid (all three rings share the same centre)
		//   - normal  : axes[i]  (the eigen direction for this axis)
		//   - radius  : circumsphere radius R
		// The ring lies in the plane perpendicular to axes[i] passing through
		// the centroid, so each ring slices through the centre of the point cloud.
		m_ringActors[i] = PrototypeHelpers::makeRingActor(m_pca.centroid, m_pca.axes[i], R,
														   col[0], col[1], col[2], 2.0);
		ren->AddActor(m_ringActors[i]);
	}

	ui->volumeView->render();
}

// ---------------------------------------------------------------------------
// Landmark slot
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onLandmark()
{
	if (!m_pca.valid)
	{
		qWarning("onLandmark: no valid PCA result available; load an image first.");
		return;
	}

	if (!m_image)
	{
		qWarning("onLandmark: no image cached.");
		return;
	}

	if (!std::isfinite(m_threshold))
	{
		qWarning("onLandmark: threshold is not finite; cannot search for surface.");
		return;
	}

	vtkRenderer* ren = (ui && ui->volumeView) ? ui->volumeView->renderer() : nullptr;

	// Sphere glyph radius reused from setImage() (8 % of circumsphere radius)
	const double glyphR = 0.08 * m_pca.circumRadius;

	// Axis colours matching those in setImage()
	const double axisColors[3][3] = {
		{ 1.0, 0.2, 0.2 },  // axis 0 - red
		{ 0.2, 1.0, 0.2 },  // axis 1 - green
		{ 0.2, 0.2, 1.0 },  // axis 2 - blue
	};

	// JSON arrays to accumulate per-axis landmark data
	QJsonArray jsonLandmarks;

	for (int i = 0; i < 3; ++i)
	{
		const double* col = axisColors[i];

		// Each eigen axis has two directions (+/-) relative to the centroid.
		// For each direction the search ray is cast from the centroid outward,
		// intersected with the image bounding box, and then walked INWARD from
		// that bounding-box entry point toward the centroid.  The first voxel
		// whose scalar value crosses from below-threshold to above-threshold is
		// the surface landmark point.
		//
		// axisDir (+): centroid ? +axis  (outward direction for the + half)
		// axisDir (-): centroid ? -axis  (outward direction for the - half)
		const double axisDirPos[3] = { m_pca.axes[i][0],  m_pca.axes[i][1],  m_pca.axes[i][2] };
		const double axisDirNeg[3] = { -m_pca.axes[i][0], -m_pca.axes[i][1], -m_pca.axes[i][2] };

		PrototypeHelpers::findSurfacePointFromBoundary(
			m_image, m_pca.centroid, axisDirPos, m_threshold,
			m_landmarkPoints[static_cast<std::size_t>(i)][0].data());
		PrototypeHelpers::findSurfacePointFromBoundary(
			m_image, m_pca.centroid, axisDirNeg, m_threshold,
			m_landmarkPoints[static_cast<std::size_t>(i)][1].data());

		const double* lPos = m_landmarkPoints[static_cast<std::size_t>(i)][0].data();
		const double* lNeg = m_landmarkPoints[static_cast<std::size_t>(i)][1].data();

		qDebug("Landmark axis %d  +: (%.2f, %.2f, %.2f)  -: (%.2f, %.2f, %.2f)",
			   i,
			   lPos[0], lPos[1], lPos[2],
			   lNeg[0], lNeg[1], lNeg[2]);

		// Relocate the existing tip sphere actors to the new surface positions.
		// The actors are already in the renderer from setImage(); we replace them
		// in-place rather than removing and re-adding to avoid flicker.
		if (ren)
		{
			const std::size_t posIdx = static_cast<std::size_t>(i * 2);
			const std::size_t negIdx = static_cast<std::size_t>(i * 2 + 1);

			if (m_tipActors[posIdx]) ren->RemoveActor(m_tipActors[posIdx]);
			if (m_tipActors[negIdx]) ren->RemoveActor(m_tipActors[negIdx]);

			m_tipActors[posIdx] = PrototypeHelpers::makeSphereActor(lPos, glyphR, col[0], col[1], col[2]);
			m_tipActors[negIdx] = PrototypeHelpers::makeSphereActor(lNeg, glyphR, col[0], col[1], col[2]);

			ren->AddActor(m_tipActors[posIdx]);
			ren->AddActor(m_tipActors[negIdx]);
		}

		// Accumulate JSON for this axis
		auto packVec3 = [](const double v[3]) -> QJsonArray
			{
				return QJsonArray{ v[0], v[1], v[2] };
			};

		QJsonObject axisObj;
		axisObj[QStringLiteral("index")] = i;
		axisObj[QStringLiteral("eigenvalue")] = m_pca.eigenvalues[i];
		axisObj[QStringLiteral("eigenvector")] = packVec3(m_pca.axes[i]);
		axisObj[QStringLiteral("landmarkPos")] = packVec3(lPos);
		axisObj[QStringLiteral("landmarkNeg")] = packVec3(lNeg);
		jsonLandmarks.append(axisObj);
	}

	// ------------------------------------------------------------------
	// Build and cache the per-axis raw landmark result (existing behaviour)
	// ------------------------------------------------------------------
	auto packVec3 = [](const double v[3]) -> QJsonArray
		{
			return QJsonArray{ v[0], v[1], v[2] };
		};

	m_landmarkResult = QJsonObject{};
	m_landmarkResult[QStringLiteral("centroid")] = packVec3(m_pca.centroid);
	m_landmarkResult[QStringLiteral("circumRadius")] = m_pca.circumRadius;
	m_landmarkResult[QStringLiteral("threshold")] = m_threshold;
	m_landmarkResult[QStringLiteral("axes")] = jsonLandmarks;

	// ------------------------------------------------------------------
	// Build the consolidated landmark JSON cache (written to disk on close).
	// ------------------------------------------------------------------
	m_landmarkJson = QJsonObject{};
	m_landmarkJson[QStringLiteral("sourceSidecar")] = m_sidecarPath;
	m_landmarkJson[QStringLiteral("cropImage")] = m_cropPath;
	m_landmarkJson[QStringLiteral("threshold")] = m_threshold;

	if (!m_originalPcaJson.isEmpty())
		m_landmarkJson[QStringLiteral("originalPca")] = m_originalPcaJson;

	if (!m_reslicedPcaJson.isEmpty())
		m_landmarkJson[QStringLiteral("reslicedPca")] = m_reslicedPcaJson;

	m_landmarkJson[QStringLiteral("landmarks")] = m_landmarkResult;

	qDebug("onLandmark: landmark JSON cached:\n%s",
		   qUtf8Printable(QJsonDocument(m_landmarkJson).toJson(QJsonDocument::Indented)));

	if (ren)
		ui->volumeView->render();

	// Landmark completed — advance to Landmarked: Regions and Regions Alt enabled.
	setWorkflowStep(WorkflowStep::Landmarked);
}

// ---------------------------------------------------------------------------
// Reslice slot
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onReslice()
{
	if (!m_pca.valid)
	{
		qWarning("onReslice: no valid PCA result available; load an image first.");
		return;
	}

	if (!m_image)
	{
		qWarning("onReslice: no image cached.");
		return;
	}

	auto resliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
	resliceAxes->Identity();

	for (int row = 0; row < 3; ++row)
	{
		resliceAxes->SetElement(row, 0, m_pca.axes[0][row]);
		resliceAxes->SetElement(row, 1, m_pca.axes[1][row]);
		resliceAxes->SetElement(row, 2, m_pca.axes[2][row]);
		resliceAxes->SetElement(row, 3, m_pca.centroid[row]);
	}

	qDebug("onReslice: reslice axes matrix:");
	for (int r = 0; r < 4; ++r)
	{
		qDebug("  [ %8.4f  %8.4f  %8.4f  %8.4f ]",
			   resliceAxes->GetElement(r, 0),
			   resliceAxes->GetElement(r, 1),
			   resliceAxes->GetElement(r, 2),
			   resliceAxes->GetElement(r, 3));
	}

	showProgressStart();
	showProgressValue(10);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	const double bgMean = m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);

	auto reslice = vtkSmartPointer<vtkImageReslice>::New();
	reslice->SetInputData(m_image);
	reslice->SetResliceAxes(resliceAxes);
	reslice->SetInterpolationModeToCubic();
	reslice->AutoCropOutputOn();
	reslice->SetOutputDimensionality(3);
	reslice->SetBackgroundLevel(bgMean);
	reslice->Update();

	showProgressValue(90);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	vtkImageData* resliced = reslice->GetOutput();
	if (!resliced)
	{
		qWarning("onReslice: vtkImageReslice produced null output.");
		showProgressEnd();
		return;
	}

	const int* outDims = resliced->GetDimensions();
	qDebug("onReslice: output dimensions: %d x %d x %d", outDims[0], outDims[1], outDims[2]);

	m_reslicedImage = vtkSmartPointer<vtkImageData>::New();
	m_reslicedImage->DeepCopy(resliced);

	showProgressEnd();

	setImage(m_reslicedImage);

	if (m_pca.valid)
	{
		m_reslicedPcaJson = pcaResultToJson(m_pca);
		qDebug("onReslice: resliced PCA JSON cached:\n%s",
			   qUtf8Printable(QJsonDocument(m_reslicedPcaJson).toJson(QJsonDocument::Indented)));
	}

	// Increment the cumulative reslice counter for this session.
	// The Clean button gate (m_resliceCount >= 8) is re-evaluated by
	// setWorkflowStep() every time a new step is entered.
	++m_resliceCount;
	qDebug("onReslice: m_resliceCount=%d", m_resliceCount);

	// Reslice completed — advance to Resliced: only Landmark enabled.
	setWorkflowStep(WorkflowStep::Resliced);
}

// ---------------------------------------------------------------------------
// Shared helper — build island actors from a completed segmentation result
// and merge the regions summary into the landmark JSON cache.
// Called by both onRegions() and onRegionsAlt() after segmentation completes.
// ---------------------------------------------------------------------------

void PrototypeMainWindow::applyIslandSegmentationResult(
	const std::vector<PrototypeHelpers::BoneIsland>& islands,
	vtkSmartPointer<vtkImageData>                    labelImage)
{
	// Cache the label image so it stays alive for the actors' pipeline
	m_labelImage = labelImage;

	// Remove actors from any previous regions run (including scalar bar)
	clearIslandActors();

	vtkRenderer* ren = (ui && ui->volumeView) ? ui->volumeView->renderer() : nullptr;

	const int nIslands = static_cast<int>(islands.size());

	// ------------------------------------------------------------------
	// Determine the voxel-count range across all islands for colour mapping
	// ------------------------------------------------------------------
	vtkIdType minVoxels = islands[0].voxelCount;
	vtkIdType maxVoxels = islands[0].voxelCount;

	for (const auto& isl : islands)
	{
		minVoxels = std::min(minVoxels, isl.voxelCount);
		maxVoxels = std::max(maxVoxels, isl.voxelCount);
	}

	qDebug("applyIslandSegmentationResult: voxel-count range  min=%lld  max=%lld",
		   static_cast<long long>(minVoxels),
		   static_cast<long long>(maxVoxels));

	// ------------------------------------------------------------------
	// Build the colour transfer function over [minVoxels, maxVoxels]
	// ------------------------------------------------------------------
	auto colorTF = PrototypeHelpers::makeIslandColorTF(
		static_cast<double>(minVoxels),
		static_cast<double>(maxVoxels));

	// ------------------------------------------------------------------
	// Create one surface actor per island, coloured by its voxel count
	// ------------------------------------------------------------------
	QJsonArray regionsArray;

	for (int idx = 0; idx < nIslands; ++idx)
	{
		const auto& island = islands[static_cast<std::size_t>(idx)];

		// Sample the transfer function at this island's voxel count
		double rgb[3] = { 1.0, 1.0, 1.0 };
		colorTF->GetColor(static_cast<double>(island.voxelCount), rgb);

		auto actor = PrototypeHelpers::makeIslandSurfaceActor(
			m_labelImage,
			island.label,
			rgb[0], rgb[1], rgb[2],
			0.55);

		m_islandActors.push_back(actor);

		if (ren)
			ren->AddActor(actor);

		qDebug("applyIslandSegmentationResult: island %d  label=%d  voxels=%lld  rgb=(%.3f,%.3f,%.3f)",
			   idx, island.label,
			   static_cast<long long>(island.voxelCount),
			   rgb[0], rgb[1], rgb[2]);

		// Augment the existing island JSON with the mapped colour for reference
		QJsonObject islandJson = island.json;
		islandJson[QStringLiteral("colorR")] = rgb[0];
		islandJson[QStringLiteral("colorG")] = rgb[1];
		islandJson[QStringLiteral("colorB")] = rgb[2];
		regionsArray.append(islandJson);
	}

	// ------------------------------------------------------------------
	// Scalar bar — only when there are at least 2 distinct islands so the
	// colour scale has meaningful variation to display
	// ------------------------------------------------------------------
	if (nIslands > 1 && ren)
	{
		m_islandScalarBar = PrototypeHelpers::makeIslandScalarBar(
			colorTF,
			minVoxels,
			maxVoxels);

		ren->AddActor(m_islandScalarBar);

		qDebug("applyIslandSegmentationResult: scalar bar added (%d islands, range %lld–%lld voxels).",
			   nIslands,
			   static_cast<long long>(minVoxels),
			   static_cast<long long>(maxVoxels));
	}

	// ------------------------------------------------------------------
	// Merge regions summary into the landmark JSON cache
	// ------------------------------------------------------------------
	m_landmarkJson[QStringLiteral("regions")] = regionsArray;

	qDebug("applyIslandSegmentationResult: %d islands cached in landmarkJson[\"regions\"].",
		   nIslands);

	if (ren)
		ui->volumeView->render();
}

// ---------------------------------------------------------------------------
// Regions slot — threshold + seeded BFS flood-fill ? island surface actors
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onRegions()
{
	// ------------------------------------------------------------------
	// Pre-conditions
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		qWarning("onRegions: no resliced image available; run Reslice first.");
		return;
	}

	if (m_landmarkResult.isEmpty())
	{
		qWarning("onRegions: no landmark points available; run Landmark first.");
		return;
	}

	if (!std::isfinite(m_threshold))
	{
		qWarning("onRegions: threshold is not finite; cannot segment.");
		return;
	}

	// ------------------------------------------------------------------
	// Collect the 6 landmark world-space seed points from m_landmarkPoints.
	// Each of the 3 axes contributes one positive and one negative seed.
	// ------------------------------------------------------------------
	std::vector<std::array<double, 3>> seeds;
	seeds.reserve(6);

	for (int i = 0; i < 3; ++i)
	{
		for (int d = 0; d < 2; ++d)
		{
			const double* pt = m_landmarkPoints[static_cast<std::size_t>(i)]
				[static_cast<std::size_t>(d)].data();
			seeds.push_back({ pt[0], pt[1], pt[2] });
		}
	}

	qDebug("onRegions: running seeded BFS with %zu seeds, threshold=%.4f",
		   seeds.size(), m_threshold);

	// ------------------------------------------------------------------
	// Progress callback
	// ------------------------------------------------------------------
	showProgressStart();
	const auto regionProgress = [this](int percent)
		{
			showProgressValue(percent);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		};

	// ------------------------------------------------------------------
	// Run segmentation
	// ------------------------------------------------------------------
	vtkSmartPointer<vtkImageData> labelImage;

	const std::vector<PrototypeHelpers::BoneIsland> islands =
		PrototypeHelpers::segmentBoneIslands(
			m_reslicedImage,
			m_threshold,
			seeds,
			labelImage,
			regionProgress);

	showProgressEnd();

	if (islands.empty())
	{
		qWarning("onRegions: no bone islands were found.");
		return;
	}

	applyIslandSegmentationResult(islands, labelImage);

	// Segmentation completed — advance to Segmented: no step buttons enabled.
	setWorkflowStep(WorkflowStep::Segmented);
}

// ---------------------------------------------------------------------------
// Regions Alt slot — morphological pipeline (smooth ? erode ? dilate ? connectivity)
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onRegionsAlt()
{
	// ------------------------------------------------------------------
	// Pre-conditions (identical to onRegions)
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		qWarning("onRegionsAlt: no resliced image available; run Reslice first.");
		return;
	}

	if (m_landmarkResult.isEmpty())
	{
		qWarning("onRegionsAlt: no landmark points available; run Landmark first.");
		return;
	}

	if (!std::isfinite(m_threshold))
	{
		qWarning("onRegionsAlt: threshold is not finite; cannot segment.");
		return;
	}

	// ------------------------------------------------------------------
	// Collect the 6 landmark world-space seed points from m_landmarkPoints.
	// Each of the 3 axes contributes one positive and one negative seed.
	// ------------------------------------------------------------------
	std::vector<std::array<double, 3>> seeds;
	seeds.reserve(6);

	for (int i = 0; i < 3; ++i)
	{
		for (int d = 0; d < 2; ++d)
		{
			const double* pt = m_landmarkPoints[static_cast<std::size_t>(i)]
				[static_cast<std::size_t>(d)].data();
			seeds.push_back({ pt[0], pt[1], pt[2] });
		}
	}

	qDebug("onRegionsAlt: running morphological pipeline with %zu seeds, threshold=%.4f",
		   seeds.size(), m_threshold);

	// ------------------------------------------------------------------
	// Progress callback
	// ------------------------------------------------------------------
	showProgressStart();
	const auto regionProgress = [this](int percent)
		{
			showProgressValue(percent);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		};

	// ------------------------------------------------------------------
	// Run alternate segmentation (default smoothStdDev=1.0, morphKernelSize=1)
	// ------------------------------------------------------------------
	vtkSmartPointer<vtkImageData> labelImage;

	const std::vector<PrototypeHelpers::BoneIsland> islands =
		PrototypeHelpers::segmentBoneIslandsAlternate(
			m_reslicedImage,
			m_threshold,
			seeds,
			labelImage,
			1.0,  // smoothStdDev: Gaussian standard deviation in voxel units
			1,    // morphKernelSize: half-width ? 3×3×3 structuring element
			regionProgress);

	showProgressEnd();

	if (islands.empty())
	{
		qWarning("onRegionsAlt: no bone islands were found.");
		return;
	}

	applyIslandSegmentationResult(islands, labelImage);

	// Segmentation completed — advance to Segmented: no step buttons enabled.
	setWorkflowStep(WorkflowStep::Segmented);
}

// ---------------------------------------------------------------------------
// Clean slot — post-segmentation clean step (stub)
// Enabled only after segmentation completes and at least 8 Reslice operations
// have been performed in the current session.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Clean slot — post-segmentation clean step
//
// Algorithm:
//   1. Derive the noise distribution from the cached background statistics:
//      draw = clamp(backgroundMean + U(-2?, +2?),  volMin, volMax)
//      where U(-2?, +2?) is a uniform random value in [-2·bgStdDev, +2·bgStdDev].
//   2. Deep-copy the resliced volume into C.
//   3. For every voxel v in the resliced volume:
//        if v > threshold  AND  v is NOT inside any segmented island label:
//            replace the corresponding voxel in C with a drawn noise value.
//   4. Display C via setImage().
//
// Pre-conditions (all checked below):
//   - m_reslicedImage    : resliced volume exists
//   - m_labelImage       : segmentation label map exists (0 = background, ?1 = island)
//   - m_threshold        : finite threshold
//   - m_backgroundMean / m_backgroundStdDev : cached from setImage()
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onClean()
{
	qDebug("onClean: clean step triggered (resliceCount=%d).", m_resliceCount);

	// ------------------------------------------------------------------
	// Pre-conditions
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		qWarning("onClean: no resliced image available; run Reslice first.");
		return;
	}

	if (!m_labelImage)
	{
		qWarning("onClean: no label image available; run Regions or Regions Alt first.");
		return;
	}

	if (!std::isfinite(m_threshold))
	{
		qWarning("onClean: threshold is not finite; cannot clean.");
		return;
	}

	// ------------------------------------------------------------------
	// Scalar range of the resliced volume — used to clamp random draws
	// ------------------------------------------------------------------

	const double scalarRange[2] = {
	m_imageStats.value(QStringLiteral("min")).toDouble(0.0),
	m_imageStats.value(QStringLiteral("max")).toDouble(255.0) };
	const double volMin = scalarRange[0];
	const double volMax = scalarRange[1];

	// ------------------------------------------------------------------
	// Background noise distribution parameters sourced from m_imageStats,
	// which is populated by setImage() via computeScalarThresholdStats().
	// "meanBg"   — mean scalar value of all below-threshold voxels.
	// "stdDevBg" — standard deviation of below-threshold voxels.
	// ------------------------------------------------------------------
	const double bgMean = m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);
	const double bgStdDev = m_imageStats.value(QStringLiteral("stdDevBg")).toDouble(0.0);
	const double noiseHalfWidth = 2.0 * bgStdDev;   // ± 2? uniform range

	qDebug("onClean: bgMean=%.4f  bgStdDev=%.4f  noiseHalfWidth=%.4f  "
		   "volMin=%.4f  volMax=%.4f  threshold=%.4f",
		   bgMean, bgStdDev, noiseHalfWidth, volMin, volMax, m_threshold);

	// ------------------------------------------------------------------
	// Initialize C++17 random number generator
	// uniform_real_distribution over [-noiseHalfWidth, +noiseHalfWidth]
	// ------------------------------------------------------------------
	std::mt19937_64                          rng{ std::random_device{}() };
	std::uniform_real_distribution<double>   noiseDist(-noiseHalfWidth, noiseHalfWidth);

	// ------------------------------------------------------------------
	// Deep-copy the resliced volume into C (the output cleaned volume)
	// ------------------------------------------------------------------
	auto cleanedImage = vtkSmartPointer<vtkImageData>::New();
	cleanedImage->DeepCopy(m_reslicedImage);

	vtkDataArray* reslicedScalars = m_reslicedImage->GetPointData()->GetScalars();
	vtkDataArray* labelScalars = m_labelImage->GetPointData()->GetScalars();
	vtkDataArray* cleanedScalars = cleanedImage->GetPointData()->GetScalars();

	if (!reslicedScalars || !labelScalars || !cleanedScalars)
	{
		qWarning("onClean: scalar arrays missing on resliced, label, or cleaned image.");
		return;
	}

	const vtkIdType nReslicedPoints = m_reslicedImage->GetNumberOfPoints();
	const vtkIdType nLabelPoints = m_labelImage->GetNumberOfPoints();

	// The label image is produced from the resliced image so their point
	// counts must match.  Guard against an unexpected mismatch.
	if (nReslicedPoints != nLabelPoints)
	{
		qWarning("onClean: resliced image point count (%lld) does not match "
				 "label image point count (%lld); aborting.",
				 static_cast<long long>(nReslicedPoints),
				 static_cast<long long>(nLabelPoints));
		return;
	}

	showProgressStart();

	// ------------------------------------------------------------------
	// Voxel loop  [progress 0 ? 80]
	//
	// For each voxel i:
	//   - scalar v  = resliced intensity
	//   - label  l  = segmentation label (0 = unlabelled / outside all islands)
	//
	// Replace in C when:
	//   v > threshold   (foreground intensity)
	//   AND l == 0      (not inside any segmented island)
	// ------------------------------------------------------------------
	vtkIdType replacedCount = 0;

	for (vtkIdType i = 0; i < nReslicedPoints; ++i)
	{
		// Progress: report every 64 K voxels, mapped to the [0, 80] sub-range.
		if ((i & 0xFFFF) == 0)
		{
			const int pct = static_cast<int>(
				std::clamp(static_cast<double>(i) / static_cast<double>(nReslicedPoints), 0.0, 1.0) * 80.0);
			showProgressValue(pct);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		}

		const double v = reslicedScalars->GetTuple1(i);
		const double l = labelScalars->GetTuple1(i);

		// Keep voxels that are below the threshold (background) or inside an island.
		if (v <= m_threshold || l != 0.0)
			continue;

		// Voxel is above-threshold AND outside every segmented island —
		// replace it with a background-noise draw clamped to [volMin, volMax].
		const double noise = noiseDist(rng);
		const double noiseVal = std::clamp(bgMean + noise, volMin, volMax);
		cleanedScalars->SetTuple1(i, noiseVal);
		++replacedCount;
	}

	cleanedScalars->Modified();
	cleanedImage->Modified();

	qDebug("onClean: replaced %lld above-threshold outside-island voxels with background noise.",
		   static_cast<long long>(replacedCount));

	// ------------------------------------------------------------------
	// VOI crop: tighten the cleaned volume to the region of interest.
	//
	// Steps:
	//   1. Compute Dmin = half the shortest inter-landmark paired distance
	//      (pos–neg pair along the same axis) in world coordinates.
	//   2. Build a vtkBoundingBox B over all segmented island voxels in
	//      world coordinates (label > 0 in m_labelImage).
	//   3. Inflate B by Dmin uniformly from its centroid.
	//   4. Map B to voxel extent in cleanedImage and apply vtkExtractVOI.
	// ------------------------------------------------------------------

	// Step 1 — Dmin: half the shortest paired landmark distance.
	// m_landmarkPoints[axis][0] = positive direction surface point
	// m_landmarkPoints[axis][1] = negative direction surface point
	double minPairedDist = std::numeric_limits<double>::max();
	for (int axis = 0; axis < 3; ++axis)
	{
		const double* lPos = m_landmarkPoints[static_cast<std::size_t>(axis)][0].data();
		const double* lNeg = m_landmarkPoints[static_cast<std::size_t>(axis)][1].data();
		const double dx = lPos[0] - lNeg[0];
		const double dy = lPos[1] - lNeg[1];
		const double dz = lPos[2] - lNeg[2];
		const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
		minPairedDist = std::min(minPairedDist, dist);
	}

	const double dMin = 0.5 * minPairedDist;

	qDebug("onClean: minPairedLandmarkDist=%.4f  dMin=%.4f", minPairedDist, dMin);

	// Step 2 — island bounding box in world coordinates  [progress 80 ? 95]
	// Iterate the label image; accumulate world positions of all labelled voxels.
	showProgressValue(80);
	m_progressBar->update();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	vtkBoundingBox islandBB;

	{
		const double* lbOrigin = m_labelImage->GetOrigin();
		const double* lbSpacing = m_labelImage->GetSpacing();
		const int* lbDims = m_labelImage->GetDimensions();
		vtkDataArray* lbScalars = m_labelImage->GetPointData()->GetScalars();

		const vtkIdType nLabelTotal = static_cast<vtkIdType>(lbDims[0])
			* static_cast<vtkIdType>(lbDims[1])
			* static_cast<vtkIdType>(lbDims[2]);

		for (int k = 0; k < lbDims[2]; ++k)
		{
			// Progress: report per slice, mapped to the [80, 95] sub-range.
			if ((k & 0xF) == 0)
			{
				const int pct = 80 + static_cast<int>(
					std::clamp(static_cast<double>(k) / static_cast<double>(lbDims[2]), 0.0, 1.0) * 15.0);
				showProgressValue(pct);
				m_progressBar->update();
				QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
			}

			for (int j = 0; j < lbDims[1]; ++j)
			{
				for (int i = 0; i < lbDims[0]; ++i)
				{
					const vtkIdType idx = static_cast<vtkIdType>(k) * lbDims[1] * lbDims[0]
						+ static_cast<vtkIdType>(j) * lbDims[0]
						+ i;

					if (lbScalars->GetTuple1(idx) == 0.0)
						continue;

					const double wx = lbOrigin[0] + i * lbSpacing[0];
					const double wy = lbOrigin[1] + j * lbSpacing[1];
					const double wz = lbOrigin[2] + k * lbSpacing[2];
					islandBB.AddPoint(wx, wy, wz);
				}
			}
		}
	}

	// Step 3 — inflate B by dMin uniformly from its centroid.
	// vtkBoundingBox::Inflate(delta) expands each face outward by delta.
	islandBB.Inflate(dMin);

	double bbBounds[6];
	islandBB.GetBounds(bbBounds);  // [xMin, xMax, yMin, yMax, zMin, zMax]

	qDebug("onClean: inflated island BB world  x=[%.3f,%.3f]  y=[%.3f,%.3f]  z=[%.3f,%.3f]",
		   bbBounds[0], bbBounds[1], bbBounds[2],
		   bbBounds[3], bbBounds[4], bbBounds[5]);

	// Step 4 — map world BB to voxel extent in cleanedImage and run vtkExtractVOI
	// [progress 95 ? 100]
	showProgressValue(95);
	m_progressBar->update();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	{
		const double* cOrigin = cleanedImage->GetOrigin();
		const double* cSpacing = cleanedImage->GetSpacing();
		const int* cDims = cleanedImage->GetDimensions();

		// Convert world bounds to nearest voxel indices, clamped to the image extent.
		auto worldToVoxelClamped = [&](double worldCoord, double origin, double spacing,
									   int dimSize) -> int
			{
				const int idx = static_cast<int>((worldCoord - origin) / spacing + 0.5);
				return std::clamp(idx, 0, dimSize - 1);
			};

		const int voiXMin = worldToVoxelClamped(bbBounds[0], cOrigin[0], cSpacing[0], cDims[0]);
		const int voiXMax = worldToVoxelClamped(bbBounds[1], cOrigin[0], cSpacing[0], cDims[0]);
		const int voiYMin = worldToVoxelClamped(bbBounds[2], cOrigin[1], cSpacing[1], cDims[1]);
		const int voiYMax = worldToVoxelClamped(bbBounds[3], cOrigin[1], cSpacing[1], cDims[1]);
		const int voiZMin = worldToVoxelClamped(bbBounds[4], cOrigin[2], cSpacing[2], cDims[2]);
		const int voiZMax = worldToVoxelClamped(bbBounds[5], cOrigin[2], cSpacing[2], cDims[2]);

		qDebug("onClean: VOI voxel extent  x=[%d,%d]  y=[%d,%d]  z=[%d,%d]",
			   voiXMin, voiXMax, voiYMin, voiYMax, voiZMin, voiZMax);

		auto extractVOI = vtkSmartPointer<vtkExtractVOI>::New();
		extractVOI->SetInputData(cleanedImage);
		extractVOI->SetVOI(voiXMin, voiXMax, voiYMin, voiYMax, voiZMin, voiZMax);
		extractVOI->SetSampleRate(1, 1, 1);
		extractVOI->Update();

		vtkImageData* cropped = extractVOI->GetOutput();
		if (cropped && cropped->GetNumberOfPoints() > 0)
		{
			auto croppedCopy = vtkSmartPointer<vtkImageData>::New();
			croppedCopy->DeepCopy(cropped);
			cleanedImage = croppedCopy;

			const int* croppedDims = cleanedImage->GetDimensions();
			qDebug("onClean: VOI crop applied — new dims: %d x %d x %d",
				   croppedDims[0], croppedDims[1], croppedDims[2]);
		}
		else
		{
			qWarning("onClean: vtkExtractVOI produced empty output; skipping crop.");
		}
	}

	showProgressEnd();

	// ------------------------------------------------------------------
	// Cache the cleaned volume and update the display.
	// setImage() is NOT called here to avoid resetting the workflow state
	// and recomputing PCA.  Instead we update only the VolumeView directly,
	// mirroring the minimal path used elsewhere when the image content
	// changes without replacing the pipeline source.
	// ------------------------------------------------------------------
	m_reslicedImage = cleanedImage;
	m_image = m_reslicedImage.Get();

	ui->volumeView->setImageData(m_reslicedImage);
	ui->volumeView->updateData();

	const double level = std::isfinite(m_threshold)
		? m_threshold
		: 0.5 * (scalarRange[0] + scalarRange[1]);

	const double window = 2.0 * m_imageStats.value(QStringLiteral("stdDev")).toDouble(1.0);

	ui->volumeView->setColorWindowLevel(window, level);

	ui->volumeView->renderer()->ResetCamera();
	ui->volumeView->renderer()->ResetCameraClippingRange();
	ui->volumeView->renderWindow()->Render();

	// Clean completed — advance to Cleaned: all step buttons disabled.
	setWorkflowStep(WorkflowStep::Cleaned);
}

// ---------------------------------------------------------------------------
// Restart slot — revert to the original image and reset the full workflow
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onRestart()
{
	if (!m_originalImage)
	{
		qDebug("onRestart: no original image cached; nothing to restore.");
		return;
	}

	qDebug("onRestart: reverting to original image and resetting workflow.");

	// Discard all derived state so setImage() treats this as a clean first load.
	// Clear island actors and scalar bar before releasing the label image.
	clearIslandActors();
	m_labelImage = nullptr;
	m_reslicedImage = nullptr;
	m_reslicedPcaJson = QJsonObject{};
	m_landmarkResult = QJsonObject{};
	m_landmarkJson = QJsonObject{};
	m_landmarkPoints = {};
	m_imageStats = QJsonObject{};

	// Reset the reslice counter so the Clean gate starts from zero on restart.
	m_resliceCount = 0;

	// Restore the view to the original image with its original PCA axes.
	// setImage() will recompute the PCA from the original image data and
	// rebuild all PCA overlay actors, which gives back the default axes.
	setImage(m_originalImage);

	// Ensure m_originalPcaJson is refreshed from the just-computed PCA
	// (setImage only writes it when m_reslicedImage is null, which it now is).
	if (m_pca.valid)
	{
		m_originalPcaJson = pcaResultToJson(m_pca);
		qDebug("onRestart: original PCA JSON refreshed.");
	}

	// Reset the workflow to the start: only Reslice enabled.
	setWorkflowStep(WorkflowStep::Idle);
}

// ---------------------------------------------------------------------------
// Async load
// ---------------------------------------------------------------------------

void PrototypeMainWindow::loadFromSidecarAsync(const QString& sidecarPath)
{
	QTimer::singleShot(0, this, [this, sidecarPath]()
	{
		loadFromSidecar(sidecarPath);
	});
}

// ---------------------------------------------------------------------------
// onOutlineToggled — forward the checked state to VolumeView::setOutlineVisible
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onOutlineToggled(bool checked)
{
	if (!ui || !ui->volumeView)
		return;

	ui->volumeView->setOutlineVisible(checked);
}
#include "PrototypeMainWindow.h"
#include "PrototypeHelpers.h"
#include "ui_MainWindow.h"

#include "VolumeView.h"
#include "ImageLoader.h"
#include "JsonUtils.h"

#include <vtkEventQtSlotConnect.h>
#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkRenderer.h>
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

#include <array>
#include <cmath>
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

	// "Landmark" toolbar button: searches along each PCA axis for surface transitions
	QAction* actLandmark = new QAction(tr("Landmark"), this);
	actLandmark->setToolTip(tr("Find surface landmark points along the PCA axes"));
	ui->toolBar->addAction(actLandmark);
	connect(actLandmark, &QAction::triggered, this, &PrototypeMainWindow::onLandmark);

	// "Reslice" toolbar button: reslice the volume aligned to the PCA axes
	QAction* actReslice = new QAction(tr("Reslice"), this);
	actReslice->setToolTip(tr("Reslice the volume aligned to the PCA principal axes"));
	ui->toolBar->addAction(actReslice);
	connect(actReslice, &QAction::triggered, this, &PrototypeMainWindow::onReslice);

	// "Regions" toolbar button: threshold + seeded BFS island segmentation
	QAction* actRegions = new QAction(tr("Regions"), this);
	actRegions->setToolTip(tr("Segment bone islands from the resliced volume using landmark seeds"));
	ui->toolBar->addAction(actRegions);
	connect(actRegions, &QAction::triggered, this, &PrototypeMainWindow::onRegions);

	// "Outline" toggle toolbar button: shows/hides the VolumeView bounding-box outline actor.
	// The action is checkable so it renders in a depressed state while the outline is visible
	// and returns to the normal raised state when unchecked.
	m_actOutline = new QAction(tr("Outline"), this);
	m_actOutline->setToolTip(tr("Toggle the volume bounding-box outline"));
	m_actOutline->setCheckable(true);
	m_actOutline->setChecked(false); // outline is hidden by default (matches VolumeView default)
	ui->toolBar->addAction(m_actOutline);
	connect(m_actOutline, &QAction::toggled, this, &PrototypeMainWindow::onOutlineToggled);

	// ImageLoader + VTK event wiring
	m_imageLoader    = vtkSmartPointer<ImageLoader>::New();
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

	for (auto& a : m_islandActors)
	{
		if (a) ren->RemoveActor(a);
	}
	m_islandActors.clear();
}

// ---------------------------------------------------------------------------
// PCA JSON serialisation helper
// ---------------------------------------------------------------------------

// static
QJsonObject PrototypeMainWindow::pcaResultToJson(const PcaResult& pca)
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
		axisObj[QStringLiteral("index")]      = i;
		axisObj[QStringLiteral("eigenvalue")] = pca.eigenvalues[i];
		axisObj[QStringLiteral("direction")]  = packVec3(pca.axes[i]);
		axesArray.append(axisObj);
	}

	QJsonObject obj;
	obj[QStringLiteral("centroid")]     = packVec3(pca.centroid);
	obj[QStringLiteral("circumRadius")] = pca.circumRadius;
	obj[QStringLiteral("axes")]         = axesArray;
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
	const QString outputName   = cropBaseName + QStringLiteral("_prototype.json");

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
	m_cropPath    = cropPath;

	m_imageLoader->SetInputPath(cropPath);
	m_imageLoader->SetImageType(ImageLoader::ImageType::NIFTI);
	m_imageLoader->Update();

	vtkSmartPointer<vtkImageData> out = m_imageLoader->GetOutput();
	if (!out)
		throw std::runtime_error(("ImageLoader returned null output for: " + cropPath).toStdString());

	const int* dims = out->GetDimensions();
	if (dims[0] <= 1 || dims[1] <= 1 || dims[2] <= 1)
		throw std::runtime_error(("ImageLoader produced invalid volume dimensions for: " + cropPath).toStdString());

	setImage(out);
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
	m_pca.valid      = false;
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
	// window = standard deviation of scalar values
	if (!image)
		return;

	double scalarRange[2] = { 0.0, 255.0 };
	image->GetScalarRange(scalarRange);

	const double level = std::isfinite(m_threshold)
		? m_threshold
		: 0.5 * (scalarRange[0] + scalarRange[1]);

	const double window = 2.0 * PrototypeHelpers::computeScalarStdDev(image);

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

	// Sphere glyph radius = 8 % of the circumsphere radius (4× the previous 2 % size)
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
		const double  R   = m_pca.circumRadius;

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
		m_tipActors[static_cast<std::size_t>(i * 2)]     = PrototypeHelpers::makeSphereActor(tipPos, glyphR, col[0], col[1], col[2]);
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
		const double axisDirPos[3] = {  m_pca.axes[i][0],  m_pca.axes[i][1],  m_pca.axes[i][2] };
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
		axisObj[QStringLiteral("index")]       = i;
		axisObj[QStringLiteral("eigenvalue")]  = m_pca.eigenvalues[i];
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
	m_landmarkResult[QStringLiteral("centroid")]     = packVec3(m_pca.centroid);
	m_landmarkResult[QStringLiteral("circumRadius")] = m_pca.circumRadius;
	m_landmarkResult[QStringLiteral("threshold")]    = m_threshold;
	m_landmarkResult[QStringLiteral("axes")]         = jsonLandmarks;

	// ------------------------------------------------------------------
	// Build the consolidated landmark JSON cache (written to disk on close).
	// ------------------------------------------------------------------
	m_landmarkJson = QJsonObject{};
	m_landmarkJson[QStringLiteral("sourceSidecar")] = m_sidecarPath;
	m_landmarkJson[QStringLiteral("cropImage")]     = m_cropPath;
	m_landmarkJson[QStringLiteral("threshold")]     = m_threshold;

	if (!m_originalPcaJson.isEmpty())
		m_landmarkJson[QStringLiteral("originalPca")] = m_originalPcaJson;

	if (!m_reslicedPcaJson.isEmpty())
		m_landmarkJson[QStringLiteral("reslicedPca")] = m_reslicedPcaJson;

	m_landmarkJson[QStringLiteral("landmarks")] = m_landmarkResult;

	qDebug("onLandmark: landmark JSON cached:\n%s",
	       qUtf8Printable(QJsonDocument(m_landmarkJson).toJson(QJsonDocument::Indented)));

	if (ren)
		ui->volumeView->render();
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

	auto reslice = vtkSmartPointer<vtkImageReslice>::New();
	reslice->SetInputData(m_image);
	reslice->SetResliceAxes(resliceAxes);
	reslice->SetInterpolationModeToLinear();
	reslice->AutoCropOutputOn();
	reslice->SetOutputDimensionality(3);
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
}

// ---------------------------------------------------------------------------
// Regions slot — threshold ? seeded BFS flood-fill ? island surface actors
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
	// Progress callback (occupies [0, 100] of the progress bar)
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

	// Cache the label image so it stays alive for the actors' pipeline
	m_labelImage = labelImage;

	// ------------------------------------------------------------------
	// Remove any actors left from a previous onRegions() run
	// ------------------------------------------------------------------
	clearIslandActors();

	vtkRenderer* ren = (ui && ui->volumeView) ? ui->volumeView->renderer() : nullptr;

	// ------------------------------------------------------------------
	// Assign visually distinct colours using a simple HSV rotation.
	// Six evenly-spaced hues cover all axis-endpoint seeds well.
	// ------------------------------------------------------------------
	const int nIslands = static_cast<int>(islands.size());

	// JSON array to accumulate island summaries
	QJsonArray regionsArray;

	for (int idx = 0; idx < nIslands; ++idx)
	{
		const auto& island = islands[static_cast<std::size_t>(idx)];

		// HSV ? RGB: hue cycles 0–360° across all islands
		const double hue = (360.0 * idx) / static_cast<double>(std::max(nIslands, 1));
		double r = 1.0, g = 1.0, b = 1.0;
		vtkMath::HSVToRGB(hue / 360.0, 0.85, 0.95, &r, &g, &b);

		auto actor = PrototypeHelpers::makeIslandSurfaceActor(
			m_labelImage,
			island.label,
			r, g, b,
			0.55);

		m_islandActors.push_back(actor);

		if (ren)
			ren->AddActor(actor);

		qDebug("onRegions: island %d  label=%d  voxels=%lld",
		       idx, island.label,
		       static_cast<long long>(island.voxelCount));

		regionsArray.append(island.json);
	}

	// ------------------------------------------------------------------
	// Merge regions summary into the landmark JSON cache so it is written
	// to the prototype sidecar on close alongside the landmark data.
	// ------------------------------------------------------------------
	m_landmarkJson[QStringLiteral("regions")] = regionsArray;

	qDebug("onRegions: %d islands segmented and cached in landmarkJson[\"regions\"].",
	       nIslands);

	if (ren)
		ui->volumeView->render();
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
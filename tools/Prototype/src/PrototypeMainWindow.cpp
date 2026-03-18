#include "PrototypeMainWindow.h"
#include "ui_MainWindow.h"

#include "VolumeView.h"
#include "ImageLoader.h"
#include "JsonUtils.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkImageData.h>
#include <vtkImageThreshold.h>
#include <vtkLine.h>
#include <vtkMath.h>
#include <vtkPoints.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRegularPolygonSource.h>
#include <vtkRenderer.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>
#include <vtkSuperquadricSource.h>

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QThread>

#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// Internal helpers (file-scope)
// ---------------------------------------------------------------------------
namespace
{
	static QJsonObject readJsonObjectFileOrThrow(const QString& path)
	{
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly))
			throw std::runtime_error(("Failed to open JSON file: " + path).toStdString());

		const QByteArray data = f.readAll();
		f.close();

		const QJsonDocument doc = QJsonDocument::fromJson(data);
		if (!doc.isObject())
			throw std::runtime_error(("Invalid JSON (expected object): " + path).toStdString());

		return doc.object();
	}

	static QString cropPathFromSidecarOrThrow(const QJsonObject& obj)
	{
		const QString cropPath = obj.value(QStringLiteral("crop"))
			.toObject()
			.value(QStringLiteral("outputPath"))
			.toString();

		if (cropPath.isEmpty())
			throw std::runtime_error("Sidecar missing crop.outputPath (crop not completed?).");

		return cropPath;
	}

	// Returns the threshold value from the sidecar, or quiet_NaN if absent.
	static double thresholdFromSidecar(const QJsonObject& obj)
	{
		const QJsonValue v = obj.value(QStringLiteral("threshold"))
			.toObject()
			.value(QStringLiteral("value"));

		if (v.isDouble())
		{
			qDebug("Threshold: %f", v.toDouble());
			return v.toDouble();
		}

		qDebug("Threshold: (not present)");
		return std::numeric_limits<double>::quiet_NaN();
	}

	// Computes the population standard deviation of the image's scalar values.
	static double computeScalarStdDev(vtkImageData* image)
	{
		if (!image)
			return 1.0;

		const vtkIdType nPoints = image->GetNumberOfPoints();
		if (nPoints <= 0)
			return 1.0;

		double sum   = 0.0;
		double sumSq = 0.0;

		for (vtkIdType i = 0; i < nPoints; ++i)
		{
			const double v = image->GetPointData()->GetScalars()->GetTuple1(i);
			sum   += v;
			sumSq += v * v;
		}

		const double mean     = sum / static_cast<double>(nPoints);
		const double variance = (sumSq / static_cast<double>(nPoints)) - (mean * mean);
		return std::sqrt(std::max(variance, 0.0));
	}

	// ---------------------------------------------------------------------------
	// PCA overlay helpers
	// ---------------------------------------------------------------------------

	// Result of PCA + circumsphere computation on the binary voxel set.
	struct PcaResult
	{
		double centroid[3];       // binary-volume centroid in world coordinates
		double axes[3][3];        // eigenvectors as rows: axes[i] = i-th principal axis (unit vector)
		double eigenvalues[3];    // eigenvalues in descending order
		double circumRadius;      // radius of the circumsphere (fits outside the bounding box)
	};

	// Compute PCA of the above-threshold voxels.
	// progressCb(percent) is called at key milestones [0..100] so the caller
	// can update a progress bar. Pass nullptr to skip all progress reporting.
	// Returns false and logs a warning when there are too few voxels.
	static bool computePca(vtkImageData* image, double threshold, PcaResult& result,
	                       const std::function<void(int)>& progressCb = nullptr)
	{
		if (!image)
			return false;

		const double* spacing = image->GetSpacing();
		const double* origin  = image->GetOrigin();
		const int*    dims    = image->GetDimensions();
		vtkDataArray* scalars = image->GetPointData()->GetScalars();

		if (!scalars)
			return false;

		// Report 0 % at the start of the first pass.
		if (progressCb) progressCb(0);

		// --- Pass 1: centroid and count ---
		double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
		vtkIdType count = 0;

		for (int k = 0; k < dims[2]; ++k)
		{
			for (int j = 0; j < dims[1]; ++j)
			{
				for (int i = 0; i < dims[0]; ++i)
				{
					const vtkIdType idx = static_cast<vtkIdType>(k) * dims[1] * dims[0]
					                    + static_cast<vtkIdType>(j) * dims[0]
					                    + i;
					if (scalars->GetTuple1(idx) < threshold)
						continue;

					const double wx = origin[0] + i * spacing[0];
					const double wy = origin[1] + j * spacing[1];
					const double wz = origin[2] + k * spacing[2];

					sumX += wx;
					sumY += wy;
					sumZ += wz;
					++count;
				}
			}
		}

		// Pass 1 complete: 40 %
		if (progressCb) progressCb(40);

		if (count < 3)
		{
			qWarning("computePca: fewer than 3 above-threshold voxels (%lld); PCA skipped.",
			         static_cast<long long>(count));
			return false;
		}

		const double n = static_cast<double>(count);
		result.centroid[0] = sumX / n;
		result.centroid[1] = sumY / n;
		result.centroid[2] = sumZ / n;

		// --- Pass 2: 3×3 covariance (upper-triangle, population formula) ---
		double c00 = 0.0, c01 = 0.0, c02 = 0.0;
		double c11 = 0.0, c12 = 0.0;
		double c22 = 0.0;

		// Track bounding box of the binary voxels (world space) for circumsphere radius
		double bbMin[3] = { std::numeric_limits<double>::max(),
		                    std::numeric_limits<double>::max(),
		                    std::numeric_limits<double>::max() };
		double bbMax[3] = { std::numeric_limits<double>::lowest(),
		                    std::numeric_limits<double>::lowest(),
		                    std::numeric_limits<double>::lowest() };

		for (int k = 0; k < dims[2]; ++k)
		{
			for (int j = 0; j < dims[1]; ++j)
			{
				for (int i = 0; i < dims[0]; ++i)
				{
					const vtkIdType idx = static_cast<vtkIdType>(k) * dims[1] * dims[0]
					                    + static_cast<vtkIdType>(j) * dims[0]
					                    + i;
					if (scalars->GetTuple1(idx) < threshold)
						continue;

					const double wx = origin[0] + i * spacing[0] - result.centroid[0];
					const double wy = origin[1] + j * spacing[1] - result.centroid[1];
					const double wz = origin[2] + k * spacing[2] - result.centroid[2];

					c00 += wx * wx;  c01 += wx * wy;  c02 += wx * wz;
					c11 += wy * wy;  c12 += wy * wz;
					c22 += wz * wz;

					// bounding box
					const double awx = wx + result.centroid[0];
					const double awy = wy + result.centroid[1];
					const double awz = wz + result.centroid[2];
					bbMin[0] = std::min(bbMin[0], awx); bbMax[0] = std::max(bbMax[0], awx);
					bbMin[1] = std::min(bbMin[1], awy); bbMax[1] = std::max(bbMax[1], awy);
					bbMin[2] = std::min(bbMin[2], awz); bbMax[2] = std::max(bbMax[2], awz);
				}
			}
		}

		// Pass 2 complete: 80 %
		if (progressCb) progressCb(80);

		c00 /= n; c01 /= n; c02 /= n;
		c11 /= n; c12 /= n;
		c22 /= n;

		// --- Eigen-decomposition via vtkMath::Jacobi ---
		// vtkMath::Jacobi expects a double** (array of row pointers)
		double row0[3] = { c00, c01, c02 };
		double row1[3] = { c01, c11, c12 };
		double row2[3] = { c02, c12, c22 };
		double* cov[3] = { row0, row1, row2 };

		double evecData[3][3]; // columns = eigenvectors after Jacobi
		double* evecs[3] = { evecData[0], evecData[1], evecData[2] };
		double  evals[3];

		vtkMath::Jacobi(cov, evals, evecs);
		// vtkMath::Jacobi returns eigenvalues in DESCENDING order

		// Transpose: Jacobi stores eigenvectors as columns of evecs[][] matrix
		// evecs[col][row], so axis i = (evecs[0][i], evecs[1][i], evecs[2][i])
		for (int i = 0; i < 3; ++i)
		{
			result.axes[i][0] = evecs[0][i];
			result.axes[i][1] = evecs[1][i];
			result.axes[i][2] = evecs[2][i];
			vtkMath::Normalize(result.axes[i]);
			result.eigenvalues[i] = evals[i];
		}

		// Circumsphere radius: half-diagonal of the binary bounding box,
		// centred on the centroid, guaranteed to lie outside the bounding box.
		const double dx = bbMax[0] - bbMin[0];
		const double dy = bbMax[1] - bbMin[1];
		const double dz = bbMax[2] - bbMin[2];
		result.circumRadius = 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz);

		qDebug("PCA centroid: (%.2f, %.2f, %.2f)", result.centroid[0], result.centroid[1], result.centroid[2]);
		qDebug("PCA eigenvalues: %.4f  %.4f  %.4f", evals[0], evals[1], evals[2]);
		qDebug("PCA circumsphere radius: %.2f", result.circumRadius);

		// Eigen-decomposition complete: 100 %
		if (progressCb) progressCb(100);

		return true;
	}

	// Build a thin line actor between two world-space points.
	static vtkSmartPointer<vtkActor> makeLineActor(
		const double p0[3], const double p1[3],
		double r, double g, double b, double lineWidth = 2.0)
	{
		auto pts  = vtkSmartPointer<vtkPoints>::New();
		auto line = vtkSmartPointer<vtkLine>::New();
		auto cells = vtkSmartPointer<vtkCellArray>::New();
		auto pd   = vtkSmartPointer<vtkPolyData>::New();

		pts->InsertNextPoint(p0);
		pts->InsertNextPoint(p1);
		line->GetPointIds()->SetId(0, 0);
		line->GetPointIds()->SetId(1, 1);
		cells->InsertNextCell(line);

		pd->SetPoints(pts);
		pd->SetLines(cells);

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputData(pd);

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetColor(r, g, b);
		actor->GetProperty()->SetLineWidth(static_cast<float>(lineWidth));
		actor->GetProperty()->SetLighting(false);

		return actor;
	}

	// Build a sphere glyph actor centred at `centre`.
	static vtkSmartPointer<vtkActor> makeSphereActor(
		const double centre[3], double radius,
		double r, double g, double b)
	{
		auto sphere = vtkSmartPointer<vtkSphereSource>::New();
		sphere->SetCenter(centre);
		sphere->SetRadius(radius);
		sphere->SetPhiResolution(16);
		sphere->SetThetaResolution(16);

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputConnection(sphere->GetOutputPort());

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetColor(r, g, b);

		return actor;
	}

	// Build a ring (closed polygon) actor.
	// centre    : world-space centre of the ring
	// normal    : unit vector perpendicular to the ring plane (eigen direction)
	// radius    : circumsphere radius
	// r,g,b     : line colour
	// lineWidth : rendered line width in pixels
	static vtkSmartPointer<vtkActor> makeRingActor(
		const double centre[3], const double normal[3], double radius,
		double r, double g, double b, double lineWidth = 2.0)
	{
		auto ring = vtkSmartPointer<vtkRegularPolygonSource>::New();
		ring->SetNumberOfSides(64);
		ring->SetRadius(radius);
		ring->SetCenter(centre);
		ring->SetNormal(normal);
		ring->GeneratePolygonOff(); // outline (closed polyline) only, no filled face

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputConnection(ring->GetOutputPort());

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetColor(r, g, b);
		actor->GetProperty()->SetLineWidth(static_cast<float>(lineWidth));
		actor->GetProperty()->SetLighting(false);

		return actor;
	}

} // namespace

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
// Public API
// ---------------------------------------------------------------------------

void PrototypeMainWindow::loadFromSidecar(const QString& sidecarPath)
{
	const QJsonObject sidecar = readJsonObjectFileOrThrow(sidecarPath);

	const QString cropPath = cropPathFromSidecarOrThrow(sidecar);
	qDebug("Project:   %s", qUtf8Printable(sidecarPath));
	qDebug("Crop path: %s", qUtf8Printable(cropPath));

	m_threshold = thresholdFromSidecar(sidecar);

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

	ui->volumeView->setImageData(image);
	ui->volumeView->updateData();

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

	const double window = 2.0 * computeScalarStdDev(image);

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

	PcaResult pca;
	const bool ok = computePca(image, m_threshold, pca, pcaProgress);
	showProgressEnd();

	if (!ok)
		return;

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	// Sphere glyph radius = 8 % of the circumsphere radius (4× the previous 2 % size)
	const double glyphR = 0.08 * pca.circumRadius;

	// Axis colours: R=axis0 (largest variance), G=axis1, B=axis2
	const double axisColors[3][3] = {
		{ 1.0, 0.2, 0.2 },  // axis 0 - red
		{ 0.2, 1.0, 0.2 },  // axis 1 - green
		{ 0.2, 0.2, 1.0 },  // axis 2 - blue
	};

	for (int i = 0; i < 3; ++i)
	{
		const double* col = axisColors[i];
		const double  R   = pca.circumRadius;

		// Tip points along +axis and -axis
		double tipPos[3], tipNeg[3];
		for (int d = 0; d < 3; ++d)
		{
			tipPos[d] = pca.centroid[d] + R * pca.axes[i][d];
			tipNeg[d] = pca.centroid[d] - R * pca.axes[i][d];
		}

		// Shaft from -tip to +tip
		m_axisActors[i] = makeLineActor(tipNeg, tipPos, col[0], col[1], col[2], 2.5);
		ren->AddActor(m_axisActors[i]);

		// Sphere glyphs at both ends (4x larger than the original 2 % size)
		m_tipActors[static_cast<std::size_t>(i * 2)]     = makeSphereActor(tipPos, glyphR, col[0], col[1], col[2]);
		m_tipActors[static_cast<std::size_t>(i * 2 + 1)] = makeSphereActor(tipNeg, glyphR, col[0], col[1], col[2]);
		ren->AddActor(m_tipActors[static_cast<std::size_t>(i * 2)]);
		ren->AddActor(m_tipActors[static_cast<std::size_t>(i * 2 + 1)]);

		// Circumsphere ring i:
		//   - centre  : positive tip of axis i (centroid + R * axes[i])
		//   - normal  : axes[i]  (the eigen direction for this axis)
		//   - radius  : circumsphere radius R
		// The ring therefore lies in the plane perpendicular to axes[i]
		// that passes through the positive axis tip on the circumsphere.
		m_ringActors[i] = makeRingActor(pca.centroid, pca.axes[i], R,
		                                col[0], col[1], col[2], 2.0);
		ren->AddActor(m_ringActors[i]);
	}

	ui->volumeView->render();
}

void PrototypeMainWindow::loadFromSidecarAsync(const QString& sidecarPath)
{
	// Post the load to the event loop so the window can show and paint
	// itself before the blocking VTK pipeline executes.
	QTimer::singleShot(0, this, [this, sidecarPath]()
	{
		loadFromSidecar(sidecarPath);
	});
}
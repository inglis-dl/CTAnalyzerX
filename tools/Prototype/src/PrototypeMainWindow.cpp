#include "PrototypeMainWindow.h"
#include "ui_MainWindow.h"

#include "VolumeView.h"
#include "ImageLoader.h"
#include "JsonUtils.h"

#include <vtkDataArray.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QThread>

#include <cmath>
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
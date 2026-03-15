#include "CropExporter.h"

#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QApplication>
#include <QDebug>
#include <QJsonArray>

#include <vtkSmartPointer.h>
#include <vtkExtractVOI.h>
#include <vtkNIFTIWriter.h>        // use vtk-dicom's writer
#include <vtkImageData.h>
#include <vtkAlgorithmOutput.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkCommand.h>
#include <vtkCellDataToPointData.h>
#include <vtkNIFTIReader.h>       // use vtk-dicom's reader for header metadata
#include <vtkCellData.h>
#include <vtkPointData.h>
#include <vtkNIFTIHeader.h>       // header type used by vtk-dicom writer

CropExporter::CropExporter(QObject* parent)
	: QObject(parent)
	, m_inputPort(nullptr)
	, m_inputImage(nullptr)
{
	// create the VTK-to-Qt connector once; connections are made per-writer at apply()
	m_vtkConn = vtkSmartPointer<vtkEventQtSlotConnect>::New();
	m_niftiWriter = vtkSmartPointer<vtkNIFTIWriter>::New();

	if (m_vtkConn) {
		// connect StartEvent
		m_vtkConn->Connect(m_niftiWriter, vtkCommand::StartEvent, this, SLOT(onVtkStartEvent()));
		// connect ProgressEvent
		m_vtkConn->Connect(m_niftiWriter, vtkCommand::ProgressEvent, this, SLOT(onVtkProgressEvent()));
		// connect EndEvent
		m_vtkConn->Connect(m_niftiWriter, vtkCommand::EndEvent, this, SLOT(onVtkEndEvent()));
	}
}

void CropExporter::setCropRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax)
{
	m_region = { xMin, xMax, yMin, yMax, zMin, zMax };
}

void CropExporter::setInputConnection(vtkAlgorithmOutput* port)
{
	m_inputPort = port;
}

void CropExporter::setInputData(vtkImageData* image)
{
	m_inputImage = image;
}

void CropExporter::setInputFilePath(const QString& inputPath)
{
	m_inputPath = inputPath;
}

void CropExporter::apply()
{
	// Validate prerequisites
	if (m_region[1] < m_region[0] || m_region[3] < m_region[2] || m_region[5] < m_region[4]) {
		emit writeFinished(QString(), false, QStringLiteral("Invalid crop region"));
		return;
	}

	if (m_inputPort == nullptr && m_inputImage == nullptr) {
		emit writeFinished(QString(), false, QStringLiteral("No input image or pipeline available"));
		return;
	}

	if (m_inputPath.isEmpty()) {
		emit writeFinished(QString(), false, QStringLiteral("No input file path provided"));
		return;
	}

	const int xdim = int(m_region[1] - m_region[0] + 1);
	const int ydim = int(m_region[3] - m_region[2] + 1);
	const int zdim = int(m_region[5] - m_region[4] + 1);

	const QString outPath = makeAutoOutputPath(m_inputPath, xdim, ydim, zdim);

	QApplication::setOverrideCursor(Qt::BusyCursor);

	// perform crop & write; writer events will be forwarded via vtkEventQtSlotConnect
	bool ok = performCropAndWrite(outPath);

	QApplication::restoreOverrideCursor();

	// centralized finalization (sidecar + finished signal)
	finalizeWrite(outPath, ok);
}

// New: allow saving to a chosen path (used by Save-only flow).
void CropExporter::saveToFile(const QString& outPath)
{
	if (outPath.isEmpty()) {
		emit writeFinished(QString(), false, QStringLiteral("No output path provided"));
		return;
	}

	// Validate crop extents and inputs as in apply()
	if (m_region[1] < m_region[0] || m_region[3] < m_region[2] || m_region[5] < m_region[4]) {
		emit writeFinished(QString(), false, QStringLiteral("Invalid crop region"));
		return;
	}
	if (m_inputPort == nullptr && m_inputImage == nullptr) {
		emit writeFinished(QString(), false, QStringLiteral("No input image or pipeline available"));
		return;
	}

	QApplication::setOverrideCursor(Qt::BusyCursor);
	bool ok = performCropAndWrite(outPath);
	QApplication::restoreOverrideCursor();

	// centralized finalization (sidecar + finished signal)
	finalizeWrite(outPath, ok);
}

bool CropExporter::performCropAndWrite(const QString& outPath)
{
	// Build ExtractVOI (preserves origin & spacing correctly for the extracted volume)
	vtkSmartPointer<vtkExtractVOI> extract = vtkSmartPointer<vtkExtractVOI>::New();

	if (m_inputPort) {
		extract->SetInputConnection(m_inputPort);
	}
	else if (m_inputImage) {
		extract->SetInputData(m_inputImage);
	}
	else {
		return false;
	}

	extract->SetVOI(
		static_cast<int>(m_region[0]), static_cast<int>(m_region[1]),
		static_cast<int>(m_region[2]), static_cast<int>(m_region[3]),
		static_cast<int>(m_region[4]), static_cast<int>(m_region[5])
	);

	qDebug() << m_region[0] << m_region[1]
		<< m_region[2] << m_region[3]
		<< m_region[4] << m_region[5];

	qDebug() << outPath.toUtf8().toStdString().c_str();

	// Force execution of the extraction so we have a concrete vtkImageData to inspect
	extract->Update();
	vtkImageData* extractedOutput = extract->GetOutput();
	if (!extractedOutput)
	{
		emit writeFinished(QString(), false, QStringLiteral("Extraction failed"));
		return false;
	}

	// Ensure we have active point scalars. If only cell scalars exist, convert them to point scalars.
	// This avoids vtkNIFTIWriter assuming point scalars exist (which can lead to scalarInfo==nullptr).
	if (!extractedOutput->GetPointData() || !extractedOutput->GetPointData()->GetScalars())
	{
		// Try converting cell data -> point data if available
		if (extractedOutput->GetCellData() && extractedOutput->GetCellData()->GetScalars())
		{
			vtkSmartPointer<vtkCellDataToPointData> c2p = vtkSmartPointer<vtkCellDataToPointData>::New();
			c2p->SetInputData(extractedOutput);
			c2p->PassCellDataOff(); // prefer point data
			c2p->Update();
			// Use the converted output as the data we hand to the writer
			extractedOutput = vtkImageData::SafeDownCast(c2p->GetOutput());
		}
		else
		{
			emit writeFinished(QString(), false, QStringLiteral("No scalar data available to write"));
			return false;
		}
	}

	// If the input file is a NIFTI, try to read only metadata and attach to the writer.
	// This provides qform/sform/voxel-offset and other header info to the writer.
	if (!m_inputPath.isEmpty())
	{
		// Only attempt when the file looks like a NIFTI (.nii/.nii.gz/.hdr/.img)
		QString lower = m_inputPath.toLower();
		if (lower.endsWith(".nii") || lower.endsWith(".nii.gz") || lower.endsWith(".hdr") || lower.endsWith(".img"))
		{
			vtkSmartPointer<vtkNIFTIReader> headerReader = vtkSmartPointer<vtkNIFTIReader>::New();
			headerReader->SetFileName(m_inputPath.toUtf8().constData());
			// Avoid reading whole data; UpdateInformation should populate header details
			headerReader->UpdateInformation();
			// If the reader provides a header object, attach it to the writer
			vtkNIFTIHeader* niftiHdr = headerReader->GetNIFTIHeader();
			if (niftiHdr)
			{
				// SetNIFTIHeader will be managed by the writer (it registers the object)
				m_niftiWriter->SetNIFTIHeader(niftiHdr);
			}
			// Optionally transfer Q/S form matrices if available via reader (reader API permitting).
			// (Not shown here to avoid assumptions about reader API beyond GetNIFTIHeader()).
		}
	}

	// Provide the actual image data to the writer (concrete data, with active point scalars)
	m_niftiWriter->SetInputData(extractedOutput);
	m_niftiWriter->SetFileName(outPath.toUtf8().constData());

	// Perform blocking write (writer will emit progress which we forward)
	m_niftiWriter->Write();

	// Note: writer->Write() returns void; rely on writer error codes/events if needed.
	return true;
}

// VTK event handlers -------------------------------------------------------

void CropExporter::onVtkStartEvent()
{
	emit writeStarted();
}

void CropExporter::onVtkProgressEvent()
{
	if (!m_niftiWriter) return;
	// writer provides GetProgress via vtkObject/vtkAlgorithm
	double p = m_niftiWriter->GetProgress();
	int ip = static_cast<int>(std::round(std::clamp(p, 0.0, 1.0) * 100.0));
	emit writeProgress(ip);
}

void CropExporter::onVtkEndEvent()
{
	emit writeProgress(100);
	emit writeStarted(); // ensure main window shows/hides appropriately; callers may ignore duplicate
}

// centralized finalization used by both apply() and saveToFile()
void CropExporter::finalizeWrite(const QString& outPath, bool success)
{
	if (success) {
		// Build sidecar params and emit request for sidecar persistence
		QJsonObject params;
		params[QLatin1String("operation")] = QLatin1String("crop");
		params[QLatin1String("cropped_path")] = outPath;
		QJsonArray ext;
		ext.append(m_region[0]); ext.append(m_region[1]);
		ext.append(m_region[2]); ext.append(m_region[3]);
		ext.append(m_region[4]); ext.append(m_region[5]);
		params[QLatin1String("crop_extents")] = ext;
		params[QLatin1String("timestamp")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

		emit sidecarUpdateRequested(outPath, params);
		emit writeFinished(outPath, true, QStringLiteral("Success"));
	}
	else {
		emit writeFinished(outPath, false, QStringLiteral("Write failed"));
	}
}

// Ensure generated filenames use uncompressed .nii only.
QString CropExporter::makeAutoOutputPath(const QString& inputPath, int xdim, int ydim, int zdim) const
{
	// Build a filename based on the input file and dimensions.
	// Use the input file directory so outputs are colocated with the source file.
	QFileInfo fi(inputPath);
	QString dir = fi.absolutePath();
	if (dir.isEmpty())
		dir = QDir::currentPath();

	// Use the base name (without all suffixes) and append crop info.
	QString base = fi.completeBaseName();

	// NOTE: produce uncompressed .nii output only and do not include a timestamp.
	QString outName = QStringLiteral("%1_crop_%2x%3x%4.nii")
		.arg(base)
		.arg(xdim)
		.arg(ydim)
		.arg(zdim);

	qDebug() << "CropExporter: auto-generated output path:" << outName;

	return QDir(dir).filePath(outName);
}
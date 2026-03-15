#pragma once

#include <QObject>
#include <array>
#include <QJsonObject>

#include <vtkSmartPointer.h>

class vtkAlgorithmOutput;
class vtkImageData;
class vtkEventQtSlotConnect;
class vtkNIFTIWriter;

class CropExporter : public QObject
{
	Q_OBJECT
public:
	explicit CropExporter(QObject* parent = nullptr);
	~CropExporter() override = default;

signals:
	// Emitted when writer starts
	void writeStarted();
	// Emitted periodically with percent [0..100]
	void writeProgress(int percent);
	// Emitted after a write attempt. success==true -> path contains file written.
	void writeFinished(const QString& path, bool success, const QString& message);
	// Request a sidecar update so the state machine / provenance handler can persist provenance.
	// (outPath, params) -> params contains crop_extents/timestamp/etc.
	void sidecarUpdateRequested(const QString& outPath, const QJsonObject& params);

public slots:
	// Supply inputs via slots (signals/slots-only integration).
	void setCropRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);
	void setInputConnection(vtkAlgorithmOutput* port); // optional pipeline connection
	void setInputData(vtkImageData* image);             // optional direct image pointer
	void setInputFilePath(const QString& inputPath);    // used to auto-generate output filename
	void apply();                                       // trigger the crop+write operation

	// New: save to a user-provided path. This is used by the Save-only flow.
	void saveToFile(const QString& outPath);

private slots:
	// VTK event handlers forwarded via vtkEventQtSlotConnect
	void onVtkStartEvent();
	void onVtkProgressEvent();
	void onVtkEndEvent();

private:
	bool performCropAndWrite(const QString& outPath);
	QString makeAutoOutputPath(const QString& inputPath, int xdim, int ydim, int zdim) const;

	// helper used by both apply() and saveToFile() to centralize sidecar + finish signalling
	void finalizeWrite(const QString& outPath, bool success);

	// transient state (no ownership)
	std::array<int, 6> m_region{ 0,0,0,0,0,0 };
	vtkAlgorithmOutput* m_inputPort = nullptr;
	vtkImageData* m_inputImage = nullptr;
	QString m_inputPath;

	// VTK-side wiring for writer progress/events (owned by this object)
	vtkSmartPointer<vtkEventQtSlotConnect> m_vtkConn;
	vtkSmartPointer<vtkNIFTIWriter> m_niftiWriter;
};

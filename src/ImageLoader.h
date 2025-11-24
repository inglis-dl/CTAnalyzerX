#ifndef IMAGELOADER_H
#define IMAGELOADER_H

#include <QString>
#include <vtkSmartPointer.h>
#include <vtkImageAlgorithm.h>
#include <vtkImageData.h>

class ImageLoader : public vtkImageAlgorithm {
public:
	enum class ImageType {
		ScancoISQ,
		DICOM,
		NIFTI
	};

	static ImageLoader* New();
	vtkTypeMacro(ImageLoader, vtkImageAlgorithm);

	void SetInputPath(const QString& path);
	void SetImageType(ImageType type);

	// For convenience, keep this method for non-pipeline usage
	vtkSmartPointer<vtkImageData> Load();

	// Add this method to get the last progress value
	double GetProgress() const;

	// Add this method for file type detection
	static bool CanReadFile(const QString& filePath);

protected:
	ImageLoader();
	~ImageLoader() override = default;

	// VTK pipeline overrides to properly forward metadata
	int RequestInformation(vtkInformation* request, vtkInformationVector** inputVector,
		vtkInformationVector* outputVector) override;
	int FillOutputPortInformation(int port, vtkInformation* info) override;

	// Helper: Forward VTK events from a reader to this ImageLoader
	void forwardReaderEvents(vtkObject* reader);

	// Static callback for VTK events, forwards to instance method
	static void onReaderEvent(vtkObject* caller, unsigned long eventId, void* clientData, void* callData);

	int RequestData(vtkInformation*, vtkInformationVector**, vtkInformationVector*) override;

private:
	QString inputPath;
	ImageType type;

	// Store the last progress value from forwarded events
	double lastProgress = 0.0;

	vtkSmartPointer<vtkImageData> LoadScancoISQ();
	vtkSmartPointer<vtkImageData> LoadDICOM();
	vtkSmartPointer<vtkImageData> LoadNIfTI();

	// Cached reader instance used for both RequestInformation and RequestData
	vtkSmartPointer<vtkImageAlgorithm> cachedReader;

	// Ensure cachedReader exists and is configured for current path/type
	void EnsureReaderInitialized();

	ImageLoader(const ImageLoader&) = delete;
	void operator=(const ImageLoader&) = delete;
};

#endif // IMAGELOADER_H

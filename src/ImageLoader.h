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
		DICOM
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

	// Forward VTK events from a reader to this ImageLoader
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

	ImageLoader(const ImageLoader&) = delete;
	void operator=(const ImageLoader&) = delete;
};

#endif // IMAGELOADER_H

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

protected:
	ImageLoader();
	~ImageLoader() override = default;

	int RequestData(vtkInformation*, vtkInformationVector**, vtkInformationVector*) override;

private:
	QString inputPath;
	ImageType type;

	vtkSmartPointer<vtkImageData> LoadScancoISQ();
	vtkSmartPointer<vtkImageData> LoadDICOM();

	ImageLoader(const ImageLoader&) = delete;
	void operator=(const ImageLoader&) = delete;
};

#endif // IMAGELOADER_H

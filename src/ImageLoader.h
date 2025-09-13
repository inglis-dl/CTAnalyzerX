#ifndef IMAGELOADER_H
#define IMAGELOADER_H

#include <QString>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>

class ImageLoader {
public:
	enum class ImageType {
		ScancoISQ,
		DICOM
	};

	ImageLoader() = default;
	~ImageLoader() = default;

	void setInputPath(const QString& path);
	void setImageType(ImageType type);

	vtkSmartPointer<vtkImageData> load();

private:
	QString inputPath;
	ImageType type;

	vtkSmartPointer<vtkImageData> loadScancoISQ();
	vtkSmartPointer<vtkImageData> loadDICOM();
};


#endif // IMAGELOADER_H

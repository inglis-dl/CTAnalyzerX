#include "ImageLoader.h"

#include <vtkDICOMReader.h>
#include <vtkScancoCTReader.h>
#include <vtkNew.h>
#include <vtkDICOMDirectory.h>

void ImageLoader::setInputPath(const QString& path) {
	this->inputPath = path;
}

void ImageLoader::setImageType(ImageType type) {
	this->type = type;
}

vtkSmartPointer<vtkImageData> ImageLoader::load() {
	switch (type) {
		case ImageType::ScancoISQ:
		return loadScancoISQ();
		case ImageType::DICOM:
		return loadDICOM();
		default:
		return nullptr;
	}
}

vtkSmartPointer<vtkImageData> ImageLoader::loadScancoISQ() {
	auto reader = vtkSmartPointer<vtkScancoCTReader>::New();
	reader->SetFileName(inputPath.toUtf8().constData());
	reader->Update();

	return reader->GetOutput();
}

vtkSmartPointer<vtkImageData> ImageLoader::loadDICOM() {

	// Scan the directory for DICOM series
	vtkNew<vtkDICOMDirectory> dicomDirectory;
	dicomDirectory->SetDirectoryName(inputPath.toUtf8().constData());
	dicomDirectory->RequirePixelDataOn(); // Only include image data
	dicomDirectory->Update();

	int numSeries = dicomDirectory->GetNumberOfSeries();
	if (numSeries < 1) {
		std::cerr << "No DICOM image series found in directory!" << std::endl;
		return nullptr;
	}

	// Read the first series
	auto reader = vtkSmartPointer<vtkDICOMReader>::New();
	reader->SetFileNames(dicomDirectory->GetFileNamesForSeries(0));
	reader->SetMemoryRowOrderToFileNative(); // Optional: preserves original row order
	reader->Update();

	return reader->GetOutput();
}

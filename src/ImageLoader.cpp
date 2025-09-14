#include "ImageLoader.h"

#include <vtkObjectFactory.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkDICOMReader.h>
#include <vtkScancoCTReader.h>
#include <vtkNew.h>
#include <vtkDICOMDirectory.h>
#include <QFileInfo>

// VTK object factory macro
vtkStandardNewMacro(ImageLoader);

ImageLoader::ImageLoader()
	: inputPath(), type(ImageType::DICOM)
{
	// No input ports, 1 output port
	this->SetNumberOfInputPorts(0);
	this->SetNumberOfOutputPorts(1);
}

void ImageLoader::SetInputPath(const QString& path) {
	this->inputPath = path;

	QFileInfo info(path);
	if (info.isDir()) {
		this->type = ImageType::DICOM;
	}
	else if (path.endsWith(".isq", Qt::CaseInsensitive)) {
		this->type = ImageType::ScancoISQ;
	}
	else {
		// Default or unknown, fallback to DICOM
		this->type = ImageType::DICOM;
	}
	this->Modified();
}

void ImageLoader::SetImageType(ImageType type) {
	this->type = type;
	this->Modified();
}

vtkSmartPointer<vtkImageData> ImageLoader::Load() {
	switch (type) {
		case ImageType::ScancoISQ:
		return LoadScancoISQ();
		case ImageType::DICOM:
		return LoadDICOM();
		default:
		return nullptr;
	}
}

vtkSmartPointer<vtkImageData> ImageLoader::LoadScancoISQ() {
	auto reader = vtkSmartPointer<vtkScancoCTReader>::New();
	reader->SetFileName(inputPath.toUtf8().constData());
	reader->Update();
	return reader->GetOutput();
}

vtkSmartPointer<vtkImageData> ImageLoader::LoadDICOM() {
	QFileInfo info(inputPath);
	QString directoryPath;

	if (info.isDir()) {
		directoryPath = inputPath;
	}
	else {
		// If a file is given, use its parent directory
		directoryPath = info.absolutePath();
	}

	// Scan the directory for DICOM series
	vtkNew<vtkDICOMDirectory> dicomDirectory;
	dicomDirectory->SetDirectoryName(directoryPath.toUtf8().constData());
	dicomDirectory->RequirePixelDataOn();
	dicomDirectory->Update();

	int numSeries = dicomDirectory->GetNumberOfSeries();
	if (numSeries < 1) {
		std::cerr << "No DICOM image series found in directory!" << std::endl;
		return nullptr;
	}

	// Read the first series
	auto reader = vtkSmartPointer<vtkDICOMReader>::New();
	reader->SetFileNames(dicomDirectory->GetFileNamesForSeries(0));
	reader->SetMemoryRowOrderToFileNative();
	reader->Update();

	return reader->GetOutput();
}

// VTK pipeline: produce vtkImageData output
int ImageLoader::RequestData(
	vtkInformation* vtkNotUsed(request),
	vtkInformationVector** vtkNotUsed(inputVector),
	vtkInformationVector* outputVector)
{
	vtkInformation* outInfo = outputVector->GetInformationObject(0);

	vtkSmartPointer<vtkImageData> image;
	switch (type) {
		case ImageType::ScancoISQ:
		image = LoadScancoISQ();
		break;
		case ImageType::DICOM:
		image = LoadDICOM();
		break;
		default:
		image = nullptr;
		break;
	}

	if (!image) {
		return 0;
	}

	// Set the output pointer to the loaded image data
	outInfo->Set(vtkDataObject::DATA_OBJECT(), image);
	return 1;
}

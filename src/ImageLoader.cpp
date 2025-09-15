#include "ImageLoader.h"
#include <QFileInfo>

#include <vtkCommand.h>
#include <vtkDICOMDirectory.h>
#include <vtkDICOMReader.h>
#include <vtkEventForwarderCommand.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkScancoCTReader.h>
#include <vtkSmartPointer.h>

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

// Helper: Forward VTK events from a reader to this ImageLoader
void ImageLoader::forwardReaderEvents(vtkObject* reader)
{
	// Forward StartEvent, ProgressEvent, EndEvent
	for (unsigned long eventId : {vtkCommand::StartEvent, vtkCommand::ProgressEvent, vtkCommand::EndEvent}) {
		vtkSmartPointer<vtkEventForwarderCommand> forwarder = vtkSmartPointer<vtkEventForwarderCommand>::New();
		forwarder->SetTarget(this);
		reader->AddObserver(eventId, forwarder);
	}
}

// Static callback for VTK events, forwards to instance method
void ImageLoader::onReaderEvent(vtkObject* caller, unsigned long eventId, void* clientData, void* callData)
{
	ImageLoader* self = static_cast<ImageLoader*>(clientData);
	if (self) {
		if (eventId == vtkCommand::ProgressEvent && callData) {
			// Progress is passed as a double pointer in callData
			self->lastProgress = *static_cast<double*>(callData);
		}
		self->InvokeEvent(eventId, callData);
	}
}

double ImageLoader::GetProgress() const
{
	return lastProgress;
}

vtkSmartPointer<vtkImageData> ImageLoader::LoadScancoISQ() {
	auto reader = vtkSmartPointer<vtkScancoCTReader>::New();
	reader->SetFileName(inputPath.toUtf8().constData());
	forwardReaderEvents(reader);
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
		directoryPath = info.absolutePath();
	}

	vtkNew<vtkDICOMDirectory> dicomDirectory;
	dicomDirectory->SetDirectoryName(directoryPath.toUtf8().constData());
	dicomDirectory->RequirePixelDataOn();
	forwardReaderEvents(dicomDirectory);
	dicomDirectory->Update();

	int numSeries = dicomDirectory->GetNumberOfSeries();
	if (numSeries < 1) {
		std::cerr << "No DICOM image series found in directory!" << std::endl;
		return nullptr;
	}

	auto reader = vtkSmartPointer<vtkDICOMReader>::New();
	reader->SetFileNames(dicomDirectory->GetFileNamesForSeries(0));
	reader->SetMemoryRowOrderToFileNative();
	forwardReaderEvents(reader);
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

bool ImageLoader::CanReadFile(const QString& filePath)
{
	QFileInfo info(filePath);
	if (!info.exists() || !info.isFile() || !info.isReadable())
		return false;

	QString lower = filePath.toLower();
	QByteArray ba = filePath.toLocal8Bit();
	const char* cfile = ba.constData();

	// Check for Scanco ISQ and related files using vtkScancoCTReader
	if (lower.endsWith(".isq") || lower.endsWith(".rsq") || lower.endsWith(".rad") || lower.endsWith(".aim")) {
		auto scancoReader = vtkSmartPointer<vtkScancoCTReader>::New();
		if (scancoReader->CanReadFile(cfile) == 1)
			return true;
	}

	// Check for DICOM by extension (could be extended with a DICOM reader check)
	if (lower.endsWith(".dcm") || lower.endsWith(".dicom")) {
		return true;
	}

	return false;
}










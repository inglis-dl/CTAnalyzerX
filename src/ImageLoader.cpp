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
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkNIFTIImageReader.h> // added for NIfTI support
#include <vtkCallbackCommand.h>

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
	else if (path.endsWith(".nii.gz", Qt::CaseInsensitive) || path.endsWith(".nii", Qt::CaseInsensitive)) {
		// NIfTI single-file volumes (.nii or compressed .nii.gz)
		this->type = ImageType::NIFTI;
	}
	else {
		// Default or unknown, fallback to DICOM
		this->type = ImageType::DICOM;
	}
	this->Modified();
	// Invalidate cached reader so it will be recreated with the new path
	this->cachedReader = nullptr;
}

void ImageLoader::SetImageType(ImageType type) {
	this->type = type;
	this->Modified();
	this->cachedReader = nullptr;
}

vtkSmartPointer<vtkImageData> ImageLoader::Load() {
	switch (type) {
		case ImageType::ScancoISQ:
		return LoadScancoISQ();
		case ImageType::DICOM:
		return LoadDICOM();
		case ImageType::NIFTI:
		return LoadNIfTI();
		default:
		return nullptr;
	}
}

// Helper: Forward VTK events from a reader to this ImageLoader
void ImageLoader::forwardReaderEvents(vtkObject* reader)
{
	// Forward StartEvent and EndEvent using the event forwarder (preserves source->this propagation).
	for (unsigned long eventId : {vtkCommand::StartEvent, vtkCommand::EndEvent}) {
		vtkSmartPointer<vtkEventForwarderCommand> forwarder = vtkSmartPointer<vtkEventForwarderCommand>::New();
		forwarder->SetTarget(this);
		reader->AddObserver(eventId, forwarder);
	}

	// For ProgressEvent we need to capture the callData (double*) and store it into
	// ImageLoader::lastProgress. Use a vtkCallbackCommand bound to the static onReaderEvent
	// so ImageLoader can update its lastProgress and re-emit the event.
	vtkSmartPointer<vtkCallbackCommand> progressCb = vtkSmartPointer<vtkCallbackCommand>::New();
	progressCb->SetClientData(this);
	progressCb->SetCallback([](vtkObject* caller, unsigned long eventId, void* clientData, void* callData) {
		// delegate to the existing static helper so behavior is consistent
		ImageLoader::onReaderEvent(caller, eventId, clientData, callData);
	});
	reader->AddObserver(vtkCommand::ProgressEvent, progressCb);
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

vtkSmartPointer<vtkImageData> ImageLoader::LoadNIfTI()
{
	if (inputPath.isEmpty())
		return nullptr;

	auto reader = vtkSmartPointer<vtkNIFTIImageReader>::New();
	reader->SetFileName(inputPath.toUtf8().constData());
	forwardReaderEvents(reader);
	reader->Update();
	return reader->GetOutput();
}

// Ensure a single reader instance is created and configured for current type/path.
void ImageLoader::EnsureReaderInitialized()
{
	if (this->cachedReader)
	{
		// already initialized
		return;
	}

	if (this->inputPath.isEmpty())
	{
		return;
	}

	QFileInfo info(this->inputPath);

	if (this->type == ImageType::ScancoISQ)
	{
		auto r = vtkSmartPointer<vtkScancoCTReader>::New();
		r->SetFileName(this->inputPath.toUtf8().constData());
		forwardReaderEvents(r);
		this->cachedReader = r;
	}
	else if (this->type == ImageType::NIFTI)
	{
		auto nr = vtkSmartPointer<vtkNIFTIImageReader>::New();
		nr->SetFileName(this->inputPath.toUtf8().constData());
		forwardReaderEvents(nr);
		this->cachedReader = nr;
	}
	else // DICOM
	{
		// Use vtkDICOMDirectory to discover files in the directory and pass them to vtkDICOMReader.
		QString directoryPath = info.isDir() ? this->inputPath : info.absolutePath();

		vtkNew<vtkDICOMDirectory> dicomDirectory;
		dicomDirectory->SetDirectoryName(directoryPath.toUtf8().constData());
		dicomDirectory->RequirePixelDataOn();
		forwardReaderEvents(dicomDirectory);
		dicomDirectory->Update();

		int numSeries = dicomDirectory->GetNumberOfSeries();
		if (numSeries < 1)
		{
			std::cerr << "No DICOM image series found in directory!" << std::endl;
			return;
		}

		auto dr = vtkSmartPointer<vtkDICOMReader>::New();
		// Pass the discovered file list to the reader (vtkDICOMReader has SetFileNames, not SetDirectoryName)
		dr->SetFileNames(dicomDirectory->GetFileNamesForSeries(0));
		dr->SetMemoryRowOrderToFileNative();
		forwardReaderEvents(dr);
		this->cachedReader = dr;
	}
}

// Forward WHOLE_EXTENT / SPACING / ORIGIN / DIRECTION from the underlying reader to the pipeline.
int ImageLoader::RequestInformation(vtkInformation* vtkNotUsed(request),
	vtkInformationVector** vtkNotUsed(inputVector), vtkInformationVector* outputVector)
{
	vtkInformation* outInfo = outputVector->GetInformationObject(0);
	if (!outInfo)
		return 0;

	// Ensure a reader exists and that it has populated its output information.
	this->EnsureReaderInitialized();
	if (!this->cachedReader)
		return 1; // nothing to forward

	// Ask the reader to fill its output information (lightweight)
	this->cachedReader->UpdateInformation();

	vtkInformation* rOut = this->cachedReader->GetOutputInformation(0);
	if (!rOut)
		return 1;

	// Copy WHOLE_EXTENT
	if (rOut->Has(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT()))
	{
		int wholeExt[6];
		rOut->Get(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(), wholeExt);
		outInfo->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(), wholeExt, 6);
	}

	// SPACING
	if (rOut->Has(vtkDataObject::SPACING()))
	{
		double spacing[3];
		rOut->Get(vtkDataObject::SPACING(), spacing);
		outInfo->Set(vtkDataObject::SPACING(), spacing, 3);
	}

	// ORIGIN
	if (rOut->Has(vtkDataObject::ORIGIN()))
	{
		double origin[3];
		rOut->Get(vtkDataObject::ORIGIN(), origin);
		outInfo->Set(vtkDataObject::ORIGIN(), origin, 3);
	}

	// DIRECTION (if present)
	if (rOut->Has(vtkDataObject::DIRECTION()))
	{
		double dir[9];
		rOut->Get(vtkDataObject::DIRECTION(), dir);
		outInfo->Set(vtkDataObject::DIRECTION(), dir, 9);
	}

	// Optionally forward scalar type / number of components etc.
	if (rOut->Has(vtkDataObject::DATA_TYPE_NAME()))
	{
		const char* dt = rOut->Get(vtkDataObject::DATA_TYPE_NAME());
		outInfo->Set(vtkDataObject::DATA_TYPE_NAME(), dt);
	}

	return 1;
}

// Tell VTK our output is vtkImageData
int ImageLoader::FillOutputPortInformation(int port, vtkInformation* info)
{
	(void)port;
	info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkImageData");
	return 1;
}

// VTK pipeline: produce vtkImageData output
int ImageLoader::RequestData(
	vtkInformation* vtkNotUsed(request),
	vtkInformationVector** vtkNotUsed(inputVector),
	vtkInformationVector* outputVector)
{
	vtkInformation* outInfo = outputVector->GetInformationObject(0);

	// Ensure a persistent reader exists and is configured
	this->EnsureReaderInitialized();
	if (!this->cachedReader)
		return 0;

	// Execute the reader to produce data (heavy operation)
	this->cachedReader->Update();

	// Grab produced image and set as this algorithm's output
	vtkImageData* img = vtkImageData::SafeDownCast(this->cachedReader->GetOutputDataObject(0));
	if (!img)
		return 0;

	outInfo->Set(vtkDataObject::DATA_OBJECT(), img);
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

	// Check for NIfTI (single-file .nii or compressed .nii.gz)
	if (lower.endsWith(".nii.gz") || lower.endsWith(".nii")) {
		// use reader probe for stricter detection
		auto niftiProbe = vtkSmartPointer<vtkNIFTIImageReader>::New();
		if (niftiProbe->CanReadFile(cfile) == 1)
			return true;
		// fall through to false otherwise
	}

	// Check for DICOM by extension (could be extended with a DICOM reader check)
	if (lower.endsWith(".dcm") || lower.endsWith(".dicom")) {
		return true;
	}

	return false;
}

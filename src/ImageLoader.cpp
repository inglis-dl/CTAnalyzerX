#include "ImageLoader.h"
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDir>

#include <vtkCommand.h>
#include <vtkDICOMDirectory.h>
#include <vtkDICOMReader.h>
#include <vtkDICOMMetaData.h>
#include <vtkDICOMTag.h>
#include <vtkDICOMValue.h>
#include <vtkEventForwarderCommand.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkScancoCTReader.h>
#include <vtkSmartPointer.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkNIFTIReader.h>
#include <vtkCallbackCommand.h>
#include <vtkAlgorithm.h>
#include <vtkNIFTIHeader.h>

#include <iostream>

// VTK object factory macro
vtkStandardNewMacro(ImageLoader);

// Parse DICOM DT or concatenated date+time strings to QDateTime.
// Accepts forms like:
//   YYYYMMDD
//   YYYYMMDDHHMMSS
//   YYYYMMDDHHMMSS.FFFFFF
//   YYYYMMDDHHMMSS+ZZMM
static QDateTime parseDicomDateTimeString(const QString& dicomDT)
{
	if (dicomDT.isEmpty()) return QDateTime();

	// Extract leading components: YYYY MM DD [HH MM SS [.frac]] [+-ZZMM]
	QRegularExpression re(R"(^\s*(\d{4})(\d{2})(\d{2})(?:(\d{2})(\d{2})(\d{2})(?:\.(\d+))?)?(?:([+\-]\d{2})(\d{2}))?)");
	auto m = re.match(dicomDT);
	if (!m.hasMatch()) {
		return QDateTime();
	}

	int year = m.captured(1).toInt();
	int month = m.captured(2).toInt();
	int day = m.captured(3).toInt();
	int hour = m.captured(4).isEmpty() ? 0 : m.captured(4).toInt();
	int minute = m.captured(5).isEmpty() ? 0 : m.captured(5).toInt();
	int second = m.captured(6).isEmpty() ? 0 : m.captured(6).toInt();
	QString frac = m.captured(7);

	QDate date(year, month, day);
	QTime time(hour, minute, second);
	if (!date.isValid() || !time.isValid()) return QDateTime();

	QDateTime dt(date, time, Qt::UTC); // interpret as local-less, we'll adjust below if timezone present

	if (!frac.isEmpty()) {
		// convert fraction to milliseconds (pad/truncate)
		int ms = 0;
		if (frac.length() >= 3) ms = frac.left(3).toInt();
		else ms = frac.leftJustified(3, '0').toInt();
		dt = dt.addMSecs(ms);
	}

	// timezone offset present?
	if (!m.captured(8).isEmpty()) {
		QString sign = m.captured(8).left(1);
		int tzHour = m.captured(8).mid(1).toInt();
		int tzMin = m.captured(9).toInt();
		int offsetSeconds = tzHour * 3600 + tzMin * 60;
		if (sign == "-") offsetSeconds = -offsetSeconds;
		// The parsed dt was constructed in UTC; to convert the supplied local-with-offset into UTC:
		// supplied_time + (-offset) -> UTC time. Since dt currently treated as UTC, subtract offset.
		dt = dt.addSecs(-offsetSeconds);
	}

	return dt.toLocalTime(); // return as local time for display consistency
}

// Extract date string from a vtkScancoCTReader (creation or modification).
static QString extractDateFromScanco(vtkScancoCTReader* r)
{
	if (!r) return QString();
	const char* c = r->GetCreationDate();
	if (c && *c) return QString::fromUtf8(c);
	const char* m = r->GetModificationDate();
	if (m && *m) return QString::fromUtf8(m);
	return QString();
}

// Extract acquisition date/time from a vtkDICOMReader if DICOM metadata is available.
// Tries AcquisitionDateTime (0008,002A) first, then StudyDate+StudyTime, SeriesDate+SeriesTime,
// then InstanceCreationDate+InstanceCreationTime. Returns ISO datetime string on success,
// empty string otherwise.
static QString extractDateFromDICOM(vtkDICOMReader* reader, const QString& directoryPath, const QString& inputPath)
{
	if (!reader) return QString();

	vtkDICOMMetaData* meta = reader->GetMetaData();
	if (meta) {
		// Try AcquisitionDateTime (0008,002A)
		vtkDICOMTag acqDtTag(0x0008, 0x002A);
		auto it = meta->Find(acqDtTag);
		if (it != meta->End()) {
			vtkDICOMValue v = it->GetValue();
			const char* s = v.GetCharData();
			if (s && *s) {
				QDateTime dt = parseDicomDateTimeString(QString::fromUtf8(s));
				if (dt.isValid()) return dt.toString(Qt::ISODate);
			}
		}

		// Helper to try date+time tag pairs
		auto tryPair = [&](unsigned short dg, unsigned short de, unsigned short tg, unsigned short te)->QDateTime {
			vtkDICOMTag dTag(dg, de);
			vtkDICOMTag tTag(tg, te);
			auto itD = meta->Find(dTag);
			if (itD != meta->End()) {
				const char* ds = itD->GetValue().GetCharData();
				QString dateStr = ds ? QString::fromUtf8(ds) : QString();
				auto itT = meta->Find(tTag);
				QString timeStr;
				if (itT != meta->End()) {
					const char* ts = itT->GetValue().GetCharData();
					timeStr = ts ? QString::fromUtf8(ts) : QString();
				}
				if (!dateStr.isEmpty()) {
					QDateTime dt = parseDicomDateTimeString(dateStr + timeStr);
					if (dt.isValid()) return dt;
				}
			}
			return QDateTime();
			};

		QDateTime dt;

		dt = tryPair(0x0008, 0x0020, 0x0008, 0x0030); // StudyDate + StudyTime
		if (dt.isValid()) return dt.toString(Qt::ISODate);

		dt = tryPair(0x0008, 0x0021, 0x0008, 0x0031); // SeriesDate + SeriesTime
		if (dt.isValid()) return dt.toString(Qt::ISODate);

		dt = tryPair(0x0008, 0x0012, 0x0008, 0x0013); // InstanceCreationDate + InstanceCreationTime
		if (dt.isValid()) return dt.toString(Qt::ISODate);
	}

	// If metadata probes failed, fall back to filesystem timestamps (prefer birthTime)
	QFileInfo fi(inputPath);
	if (!fi.exists() || fi.isDir()) {
		// If provided a directory, attempt to find any file inside
		QDir d(directoryPath);
		QStringList entries = d.entryList(QDir::Files, QDir::Name);
		if (!entries.isEmpty()) {
			QFileInfo candidate(d.absoluteFilePath(entries.first()));
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
			QDateTime t = candidate.birthTime();
			if (!t.isValid()) t = candidate.lastModified();
#else
			QDateTime t = candidate.lastModified();
#endif
			if (t.isValid()) return t.toString(Qt::ISODate);
		}
	}
	else {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
		QDateTime t = fi.birthTime();
		if (!t.isValid()) t = fi.lastModified();
#else
		QDateTime t = fi.lastModified();
#endif
		if (t.isValid()) return t.toString(Qt::ISODate);
	}

	return QString();
}

static QString detectUnitsFromDICOM(vtkDICOMReader* reader)
{
	if (!reader) return QString();

	vtkDICOMMetaData* meta = reader->GetMetaData();
	if (!meta) return QString();

	// Common tags that imply distances in mm:
	// PixelSpacing (0028,0030), ImagerPixelSpacing (0018,1164),
	// SpacingBetweenSlices (0018,0088), SliceThickness (0018,0050)
	vtkDICOMTag tPixel(0x0028, 0x0030);
	vtkDICOMTag tImager(0x0018, 0x1164);
	vtkDICOMTag tSpacingBetween(0x0018, 0x0088);
	vtkDICOMTag tSliceThickness(0x0018, 0x0050);

	if (meta->Find(tPixel) != meta->End() ||
		meta->Find(tImager) != meta->End() ||
		meta->Find(tSpacingBetween) != meta->End() ||
		meta->Find(tSliceThickness) != meta->End()) {
		return QStringLiteral("mm");
	}

	return QString();
}

static QString detectUnitsFromScanco(vtkScancoCTReader* reader)
{
	// scancodump prints element sizes with [mm] in the reference — assume mm
	(void)reader;
	return QStringLiteral("mm");
}

static QString detectUnitsFromNIfTI(vtkNIFTIReader* reader)
{
	if (!reader) return QString();

	vtkNIFTIHeader* hdrObj = reader->GetNIFTIHeader();
	if (!hdrObj) return QString();

	int xyzt = hdrObj->GetXYZTUnits();

	// low 3 bits = spatial units (same semantics as nifti: 1=m,2=mm,3=um)
	switch (xyzt & 0x7) {
		case vtkNIFTIHeader::UnitsMeter:
		return QStringLiteral("m");
		case vtkNIFTIHeader::UnitsMM:
		return QStringLiteral("mm");
		case vtkNIFTIHeader::UnitsMicron:
		return QStringLiteral("um");
		default:
		return QString();
	}
}

// Helper to populate m_jsonMeta from a vtkImageData output and emit it
static void populateAndEmitMeta(ImageLoader* loader, vtkImageData* img,
	const QString& inputPath,
	const QString& fileType,
	const QString& explicitDate = QString(),
	const QString& units = QString())
{
	if (!loader) return;

	QJsonObject meta;
	QFileInfo fi(inputPath);
	meta.insert(QStringLiteral("fileName"), fi.fileName());
	meta.insert(QStringLiteral("fileType"), fileType);

	QString dateStr = explicitDate;
	if (dateStr.isEmpty()) {
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
		QDateTime t = fi.birthTime();
		if (!t.isValid()) t = fi.lastModified();
#else
		QDateTime t = fi.lastModified();
#endif
		if (t.isValid()) dateStr = t.toString(Qt::ISODate);
	}
	meta.insert(QStringLiteral("date"), dateStr.isEmpty() ? QStringLiteral("unknown") : dateStr);

	if (img) {
		double range[2] = { 0.0, 0.0 };
		img->GetScalarRange(range);
		QJsonArray rangeA;
		rangeA.append(range[0]);
		rangeA.append(range[1]);
		meta.insert(QStringLiteral("range"), rangeA);

		int dims[3] = { 0, 0, 0 };
		img->GetDimensions(dims);
		QJsonArray dimsA;
		dimsA.append(dims[0]);
		dimsA.append(dims[1]);
		dimsA.append(dims[2]);
		meta.insert(QStringLiteral("dims"), dimsA);

		double origin[3] = { 0.0, 0.0, 0.0 };
		img->GetOrigin(origin);
		QJsonArray originA;
		originA.append(origin[0]);
		originA.append(origin[1]);
		originA.append(origin[2]);
		meta.insert(QStringLiteral("origin"), originA);

		double spacing[3] = { 1.0, 1.0, 1.0 };
		img->GetSpacing(spacing);
		QJsonArray spacingA;
		spacingA.append(spacing[0]);
		spacingA.append(spacing[1]);
		spacingA.append(spacing[2]);
		meta.insert(QStringLiteral("spacing"), spacingA);

		// New: include scalar type name (e.g. "unsigned char", "short", etc.)
		// Use vtkImageData::GetScalarTypeAsString() to obtain a human-readable name.
		const char* st = img->GetScalarTypeAsString();
		if (st && *st) {
			meta.insert(QStringLiteral("scalarType"), QString::fromUtf8(st));
		}
		else {
			meta.insert(QStringLiteral("scalarType"), QStringLiteral("unknown"));
		}
	}

	if (!units.isEmpty()) {
		meta.insert(QStringLiteral("units"), units);
	}

	loader->setJsonMeta(meta);
	if (loader->metaEmitter()) {
		// emit signal to interested Qt consumers
		loader->metaEmitter()->metaUpdated(loader->jsonMeta());
	}
}

ImageLoader::ImageLoader()
	: inputPath(), type(ImageType::DICOM), m_metaEmitter(std::make_unique<ImageLoaderMetaEmitter>())
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

double ImageLoader::GetProgress() const
{
	return lastProgress;
}

vtkSmartPointer<vtkImageData> ImageLoader::LoadScancoISQ() {
	auto reader = vtkSmartPointer<vtkScancoCTReader>::New();
	reader->SetFileName(inputPath.toUtf8().constData());
	forwardReaderEvents(reader);
	reader->Update();

	vtkImageData* out = reader->GetOutput();
	// Populate JSON meta and emit with Scanco-provided date if available
	QString scDate = extractDateFromScanco(reader);
	QString units = detectUnitsFromScanco(reader);
	populateAndEmitMeta(this, out, inputPath, QStringLiteral("ISQ/Scanco"), scDate, units);

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

	vtkImageData* out = reader->GetOutput();

	// Attempt to extract acquisition datetime from DICOM metadata, otherwise filesystem fallback.
	QString dateStr = extractDateFromDICOM(reader, directoryPath, inputPath);

	// Best-effort units detection from DICOM metadata (assume mm when tags exist).
	QString units = detectUnitsFromDICOM(reader);

	// Populate JSON meta and emit
	populateAndEmitMeta(this, out, inputPath, QStringLiteral("DICOM"), dateStr, units);

	return reader->GetOutput();
}

vtkSmartPointer<vtkImageData> ImageLoader::LoadNIfNI()
{
	if (inputPath.isEmpty())
		return nullptr;

	auto reader = vtkSmartPointer<vtkNIFTIReader>::New();
	reader->SetFileName(inputPath.toUtf8().constData());
	forwardReaderEvents(reader);
	reader->Update();
	vtkImageData* out = reader->GetOutput();

	// NIfTI header lacks a standardized acquisition datetime; fall back to filesystem timestamp
	// NIfTI header: best-effort detection of spatial units
	QString units = detectUnitsFromNIfTI(reader);

	populateAndEmitMeta(this, out, inputPath, QStringLiteral("NIFTI"), QString(), units);

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
		auto nr = vtkSmartPointer<vtkNIFTIReader>::New();
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

	// Populate and emit JSON meta so UI consumers are updated when using the VTK pipeline path.
	// Determine fileType and any explicit date available from specific readers.
	QString fileType;
	QString explicitDate;
	QString units;

	switch (this->type) {
		case ImageLoader::ImageType::ScancoISQ:
		fileType = QStringLiteral("ISQ/Scanco");
		if (auto sc = vtkScancoCTReader::SafeDownCast(this->cachedReader)) {
			explicitDate = extractDateFromScanco(sc);
			units = detectUnitsFromScanco(sc);
		}
		break;
		case ImageLoader::ImageType::NIFTI:
		fileType = QStringLiteral("NIFTI");
		if (auto nr = vtkNIFTIReader::SafeDownCast(this->cachedReader)) {
			units = detectUnitsFromNIfTI(nr);
		}
		break;
		default: // DICOM
		fileType = QStringLiteral("DICOM");
		if (auto dr = vtkDICOMReader::SafeDownCast(this->cachedReader)) {
			QFileInfo fi(this->inputPath);
			QString directoryPath = fi.isDir() ? this->inputPath : fi.absolutePath();
			explicitDate = extractDateFromDICOM(dr, directoryPath, this->inputPath);
			units = detectUnitsFromDICOM(dr);
		}
		break;
	}

	populateAndEmitMeta(this, img, this->inputPath, fileType, explicitDate, units);

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
		auto niftiProbe = vtkSmartPointer<vtkNIFTIReader>::New();
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

void ImageLoader::setJsonMeta(const QJsonObject& meta)
{
	m_jsonMeta = meta;
}


// Forward events from a reader object to this ImageLoader instance.
// We create a vtkCallbackCommand, set its clientData to 'this' and use
// ImageLoader::onReaderEvent as the callback. Keep the callback in
// m_callbacks so it remains alive while observers are registered.
void ImageLoader::forwardReaderEvents(vtkObject* reader)
{
	if (!reader) return;

	// create callback command
	vtkSmartPointer<vtkCallbackCommand> cb = vtkSmartPointer<vtkCallbackCommand>::New();
	cb->SetClientData(this);
	cb->SetCallback(ImageLoader::onReaderEvent);

	// Observe Start/Progress/End events and forward them
	reader->AddObserver(vtkCommand::StartEvent, cb);
	reader->AddObserver(vtkCommand::ProgressEvent, cb);
	reader->AddObserver(vtkCommand::EndEvent, cb);

	// retain ownership so the callback object stays alive while the reader has observers
	m_callbacks.push_back(cb);
}

// Static callback invoked by VTK when observed reader emits events.
// It updates the ImageLoader::lastProgress on progress and forwards the event
// by invoking the same event on the ImageLoader instance so external listeners
// (e.g., vtkEventQtSlotConnect in MainWindow) will receive Start/Progress/End.
void ImageLoader::onReaderEvent(vtkObject* caller, unsigned long eventId, void* clientData, void* callData)
{
	ImageLoader* self = static_cast<ImageLoader*>(clientData);
	if (!self) return;

	if (eventId == vtkCommand::ProgressEvent) {
		double p = 0.0;
		// Preferred: query algorithm progress if available
		if (auto alg = vtkAlgorithm::SafeDownCast(caller)) {
			p = alg->GetProgress();
		}
		else if (callData) {
			// Some VTK callbacks pass a pointer to a double progress value in callData
			p = *reinterpret_cast<double*>(callData);
		}
		// clamp to [0..1]
		if (p < 0.0) p = 0.0;
		if (p > 1.0) p = 1.0;
		self->lastProgress = p;
	}

	// Re-emit the event on this ImageLoader so consumers can observe it.
	self->InvokeEvent(eventId, callData);
}
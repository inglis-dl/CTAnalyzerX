#include "ImageInfoWidget.h"
#include "ui_ImageInfoWidget.h"

#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

ImageInfoWidget::ImageInfoWidget(QWidget* parent)
	: QWidget(parent)
	, ui(new Ui::ImageInfoWidget)
{
	ui->setupUi(this);
	clear();
}

ImageInfoWidget::~ImageInfoWidget()
{
	delete ui;
}

void ImageInfoWidget::clear()
{
	// Keep same placeholders as provided in the .ui file for consistency
	ui->labelFileValue->setText(QStringLiteral("unknown"));
	ui->labelTypeValue->setText(QStringLiteral("unknown"));
	ui->label_6->setText(QStringLiteral("unknown"));        // date label widget in .ui
	ui->labelRangeValue->setText(QStringLiteral("[0,0]"));
	ui->labelDimsValue->setText(QStringLiteral("[0,0,0]"));
	ui->labelOriginValue->setText(QStringLiteral("(0,0,0)"));
	ui->labelSpacingValue->setText(QStringLiteral("(1,1,1)"));
}

QString ImageInfoWidget::detectFileType(const QString& filePath) const
{
	if (filePath.isEmpty()) return QStringLiteral("unknown");

	const QString ext = QFileInfo(filePath).suffix().toLower();

	// Common file extensions heuristics
	if (ext == QLatin1String("dcm") || ext == QLatin1String("dicom")) {
		return QStringLiteral("DICOM");
	}
	if (ext == QLatin1String("nii") || ext == QLatin1String("nii.gz") || ext == QLatin1String("hdr") ||
		ext == QLatin1String("img")) {
		return QStringLiteral("NIFTI/Analyze");
	}
	// ISQ / Scanco (common scanner formats)
	if (ext == QLatin1String("isq") || ext == QLatin1String("raw") || ext == QLatin1String("scanco")) {
		return QStringLiteral("ISQ/Scanco");
	}
	// VTK/MetaImage etc
	if (ext == QLatin1String("mhd") || ext == QLatin1String("mha")) {
		return QStringLiteral("MetaImage");
	}
	if (ext == QLatin1String("vti") || ext == QLatin1String("vtk")) {
		return QStringLiteral("VTK Image");
	}
	// Generic fallback: if file is a directory (DICOM series)
	QFileInfo fi(filePath);
	if (fi.isDir()) {
		// Heuristic: directory may contain DICOM series
		return QStringLiteral("DICOM (series)");
	}

	// Unknown extension - return the extension string capitalized
	return ext.isEmpty() ? QStringLiteral("unknown") : ext.toUpper();
}

QDateTime ImageInfoWidget::fileCreationOrModifiedTime(const QString& filePath) const
{
	if (filePath.isEmpty()) return QDateTime();

	QFileInfo fi(filePath);
	// Prefer birthTime (creation) if available; otherwise lastModified
#if (QT_VERSION >= QT_VERSION_CHECK(5, 10, 0))
	QDateTime t = fi.birthTime();
	if (!t.isValid()) t = fi.lastModified();
#else
	QDateTime t = fi.lastModified();
#endif
	return t;
}

QString ImageInfoWidget::formatRange(double minv, double maxv) const
{
	return QStringLiteral("[%1,%2]").arg(QString::number(minv), QString::number(maxv));
}

QString ImageInfoWidget::formatDims(int dims[3]) const
{
	return QStringLiteral("[%1,%2,%3]").arg(dims[0]).arg(dims[1]).arg(dims[2]);
}

QString ImageInfoWidget::formatPoint(const double p[3]) const
{
	return QStringLiteral("(%1,%2,%3)")
		.arg(QString::number(p[0]), QString::number(p[1]), QString::number(p[2]));
}

void ImageInfoWidget::setFileOnly(const QString& filePath)
{
	if (filePath.isEmpty()) {
		clear();
		return;
	}

	QFileInfo fi(filePath);
	ui->labelFileValue->setText(fi.fileName());
	ui->labelTypeValue->setText(detectFileType(filePath));

	const QDateTime dt = fileCreationOrModifiedTime(filePath);
	if (dt.isValid()) {
		ui->label_6->setText(dt.toString(Qt::ISODate));
	}
	else {
		ui->label_6->setText(QStringLiteral("unknown"));
	}

	// No image data available -> clear image-specific values to placeholders
	ui->labelRangeValue->setText(QStringLiteral("[0,0]"));
	ui->labelDimsValue->setText(QStringLiteral("[0,0,0]"));
	ui->labelOriginValue->setText(QStringLiteral("(0,0,0)"));
	ui->labelSpacingValue->setText(QStringLiteral("(1,1,1)"));
}

void ImageInfoWidget::setImage(vtkImageData* image, const QString& filePath)
{
	if (!image) {
		// If no image, fall back to file-only or clear
		if (!filePath.isEmpty()) setFileOnly(filePath);
		else clear();
		return;
	}

	// File info if provided
	if (!filePath.isEmpty()) {
		QFileInfo fi(filePath);
		ui->labelFileValue->setText(fi.fileName());
		ui->labelTypeValue->setText(detectFileType(filePath));
	}
	else {
		ui->labelFileValue->setText(QStringLiteral("in-memory"));
		ui->labelTypeValue->setText(QStringLiteral("in-memory image"));
	}

	// Acquire best-effort acquisition datetime:
	// NOTE: If a dedicated image reader parsed DICOM tags, the loader should call
	// a specialized setter or call setFileOnly() with the accurate timestamp.
	const QDateTime fileDt = fileCreationOrModifiedTime(filePath);
	if (fileDt.isValid()) {
		ui->label_6->setText(fileDt.toString(Qt::ISODate));
	}
	else {
		ui->label_6->setText(QStringLiteral("unknown"));
	}

	// Scalar range (vtkImageData exposes GetScalarRange)
	double range[2] = { 0.0, 0.0 };
	image->GetScalarRange(range);

	// Scalar type (human-readable)
	const char* st = image->GetScalarTypeAsString();
	const QString scalarTypeStr = (st && *st) ? QString::fromUtf8(st) : QStringLiteral("unknown");

	// Build combined string: "[min,max] scalarType"
	QString rangeWithType = formatRange(range[0], range[1]);
	if (!scalarTypeStr.isEmpty()) {
		rangeWithType += QStringLiteral(" ") + scalarTypeStr;
	}
	ui->labelRangeValue->setText(rangeWithType);

	// Dimensions
	int dims[3] = { 0, 0, 0 };
	image->GetDimensions(dims);
	ui->labelDimsValue->setText(formatDims(dims));

	// Origin
	double origin[3] = { 0.0, 0.0, 0.0 };
	image->GetOrigin(origin);
	ui->labelOriginValue->setText(formatPoint(origin));

	// Spacing
	double spacing[3] = { 1.0, 1.0, 1.0 };
	image->GetSpacing(spacing);
	ui->labelSpacingValue->setText(formatPoint(spacing));
}

void ImageInfoWidget::updateFromMeta(const QJsonObject& meta)
{
	// fileName
	if (meta.contains(QStringLiteral("fileName"))) {
		ui->labelFileValue->setText(meta.value(QStringLiteral("fileName")).toString(QStringLiteral("unknown")));
	}

	// fileType
	if (meta.contains(QStringLiteral("fileType"))) {
		ui->labelTypeValue->setText(meta.value(QStringLiteral("fileType")).toString(QStringLiteral("unknown")));
	}

	// date
	if (meta.contains(QStringLiteral("date"))) {
		const QString d = meta.value(QStringLiteral("date")).toString(QStringLiteral("unknown"));
		ui->label_6->setText(d.isEmpty() ? QStringLiteral("unknown") : d);
	}

	// range
	if (meta.contains(QStringLiteral("range"))) {
		QJsonArray a = meta.value(QStringLiteral("range")).toArray();
		if (a.size() >= 2) {
			double minv = a.at(0).toDouble(0.0);
			double maxv = a.at(1).toDouble(0.0);

			// check for scalarType in meta and append it to the displayed text
			QString scalarType;
			if (meta.contains(QStringLiteral("scalarType"))) {
				scalarType = meta.value(QStringLiteral("scalarType")).toString(QStringLiteral("")).trimmed();
			}

			QString rangeText = formatRange(minv, maxv);
			if (!scalarType.isEmpty()) {
				rangeText += QStringLiteral(" ") + scalarType;
			}

			ui->labelRangeValue->setText(rangeText);
		}
	}

	// dims
	if (meta.contains(QStringLiteral("dims"))) {
		QJsonArray a = meta.value(QStringLiteral("dims")).toArray();
		if (a.size() >= 3) {
			int d[3] = { a.at(0).toInt(0), a.at(1).toInt(0), a.at(2).toInt(0) };
			ui->labelDimsValue->setText(formatDims(d));
		}
	}

	// origin
	if (meta.contains(QStringLiteral("origin"))) {
		QJsonArray a = meta.value(QStringLiteral("origin")).toArray();
		if (a.size() >= 3) {
			double p[3] = { a.at(0).toDouble(0.0), a.at(1).toDouble(0.0), a.at(2).toDouble(0.0) };
			ui->labelOriginValue->setText(formatPoint(p));
		}
	}

	// spacing
	if (meta.contains(QStringLiteral("spacing"))) {
		QJsonArray a = meta.value(QStringLiteral("spacing")).toArray();
		if (a.size() >= 3) {
			double p[3] = { a.at(0).toDouble(1.0), a.at(1).toDouble(1.0), a.at(2).toDouble(1.0) };
			ui->labelSpacingValue->setText(formatPoint(p));
		}
	}
}
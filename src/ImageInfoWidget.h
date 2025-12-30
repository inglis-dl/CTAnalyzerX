#pragma once

#include <QWidget>
#include <QDateTime>
#include <QJsonObject>

namespace Ui { class ImageInfoWidget; }

class vtkImageData;

class ImageInfoWidget : public QWidget
{
	Q_OBJECT
public:
	explicit ImageInfoWidget(QWidget* parent = nullptr);
	~ImageInfoWidget() override;

	// Clear displayed info to defaults / placeholders.
	void clear();

public slots:
	// Populate the widget from a vtkImageData plus optional filename (preferred).
	// Callers that load image files should call this after loading.
	void setImage(vtkImageData* image, const QString& filePath = QString());

	// If only a filename is available (no vtkImageData), call this to at least show
	// filename/type/date using filesystem metadata and extension heuristics.
	void setFileOnly(const QString& filePath);

	// New: update the widget from a JSON metadata object produced by ImageLoader
	void updateFromMeta(const QJsonObject& meta);

private:
	Ui::ImageInfoWidget* ui;

	// Best-effort type detection from filename/extension.
	QString detectFileType(const QString& filePath) const;

	// Best-effort acquisition datetime. If DICOM headers were parsed by the loader
	// they can call setImage() and set a more accurate timestamp via this helper.
	QDateTime fileCreationOrModifiedTime(const QString& filePath) const;

	// Format helpers
	QString formatRange(double minv, double maxv) const;
	QString formatDims(int dims[3]) const;
	QString formatPoint(const double p[3]) const;
	QString formatSpacing(const double s[3]) const;
};

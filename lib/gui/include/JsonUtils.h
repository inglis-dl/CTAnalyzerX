#pragma once

#include <QJsonObject>
#include <QString>

namespace JsonUtils
{
	QString sidecarPathForImage(const QString& imagePath);
	QJsonObject readJsonSidecar(const QString& imagePath);
	bool writeJsonSidecar(const QString& imagePath, const QJsonObject& meta);

	// Convenience: create a basic crop sidecar (parameters may be empty if unknown)
	bool writeCropSidecar(const QString& outImagePath, const QString& sourceImagePath, const QJsonObject& parameters);
}
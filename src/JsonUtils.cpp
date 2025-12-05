#include "JsonUtils.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QDebug>

namespace JsonUtils
{

	QString sidecarPathForImage(const QString& imagePath)
	{
		QFileInfo fi(imagePath);
		return fi.absolutePath() + QDir::separator() + fi.completeBaseName() + QStringLiteral(".json");
	}

	QJsonObject readJsonSidecar(const QString& imagePath)
	{
		QJsonObject empty;
		const QString sidePath = sidecarPathForImage(imagePath);
		QFile f(sidePath);
		if (!f.open(QIODevice::ReadOnly)) {
			return empty;
		}
		const QByteArray b = f.readAll();
		f.close();
		const QJsonDocument doc = QJsonDocument::fromJson(b);
		if (!doc.isObject()) return empty;
		return doc.object();
	}

	bool writeJsonSidecar(const QString& imagePath, const QJsonObject& meta)
	{
		const QString sidePath = sidecarPathForImage(imagePath);
		QSaveFile f(sidePath);
		if (!f.open(QIODevice::WriteOnly)) {
			qWarning() << "JsonUtils: failed to open sidecar for write:" << sidePath;
			return false;
		}
		const QJsonDocument doc(meta);
		f.write(doc.toJson(QJsonDocument::Indented));
		if (!f.commit()) {
			qWarning() << "JsonUtils: failed to commit sidecar:" << sidePath;
			return false;
		}
		return true;
	}

	bool writeCropSidecar(const QString& outImagePath, const QString& sourceImagePath, const QJsonObject& parameters)
	{
		QJsonObject meta;
		meta.insert(QStringLiteral("schema_version"), 1);
		meta.insert(QStringLiteral("derived_from"), sourceImagePath);
		meta.insert(QStringLiteral("operation"), QStringLiteral("crop"));
		meta.insert(QStringLiteral("parameters"), parameters);
		QJsonObject tool;
		tool.insert(QStringLiteral("name"), QStringLiteral("CTAnalyzerX"));
		tool.insert(QStringLiteral("version"), QStringLiteral("unknown"));
		meta.insert(QStringLiteral("tool"), tool);
		meta.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
		return writeJsonSidecar(outImagePath, meta);
	}

} // namespace JsonUtils
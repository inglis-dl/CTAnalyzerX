#include "JsonUtils.h"

#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <QStandardPaths>
#include <QRegularExpression>
#include "JsonSettings.h"

namespace JsonUtils
{

	// Helper: read application "version" value from JSON settings (returns "unknown" on failure)
	static QString readAppVersion()
	{
		const QString settingsPath = JsonSettings::defaultSettingsPath();
		QFile f(settingsPath);
		if (!f.open(QIODevice::ReadOnly))
			return QStringLiteral("unknown");
		const QByteArray b = f.readAll();
		f.close();
		const QJsonDocument doc = QJsonDocument::fromJson(b);
		if (!doc.isObject()) return QStringLiteral("unknown");
		QJsonObject root = doc.object();
		// settings.json stores application/version under "application" group
		if (root.contains(QStringLiteral("application")) && root.value(QStringLiteral("application")).isObject()) {
			QJsonObject app = root.value(QStringLiteral("application")).toObject();
			const QString ver = app.value(QStringLiteral("version")).toString();
			if (!ver.isEmpty()) return ver;
		}
		return QStringLiteral("unknown");
	}

	// Compute canonical project JSON path stored in the user's application data area.
	// Store files in "<AppData>/projects/<sourceBase>.json" (no hashing).
	QString sidecarPathForImage(const QString& imagePath)
	{
		QFileInfo fi(imagePath);

		// Use application-specific appdata location (Qt chooses appropriate platform folder).
		QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
		if (appData.isEmpty()) {
			// Fallback to home directory if AppDataLocation cannot be determined.
			appData = QDir::homePath();
		}

		// Ensure a dedicated subfolder for projects (was "sidecars")
		QDir d(appData);
		const QString projectsDir = d.filePath(QStringLiteral("projects"));
		QDir().mkpath(projectsDir);

		// If this appears to be a crop-derived filename produced by CropExporter::makeAutoOutputPath
		// (pattern "<base>_crop_<xdim>x<ydim>x<zdim>"), derive the original base name and use that for the
		// project filename so derived outputs map back to their source project file.
		const QString base = fi.completeBaseName();
		static const QRegularExpression cropRegex(R"((.*)_crop_(\d+)x(\d+)x(\d+))", QRegularExpression::CaseInsensitiveOption);
		QRegularExpressionMatch match = cropRegex.match(base);
		QString projectBase;
		if (match.hasMatch() && match.captured(1).length() > 0) {
			projectBase = match.captured(1);
		}
		else {
			projectBase = base;
		}

		const QString fname = QStringLiteral("%1.json").arg(projectBase);
		return QDir(projectsDir).filePath(fname);
	}

	// Read canonical project JSON (no legacy fallbacks). Returns empty object when not present or invalid.
	QJsonObject readJsonSidecar(const QString& imagePath)
	{
		QJsonObject empty;
		const QString sidePath = sidecarPathForImage(imagePath);
		QFile f(sidePath);
		if (!f.open(QIODevice::ReadOnly)) return empty;
		const QByteArray b = f.readAll();
		f.close();
		const QJsonDocument doc = QJsonDocument::fromJson(b);
		if (!doc.isObject()) return empty;
		return doc.object();
	}

	bool writeJsonSidecar(const QString& imagePath, const QJsonObject& meta)
	{
		const QString sidePath = sidecarPathForImage(imagePath);

		// Prepare output meta; ensure a one-time "tool" header is present (name + version).
		QJsonObject outMeta = meta;

		// Always ensure 'tool' header exists (single header stored in the project file).
		if (!outMeta.contains(QStringLiteral("tool"))) {
			QJsonObject tool;
			tool.insert(QStringLiteral("name"), QStringLiteral("CTAnalyzerX"));
			tool.insert(QStringLiteral("version"), readAppVersion());
			outMeta.insert(QStringLiteral("tool"), tool);
		}

		// Ensure we always provide an "operations" array (empty allowed)
		if (!outMeta.contains(QStringLiteral("operations")) || !outMeta.value(QStringLiteral("operations")).isArray()) {
			outMeta.insert(QStringLiteral("operations"), QJsonArray());
		}

		QSaveFile f(sidePath);
		if (!f.open(QIODevice::WriteOnly)) {
			qWarning() << "JsonUtils: failed to open project file for write:" << sidePath;
			return false;
		}
		const QJsonDocument doc(outMeta);
		f.write(doc.toJson(QJsonDocument::Indented));
		if (!f.commit()) {
			qWarning() << "JsonUtils: failed to commit project file:" << sidePath;
			return false;
		}
		return true;
	}

	// Create a canonical crop sidecar that uses the new "operations" array model.
	bool writeCropSidecar(const QString& outImagePath, const QString& sourceImagePath, const QJsonObject& parameters)
	{
		// Top-level meta
		QJsonObject meta;
		meta.insert(QStringLiteral("source"), sourceImagePath);
		meta.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

		// Operation object describing the completed crop
		QJsonObject op;
		op.insert(QStringLiteral("name"), QStringLiteral("crop"));
		op.insert(QStringLiteral("status"), QStringLiteral("completed"));
		op.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
		op.insert(QStringLiteral("derived"), outImagePath);
		op.insert(QStringLiteral("parameters"), parameters);

		QJsonArray ops;
		ops.append(op);
		meta.insert(QStringLiteral("operations"), ops);

		// Persist canonical project JSON (JsonUtils::writeJsonSidecar will ensure 'tool' is present)
		return writeJsonSidecar(outImagePath, meta);
	}

} // namespace JsonUtils
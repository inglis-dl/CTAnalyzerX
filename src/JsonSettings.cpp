/*=========================================================================

  Program:
  Module:   JsonSettings.cxx
  Language: C++

  Author: Dean Inglis <inglis DOT dl AT gmail DOT com>

=========================================================================*/
#include "JsonSettings.h"
#include <QStandardPaths>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

const QSettings::Format JsonSettings::JsonFormat = QSettings::registerFormat("JsonFormat", &JsonSettings::readSettingsJson, &JsonSettings::writeSettingsJson);

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
void JsonSettings::parseJsonObject(QJsonObject& json, QString prefix, QVariantMap& map)
{
	QJsonValue value;
	QJsonObject obj;

	QStringList keys = json.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		value = json.value(keys[i]);
		if (value.isObject())
		{
			obj = value.toObject();
			parseJsonObject(obj, prefix + keys[i] + "/", map);
		}
		else
		{
			map.insert(prefix + keys[i], value.toVariant());
		}
	}
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
bool JsonSettings::readSettingsJson(QIODevice& device, QVariantMap& map)
{
	QJsonParseError jsonError;
	QJsonObject obj = QJsonDocument::fromJson(device.readAll(), &jsonError).object();
	if (jsonError.error != QJsonParseError::NoError)
	{
		qCritical() << "ERROR: failed to read json from IO device:" << jsonError.errorString();
		return false;
	}
	parseJsonObject(obj, QString(), map);
	return true;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
bool JsonSettings::writeSettingsJson(QIODevice& device, const QVariantMap& map)
{
	QVariantMap tmp_map = map;
	QJsonObject buffer = restoreJsonObject(tmp_map);
	device.write(QJsonDocument(buffer).toJson());
	return true;
}

// -+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-+#+-
QString JsonSettings::defaultSettingsPath() {
	QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
	QDir().mkpath(dir);
	return dir + QDir::separator() + "settings.json";
}

// helper to insert nested value
static void insertValueIntoObject(QJsonObject& obj, const QStringList& parts, const QJsonValue& val)
{
	if (parts.isEmpty()) return;
	const QString key = parts.first();
	if (parts.size() == 1) {
		obj.insert(key, val);
		return;
	}
	QJsonObject child = obj.value(key).toObject();
	QStringList rest = parts.mid(1);
	insertValueIntoObject(child, rest, val);
	obj.insert(key, child);
}

QJsonObject JsonSettings::restoreJsonObject(const QVariantMap& map)
{
	QJsonObject root;
	const QStringList keys = map.keys();
	for (const QString& flatKey : keys) {
		QVariant v = map.value(flatKey);
		QJsonValue jv = QJsonValue::fromVariant(v);
		const QStringList parts = flatKey.split('/', Qt::SkipEmptyParts);
		if (parts.isEmpty()) continue;
		insertValueIntoObject(root, parts, jv);
	}
	return root;
}

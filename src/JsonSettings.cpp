/*=========================================================================

  Program:
  Module:   JsonSettings.cxx
  Language: C++

  Author: Dean Inglis <inglis DOT dl AT gmail DOT com>

=========================================================================*/
#include "JsonSettings.h"
#include <QStandardPaths>
#include <QDir>

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
QJsonObject JsonSettings::restoreJsonObject(QVariantMap& map)
{
	QJsonObject obj;
	QStringList keys = map.keys();

	foreach(const auto key, keys)
	{
		QVariant value = map.value(key);
		QStringList sections = key.split('/');
		if (sections.size() > 1)
		{
			continue;
		}
		else
		{
			map.remove(key);
			obj.insert(key, QJsonValue::fromVariant(value));
		}
	}

	QList<QVariantMap> subMaps;
	keys = map.keys();
	for (int i = 0; i < keys.size(); i++)
	{
		bool found = false;
		QString key = keys[i];

		for (int j = 0; j < subMaps.size(); j++)
		{
			QString subKey = subMaps[j].key(QString("__key__"));
			if (subKey.contains(key.section('/', 0, 0)))
			{
				subMaps[j].insert(key.section('/', 1), map.value(key));
				found = true;
				break;
			}
		}

		if (!found)
		{
			QVariantMap tmp;
			tmp.insert(key.section('/', 0, 0), QString("__key__"));
			tmp.insert(key.section('/', 1), map.value(key));
			subMaps.append(tmp);
		}
	}

	for (int i = 0; i < subMaps.size(); i++)
	{
		QString key = subMaps[i].key(QString("__key__"));
		subMaps[i].remove(key);

		QJsonObject tmp = restoreJsonObject(subMaps[i]);
		obj.insert(key, tmp);
	}
	return obj;
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

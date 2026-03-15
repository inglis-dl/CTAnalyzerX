/*=========================================================================

  Program:
  Module:   JsonSettings.h
  Language: C++

  Author: Dean Inglis <inglis DOT dl AT gmail DOT com>

=========================================================================*/
#ifndef __JsonSettings_h
#define __JsonSettings_h

#include <QtCore>
#include <QSettings>

class JsonSettings
{
public:
	static bool readSettingsJson(QIODevice& device, QVariantMap& map);
	static bool writeSettingsJson(QIODevice& device, const QVariantMap& map);

	static const QSettings::Format JsonFormat;
	static QString defaultSettingsPath();

private:
	static void parseJsonObject(QJsonObject& json, QString prefix, QVariantMap& map);
	static QJsonObject restoreJsonObject(const QVariantMap& map);
};

#endif // __JsonSettings_h

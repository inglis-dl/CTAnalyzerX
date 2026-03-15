#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>

class ImageFrameWidget;

class WindowLevelBus : public QObject
{
	Q_OBJECT
public:
	static WindowLevelBus& instance();

	void registerFrame(ImageFrameWidget* frame);
	void unregisterFrame(ImageFrameWidget* frame);

	// Broadcast native-domain WL from sender to peers with the same image
	void broadcast(ImageFrameWidget* sender, double window, double level);

private:
	explicit WindowLevelBus(QObject* parent = nullptr);
	QVector<QPointer<ImageFrameWidget>> m_frames;
};

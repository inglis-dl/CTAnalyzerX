#include "WindowLevelBus.h"
#include "ImageFrameWidget.h"

#include <algorithm>

WindowLevelBus& WindowLevelBus::instance()
{
	static WindowLevelBus inst;
	return inst;
}

WindowLevelBus::WindowLevelBus(QObject* parent)
	: QObject(parent)
{
}

void WindowLevelBus::registerFrame(ImageFrameWidget* frame)
{
	if (!frame) return;

	// Remove dead entries
	for (int i = m_frames.size() - 1; i >= 0; --i) {
		if (m_frames[i].isNull()) {
			m_frames.remove(i);
		}
	}

	// Avoid duplicates
	for (const auto& p : m_frames) {
		if (p == frame) {
			return;
		}
	}
	m_frames.push_back(QPointer<ImageFrameWidget>(frame));
}

void WindowLevelBus::unregisterFrame(ImageFrameWidget* frame)
{
	for (int i = m_frames.size() - 1; i >= 0; --i) {
		if (m_frames[i].isNull() || m_frames[i] == frame) {
			m_frames.remove(i);
		}
	}
}

void WindowLevelBus::broadcast(ImageFrameWidget* sender, double window, double level)
{
	if (!sender) return;
	if (sender->linkPropagationMode() == ImageFrameWidget::Disabled) return;

	// Collect participants to render: always include sender
	QVector<ImageFrameWidget*> participants;
	participants.reserve(m_frames.size() + 1);
	participants.push_back(sender);

	// Forward WL to peers viewing the same image, honoring propagation mode
	for (int i = 0; i < m_frames.size();) {
		if (m_frames[i].isNull()) {
			m_frames.remove(i);
			continue;
		}

		ImageFrameWidget* target = m_frames[i];
		++i;

		if (!target || target == sender) continue;

		// Skip if either has propagation disabled
		if (target->linkPropagationMode() == ImageFrameWidget::Disabled) continue;

		// Must share the same image
		if (sender->imageData() == nullptr || target->imageData() == nullptr) continue;
		if (sender->imageData() != target->imageData()) continue;

		// Apply without re-broadcast
		target->setApplyingLinkedWindowLevel(true);
		target->applyLinkedWindowLevel(sender, window, level);
		target->setApplyingLinkedWindowLevel(false);

		participants.push_back(target);
	}

	// Keep participants visually in sync
	for (ImageFrameWidget* f : participants) {
		if (f) f->render();
	}
}
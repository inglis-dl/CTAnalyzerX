#include "WindowLevelBridge.h"

#include "VolumeView.h"
#include "SliceView.h"

WindowLevelBridge::WindowLevelBridge(VolumeView* volumeView, SliceView* sliceView, QObject* parent)
	: QObject(parent)
	, m_volumeView(volumeView)
	, m_sliceView(sliceView)
	, m_lastWindow(std::numeric_limits<double>::quiet_NaN())
	, m_lastLevel(std::numeric_limits<double>::quiet_NaN())
{
	// no automatic connections here; callers (MainWindow) will hook signals as required
}

void WindowLevelBridge::onWindowLevelChanged(double window, double level)
{
	// Avoid re-applying identical WL (prevents echo loops when signals propagate)
	if (std::isfinite(m_lastWindow) && std::isfinite(m_lastLevel) &&
		window == m_lastWindow && level == m_lastLevel) {
		return;
	}

	// Apply to volume view (native-domain)
	if (m_volumeView) {
		m_volumeView->setColorWindowLevel(window, level);
	}

	// Also apply to slice view(s) if present (keeps slices consistent when controller driven)
	if (m_sliceView) {
		m_sliceView->setWindowLevelNative(window, level);
	}

	m_lastWindow = window;
	m_lastLevel = level;
}

void WindowLevelBridge::onWindowLevelFromSlice(double window, double level)
{
	// Slice-driven WL: update volume only (volume will emit windowLevelChanged -> controller sync)
	// Avoid re-applying identical WL (prevents echo loops)
	if (std::isfinite(m_lastWindow) && std::isfinite(m_lastLevel) &&
		window == m_lastWindow && level == m_lastLevel) {
		return;
	}

	if (m_volumeView) {
		// Keep volume rendering consistent (drives controller sync)...
		m_volumeView->setColorWindowLevel(window, level);
		// ...and also update orthogonal image-slice actors so the 3D slice planes match the 2D views.
		m_volumeView->setSliceWindowLevelNative(window, level);
	}

	m_lastWindow = window;
	m_lastLevel = level;
}
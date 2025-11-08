#pragma once
#include <QObject>
#include <limits>

class VolumeView;
class SliceView;

class WindowLevelBridge : public QObject
{
	Q_OBJECT
public:
	explicit WindowLevelBridge(VolumeView* volumeView, SliceView* sliceView = nullptr, QObject* parent = nullptr);

public Q_SLOTS:
	// Called by WindowLevelController (interactive or committed)
	void onWindowLevelChanged(double window, double level);

	// Called when a SliceView changes WL via VTK interaction.
	// Applies WL to the volume view only to drive volume -> controller propagation.
	void onWindowLevelFromSlice(double window, double level);

private:
	VolumeView* m_volumeView = nullptr;
	SliceView* m_sliceView = nullptr;

	// Track last-applied WL to avoid redundant application / feedback loops
	double m_lastWindow = std::numeric_limits<double>::quiet_NaN();
	double m_lastLevel = std::numeric_limits<double>::quiet_NaN();
};

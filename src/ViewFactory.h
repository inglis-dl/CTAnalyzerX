#ifndef VIEWFACTORY_H
#define VIEWFACTORY_H

#include "SliceView.h"
#include "VolumeView.h"

class ViewFactory {
public:
	// Fix: Use the correct enum type for orientation.
	static SliceView* createSliceView(SliceView::ViewOrientation orientation, QWidget* parent = nullptr);
	static VolumeView* createVolumeView(QWidget* parent = nullptr);
};

#endif // VIEWFACTORY_H

#include "ViewFactory.h"

// Fix: Use SliceView::ViewOrientation instead of SliceView::Orientation
SliceView* ViewFactory::createSliceView(SliceView::ViewOrientation orientation, QWidget* parent) {
	return new SliceView(parent, orientation);
}

VolumeView* ViewFactory::createVolumeView(QWidget* parent) {
	return new VolumeView(parent);
}

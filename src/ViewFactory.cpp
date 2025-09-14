
#include "ViewFactory.h"

SliceView* ViewFactory::createSliceView(SliceView::Orientation orientation, QWidget* parent) {
	return new SliceView(parent, orientation);
}

VolumeView* ViewFactory::createVolumeView(QWidget* parent) {
	return new VolumeView(parent);
}

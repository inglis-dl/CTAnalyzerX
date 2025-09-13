
#include "ViewFactory.h"

/*
QVTKOpenGLNativeWidget* ViewFactory::createVolumeView(vtkSmartPointer<vtkImageData> imageData) {
	auto widget = new QVTKOpenGLNativeWidget;
	auto view = new VolumeView(imageData);
	widget->GetRenderWindow()->AddRenderer(view->getRenderer());
	return widget;
}

QVTKOpenGLNativeWidget* ViewFactory::createSliceView(vtkSmartPointer<vtkImageData> imageData, SliceOrientation orientation) {
	auto widget = new QVTKOpenGLNativeWidget;
	auto view = new SliceView(imageData, orientation);
	widget->GetRenderWindow()->AddRenderer(view->getRenderer());
	return widget;
}

*/

SliceView* ViewFactory::createSliceView(SliceView::Orientation orientation, QWidget* parent) {
	return new SliceView(orientation, parent);
}

VolumeView* ViewFactory::createVolumeView(QWidget* parent) {
	return new VolumeView(parent);
}

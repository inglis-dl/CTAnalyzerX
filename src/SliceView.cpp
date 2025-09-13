#include "SliceView.h"
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <QWheelEvent>


SliceView::SliceView(Orientation orientation, QWidget* parent)
	: QVTKOpenGLNativeWidget(parent), orientation(orientation), currentSlice(0), maxSlice(0) {

	viewer = vtkSmartPointer<vtkImageViewer2>::New();
	setRenderWindow(viewer->GetRenderWindow());
}

void SliceView::setImageData(vtkImageData* image) {
	viewer->SetInputData(image);
	switch (orientation) {
		case Axial: viewer->SetSliceOrientationToXY(); break;
		case Sagittal: viewer->SetSliceOrientationToYZ(); break;
		case Coronal: viewer->SetSliceOrientationToXZ(); break;
	}
	maxSlice = viewer->GetSliceMax();
	setSliceIndex(maxSlice / 2); // start in the middle
}

void SliceView::setSliceIndex(int index) {
	currentSlice = std::clamp(index, 0, maxSlice);
	updateSlice();
	emit sliceChanged(currentSlice);
}

int SliceView::getSliceIndex() const {
	return currentSlice;
}

void SliceView::updateSlice() {
	viewer->SetSlice(currentSlice);
	viewer->Render();
}

void SliceView::wheelEvent(QWheelEvent* event) {
	int delta = event->angleDelta().y() > 0 ? 1 : -1;
	setSliceIndex(currentSlice + delta);
}

vtkRenderWindow* SliceView::GetRenderWindow() const {
	return QVTKOpenGLNativeWidget::renderWindow();
}

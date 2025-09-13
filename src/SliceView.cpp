#include "SliceView.h"
#include <QWheelEvent>

#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkImageViewer2.h>
#include <vtkImageData.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkGenericOpenGLRenderWindow.h>

SliceView::SliceView(Orientation orientation, QWidget* parent)
	: QVTKOpenGLNativeWidget(parent), orientation(orientation), currentSlice(0), maxSlice(0) {

	renderer = vtkSmartPointer<vtkRenderer>::New();
	renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
	renderWindow->AddRenderer(renderer);
	setRenderWindow(renderWindow);

	viewer = vtkSmartPointer<vtkImageViewer2>::New();
	viewer->SetRenderWindow(renderWindow);
	viewer->SetRenderer(renderer);
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

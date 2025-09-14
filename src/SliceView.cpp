#include "SliceView.h"
#include <QWheelEvent>
#include <QSlider>

#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkImageViewer2.h>
#include <vtkImageData.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkInteractorStyleImage.h>
#include <vtkCamera.h>

#include <algorithm> // for std::clamp

SliceView::SliceView(QWidget* parent, Orientation orientation)
	: QFrame(parent), orientation(orientation), currentSlice(0), maxSlice(0) {

	ui.setupUi(this);

	renderer = vtkSmartPointer<vtkRenderer>::New();
	renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
	viewer = vtkSmartPointer<vtkImageViewer2>::New();

	renderWindow->AddRenderer(renderer);

	ui.renderArea->setRenderWindow(renderWindow);

	viewer->SetRenderWindow(renderWindow);
	viewer->SetRenderer(renderer);

	// Connect slider to setSliceIndex slot
	connect(ui.sliderSlicePosition, &QSlider::valueChanged, this, &SliceView::setSliceIndex);

	// Connect sliceChanged signal to update label
	connect(this, &SliceView::sliceChanged, this, [this](int value) {
		ui.labelSliceInfo->setText(QString("Slice: %1").arg(value));
	});
}

void SliceView::setImageData(vtkImageData* image) {
	if (!image) return;

	viewer->SetInputData(image);
	switch (orientation) {
		case Axial: viewer->SetSliceOrientationToXY(); break;
		case Sagittal: viewer->SetSliceOrientationToYZ(); break;
		case Coronal: viewer->SetSliceOrientationToXZ(); break;
	}
	maxSlice = viewer->GetSliceMax();

	// Update slider range based on image dimensions
	ui.sliderSlicePosition->setMinimum(0);
	ui.sliderSlicePosition->setMaximum(maxSlice);

	setSliceIndex(maxSlice / 2); // start in the middle
}

void SliceView::setSliceIndex(int index) {
	int clampedIndex = std::clamp(index, 0, maxSlice);
	if (currentSlice != clampedIndex) {
		currentSlice = clampedIndex;
		ui.sliderSlicePosition->blockSignals(true);
		ui.sliderSlicePosition->setValue(currentSlice);
		ui.sliderSlicePosition->blockSignals(false);
		updateSlice();
		emit sliceChanged(currentSlice);
	}
	else {
		// Always update label even if value didn't change (for direct slider move)
		emit sliceChanged(currentSlice);
	}
}

int SliceView::getSliceIndex() const {
	return currentSlice;
}

void SliceView::updateSlice() {
	viewer->SetSlice(currentSlice);
	resetCameraForOrientation();
	viewer->Render();
}

void SliceView::setAxialOrientation() {
	setOrientation(Axial);
}

void SliceView::setSagittalOrientation() {
	setOrientation(Sagittal);
}

void SliceView::setCoronalOrientation() {
	setOrientation(Coronal);
}

void SliceView::wheelEvent(QWheelEvent* event) {
	int delta = event->angleDelta().y() > 0 ? 1 : -1;
	setSliceIndex(currentSlice + delta);
}

vtkRenderWindow* SliceView::GetRenderWindow() const {
	return renderWindow;
}

SliceView::Orientation SliceView::getOrientation() const {
	return orientation;
}

void SliceView::setOrientation(Orientation newOrientation) {
	if (orientation != newOrientation) {
		orientation = newOrientation;

		switch (orientation) {
			case Axial:
			viewer->SetSliceOrientation(vtkImageViewer2::SLICE_ORIENTATION_XY);
			break;
			case Sagittal:
			viewer->SetSliceOrientation(vtkImageViewer2::SLICE_ORIENTATION_YZ);
			break;
			case Coronal:
			viewer->SetSliceOrientation(vtkImageViewer2::SLICE_ORIENTATION_XZ);
			break;
		}

		updateSlice();
	}
}

void SliceView::resetCameraForOrientation() {
	auto cam = renderer->GetActiveCamera();
	if (!cam) return;

	switch (orientation) {
		case Axial: // XY
		cam->SetFocalPoint(0, 0, 0);
		cam->SetPosition(0, 0, 1);
		cam->SetViewUp(0, 1, 0);
		break;
		case Sagittal: // YZ
		cam->SetFocalPoint(0, 0, 0);
		cam->SetPosition(1, 0, 0);
		cam->SetViewUp(0, 0, 1);
		break;
		case Coronal: // XZ
		cam->SetFocalPoint(0, 0, 0);
		cam->SetPosition(0, -1, 0);
		cam->SetViewUp(0, 0, 1);
		break;
	}
	cam->OrthogonalizeViewUp();
	renderer->ResetCamera();
}
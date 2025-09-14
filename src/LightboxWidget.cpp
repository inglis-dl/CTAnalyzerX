#include "LightBoxWidget.h"
#include "SliceView.h"
#include "VolumeView.h"
#include <vtkImageSinusoidSource.h>
#include <vtkSmartPointer.h>
#include <cmath>

LightBoxWidget::LightBoxWidget(QWidget* parent)
	: QWidget(parent)
{
	ui.setupUi(this);

	setDefaultImage();
	connectSliceSynchronization();
}

void LightBoxWidget::setDefaultImage() {
	auto sinusoid = vtkSmartPointer<vtkImageSinusoidSource>::New();
	sinusoid->SetPeriod(32);
	sinusoid->SetPhase(0);
	sinusoid->SetAmplitude(255);
	sinusoid->SetWholeExtent(0, 127, 0, 127, 0, 31);
	sinusoid->SetDirection(0.5, -0.5, 1.0 / std::sqrt(2.0));
	sinusoid->Update();

	vtkImageData* defaultImage = sinusoid->GetOutput();
	ui.axialView->setImageData(defaultImage);
	ui.axialView->setAxialOrientation();

	ui.sagittalView->setImageData(defaultImage);
	ui.sagittalView->setSagittalOrientation();

	ui.coronalView->setImageData(defaultImage);
	ui.coronalView->setCoronalOrientation();

	ui.volumeView->setImageData(defaultImage);
}

void LightBoxWidget::setImageData(vtkImageData* image) {
	ui.axialView->setImageData(image);
	ui.sagittalView->setImageData(image);
	ui.coronalView->setImageData(image);
	ui.volumeView->setImageData(image);
}

void LightBoxWidget::setAxialSlice(int index) {
	ui.axialView->setSliceIndex(index);
}

void LightBoxWidget::setSagittalSlice(int index) {
	ui.sagittalView->setSliceIndex(index);
}

void LightBoxWidget::setCoronalSlice(int index) {
	ui.coronalView->setSliceIndex(index);
}

QPixmap LightBoxWidget::grabFramebuffer() {
	return this->grab();
}

SliceView* LightBoxWidget::getAxialView() const { return ui.axialView; }
SliceView* LightBoxWidget::getSagittalView() const { return ui.sagittalView; }
SliceView* LightBoxWidget::getCoronalView() const { return ui.coronalView; }
VolumeView* LightBoxWidget::getVolumeView() const { return ui.volumeView; }

void LightBoxWidget::connectSliceSynchronization() {
	connect(ui.axialView, &SliceView::sliceChanged, this, [this](int index) {
		ui.volumeView->updateSlicePlanes(index, ui.sagittalView->getSliceIndex(), ui.coronalView->getSliceIndex());
	});
	connect(ui.sagittalView, &SliceView::sliceChanged, this, [this](int index) {
		ui.volumeView->updateSlicePlanes(ui.axialView->getSliceIndex(), index, ui.coronalView->getSliceIndex());
	});
	connect(ui.coronalView, &SliceView::sliceChanged, this, [this](int index) {
		ui.volumeView->updateSlicePlanes(ui.axialView->getSliceIndex(), ui.sagittalView->getSliceIndex(), index);
	});
}


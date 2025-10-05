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

	ui.YZView->setOrientationToYZ();
	ui.XZView->setOrientationToXZ();
	ui.XYView->setOrientationToXY();

	setDefaultImage();
	connectSliceSynchronization();
}

void LightBoxWidget::setDefaultImage() {
	auto sinusoid = vtkSmartPointer<vtkImageSinusoidSource>::New();
	sinusoid->SetPeriod(32);
	sinusoid->SetPhase(0);
	sinusoid->SetAmplitude(255);
	sinusoid->SetWholeExtent(0, 63, 0, 127, 0, 31);
	sinusoid->SetDirection(0.5, -0.5, 1.0 / std::sqrt(2.0));
	sinusoid->Update();

	vtkImageData* defaultImage = sinusoid->GetOutput();
	setImageData(defaultImage);
}

void LightBoxWidget::setImageData(vtkImageData* image) {
	ui.YZView->setImageData(image);
	ui.XZView->setImageData(image);
	ui.XYView->setImageData(image);
	ui.volumeView->setImageData(image);
}

void LightBoxWidget::setYZSlice(int index) {
	ui.YZView->setSliceIndex(index);
}

void LightBoxWidget::setXZSlice(int index) {
	ui.XZView->setSliceIndex(index);
}

void LightBoxWidget::setXYSlice(int index) {
	ui.XYView->setSliceIndex(index);
}

QPixmap LightBoxWidget::grabFramebuffer() {
	return this->grab();
}

SliceView* LightBoxWidget::getYZView() const { return ui.YZView; }
SliceView* LightBoxWidget::getXZView() const { return ui.XZView; }
SliceView* LightBoxWidget::getXYView() const { return ui.XYView; }
VolumeView* LightBoxWidget::getVolumeView() const { return ui.volumeView; }

void LightBoxWidget::connectSliceSynchronization() {
	connect(ui.YZView, &SliceView::sliceChanged, this, [this](int index) {
		ui.volumeView->updateSlicePlanes(index, ui.XZView->getSliceIndex(), ui.XYView->getSliceIndex());
	});
	connect(ui.XZView, &SliceView::sliceChanged, this, [this](int index) {
		ui.volumeView->updateSlicePlanes(ui.YZView->getSliceIndex(), index, ui.XYView->getSliceIndex());
	});
	connect(ui.XYView, &SliceView::sliceChanged, this, [this](int index) {
		ui.volumeView->updateSlicePlanes(ui.YZView->getSliceIndex(), ui.XZView->getSliceIndex(), index);
	});
}


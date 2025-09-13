#include "LightBoxWidget.h"
#include "LightboxWidget.h"
#include "ViewFactory.h"
#include <QGridLayout>
#include "ViewFactory.h"

LightBoxWidget::LightBoxWidget(QWidget* parent)
	: QWidget(parent) {
	layout = new QGridLayout(this);
	setupViews();
}

void LightBoxWidget::setImageData(vtkImageData* image) {
	axialView->setImageData(image);
	sagittalView->setImageData(image);
	coronalView->setImageData(image);
	volumeView->setImageData(image);
}

void LightBoxWidget::connectSliceSynchronization() {

	connect(axialView, &SliceView::sliceChanged, this, [=](int index) {
		axialLabel->setText(QString("Axial: %1").arg(index));
		volumeView->updateSlicePlanes(index, sagittalView->getSliceIndex(), coronalView->getSliceIndex());
});

	connect(sagittalView, &SliceView::sliceChanged, this, [=](int index) {
		sagittalLabel->setText(QString("Sagittal: %1").arg(index));
		volumeView->updateSlicePlanes(axialView->getSliceIndex(), index, coronalView->getSliceIndex());
	});

	connect(coronalView, &SliceView::sliceChanged, this, [=](int index) {
		coronalLabel->setText(QString("Coronal: %1").arg(index));
		volumeView->updateSlicePlanes(axialView->getSliceIndex(), sagittalView->getSliceIndex(), index);
	});
}

void LightBoxWidget::setupViews() {

	vtkSmartPointer<vtkImageData> dummyImage = vtkSmartPointer<vtkImageData>::New();
	axialView = ViewFactory::createSliceView(SliceView::Axial, this);
	sagittalView = ViewFactory::createSliceView(SliceView::Sagittal, this);
	coronalView = ViewFactory::createSliceView(SliceView::Coronal, this);
	volumeView = ViewFactory::createVolumeView(this);

	layout->addWidget(axialView, 0, 0);
	layout->addWidget(sagittalView, 0, 1);
	layout->addWidget(coronalView, 1, 0);
	layout->addWidget(volumeView, 1, 1);
	layout->addWidget(createCroppingControls(), 2, 0, 1, 2);

	auto labelLayout = new QHBoxLayout();
	axialLabel = new QLabel("Axial: 0");
	sagittalLabel = new QLabel("Sagittal: 0");
	coronalLabel = new QLabel("Coronal: 0");

	labelLayout->addWidget(axialLabel);
	labelLayout->addWidget(sagittalLabel);
	labelLayout->addWidget(coronalLabel);
	layout->addLayout(labelLayout, 3, 0, 1, 2);

	connectSliceSynchronization();

	shortcutFullVolume = new QShortcut(QKeySequence("Ctrl+1"), this);
	shortcutCenterROI = new QShortcut(QKeySequence("Ctrl+2"), this);
	shortcutResetROI = new QShortcut(QKeySequence("Ctrl+3"), this);

	connect(shortcutFullVolume, &QShortcut::activated, presetFullVolume, &QPushButton::click);
	connect(shortcutCenterROI, &QShortcut::activated, presetCenterROI, &QPushButton::click);
	connect(shortcutResetROI, &QShortcut::activated, presetReset, &QPushButton::click);

	auto shortcutTogglePlanes = new QShortcut(QKeySequence("Ctrl+P"), this);
	connect(shortcutTogglePlanes, &QShortcut::activated, this, [=]() {
		bool current = slicePlaneToggle->isChecked();
		slicePlaneToggle->setChecked(!current);
	});

	shortcutHelpLabel = new QLabel("ⓘ Shortcut Help");
	shortcutHelpLabel->setToolTip(
		"Keyboard Shortcuts:\n"
		"Ctrl+1 → Full Volume\n"
		"Ctrl+2 → Center ROI\n"
		"Ctrl+3 → Reset to slice-centered ROI\n"
		"Ctrl+P → Toggle slice planes"
	);
	shortcutHelpLabel->setStyleSheet("QLabel { color: #555; font-style: italic; }");

	layout->addWidget(shortcutHelpLabel, 4, 0, 1, 2, Qt::AlignLeft);

	menuBar = new QMenuBar(this);
	auto croppingMenu = menuBar->addMenu("Cropping");
	auto viewMenu = menuBar->addMenu("View");

	actionFullVolume = new QAction("Full Volume (Ctrl+1)", this);
	actionCenterROI = new QAction("Center ROI (Ctrl+2)", this);
	actionResetROI = new QAction("Reset ROI (Ctrl+3)", this);
	actionToggleSlicePlanes = new QAction("Toggle Slice Planes (Ctrl+P)", this);

	actionFullVolume->setShortcut(QKeySequence("Ctrl+1"));
	actionCenterROI->setShortcut(QKeySequence("Ctrl+2"));
	actionResetROI->setShortcut(QKeySequence("Ctrl+3"));
	actionToggleSlicePlanes->setShortcut(QKeySequence("Ctrl+P"));

	croppingMenu->addAction(actionFullVolume);
	croppingMenu->addAction(actionCenterROI);
	croppingMenu->addAction(actionResetROI);
	viewMenu->addAction(actionToggleSlicePlanes);

	layout->setMenuBar(menuBar);

	connect(actionFullVolume, &QAction::triggered, presetFullVolume, &QPushButton::click);
	connect(actionCenterROI, &QAction::triggered, presetCenterROI, &QPushButton::click);
	connect(actionResetROI, &QAction::triggered, presetReset, &QPushButton::click);

	connect(actionToggleSlicePlanes, &QAction::triggered, this, [=]() {
		bool current = slicePlaneToggle->isChecked();
		slicePlaneToggle->setChecked(!current);
	});
}

QGroupBox* LightBoxWidget::createCroppingControls() {
	auto group = new QGroupBox("Volume Cropping", this);
	auto vbox = new QVBoxLayout(group);

	axialMinSlider = new QSlider(Qt::Horizontal);
	axialMaxSlider = new QSlider(Qt::Horizontal);
	sagittalMinSlider = new QSlider(Qt::Horizontal);
	sagittalMaxSlider = new QSlider(Qt::Horizontal);
	coronalMinSlider = new QSlider(Qt::Horizontal);
	coronalMaxSlider = new QSlider(Qt::Horizontal);

	axialMinSlider->setRange(0, 255);
	axialMaxSlider->setRange(0, 255);
	sagittalMinSlider->setRange(0, 255);
	sagittalMaxSlider->setRange(0, 255);
	coronalMinSlider->setRange(0, 255);
	coronalMaxSlider->setRange(0, 255);

	axialMinSlider->setValue(0);
	axialMaxSlider->setValue(255);
	sagittalMinSlider->setValue(0);
	sagittalMaxSlider->setValue(255);
	coronalMinSlider->setValue(0);
	coronalMaxSlider->setValue(255);

	vbox->addWidget(new QLabel("Axial Min"));
	vbox->addWidget(axialMinSlider);
	vbox->addWidget(new QLabel("Axial Max"));
	vbox->addWidget(axialMaxSlider);

	vbox->addWidget(new QLabel("Sagittal Min"));
	vbox->addWidget(sagittalMinSlider);
	vbox->addWidget(new QLabel("Sagittal Max"));
	vbox->addWidget(sagittalMaxSlider);

	vbox->addWidget(new QLabel("Coronal Min"));
	vbox->addWidget(coronalMinSlider);
	vbox->addWidget(new QLabel("Coronal Max"));
	vbox->addWidget(coronalMaxSlider);

	presetFullVolume = new QPushButton("Full Volume");
	presetCenterROI = new QPushButton("Center ROI");
	presetReset = new QPushButton("Reset");

	vbox->addWidget(presetFullVolume);
	vbox->addWidget(presetCenterROI);
	vbox->addWidget(presetReset);

	connect(presetFullVolume, &QPushButton::clicked, this, [=]() {
		axialMinSlider->setValue(0);
		axialMaxSlider->setValue(255);
		sagittalMinSlider->setValue(0);
		sagittalMaxSlider->setValue(255);
		coronalMinSlider->setValue(0);
		coronalMaxSlider->setValue(255);
	});

	connect(presetCenterROI, &QPushButton::clicked, this, [=]() {
		axialMinSlider->setValue(80);
		axialMaxSlider->setValue(180);
		sagittalMinSlider->setValue(60);
		sagittalMaxSlider->setValue(160);
		coronalMinSlider->setValue(70);
		coronalMaxSlider->setValue(170);
	});

	connect(presetReset, &QPushButton::clicked, this, [=]() {
		axialMinSlider->setValue(axialView->getSliceIndex() - 20);
		axialMaxSlider->setValue(axialView->getSliceIndex() + 20);
		sagittalMinSlider->setValue(sagittalView->getSliceIndex() - 20);
		sagittalMaxSlider->setValue(sagittalView->getSliceIndex() + 20);
		coronalMinSlider->setValue(coronalView->getSliceIndex() - 20);
		coronalMaxSlider->setValue(coronalView->getSliceIndex() + 20);
	});

	slicePlaneToggle = new QCheckBox("Show Slice Planes");
	slicePlaneToggle->setChecked(false);
	vbox->addWidget(slicePlaneToggle);

	connect(slicePlaneToggle, &QCheckBox::toggled, this, [=](bool checked) {
		volumeView->toggleSlicePlanes(checked);
	});

	connectCroppingControls();

	return group;
}

void LightBoxWidget::connectCroppingControls() {
	auto connectSlider = [this](QSlider* slider) {
		connect(slider, &QSlider::valueChanged, this, &LightBoxWidget::updateCropping);
		};

	connectSlider(this->axialMinSlider);
	connectSlider(this->axialMaxSlider);
	connectSlider(this->sagittalMinSlider);
	connectSlider(this->sagittalMaxSlider);
	connectSlider(this->coronalMinSlider);
	connectSlider(this->coronalMaxSlider);
}

void LightBoxWidget::updateCropping() {
	volumeView->setCroppingRegion(
		axialMinSlider->value(), axialMaxSlider->value(),
		sagittalMinSlider->value(), sagittalMaxSlider->value(),
		coronalMinSlider->value(), coronalMaxSlider->value()
	);
}

void LightBoxWidget::setAxialSlice(int index)
{
	if (axialView) {
		axialView->setSliceIndex(index);
		axialLabel->setText(QString("Axial: %1").arg(index));
		volumeView->updateSlicePlanes(coronalView->getSliceIndex(), sagittalView->getSliceIndex(), index);
	}
}


void LightBoxWidget::setSagittalSlice(int index)
{
	if (sagittalView) {
		sagittalView->setSliceIndex(index);
		sagittalLabel->setText(QString("Sagittal: %1").arg(index));
		volumeView->updateSlicePlanes(axialView->getSliceIndex(), index, coronalView->getSliceIndex());
	}
}

void LightBoxWidget::setCoronalSlice(int index)
{
	if (coronalView) {
		coronalView->setSliceIndex(index);
		coronalLabel->setText(QString("Coronal: %1").arg(index));
		volumeView->updateSlicePlanes(axialView->getSliceIndex(), sagittalView->getSliceIndex(), index);
	}
}

QPixmap LightBoxWidget::grabFramebuffer() {
	return this->grab();
}


#include "LightboxWidget.h"
#include "SliceView.h"
#include "VolumeView.h"
#include "SelectionFrameWidget.h"

#include <vtkImageSinusoidSource.h>
#include <vtkSmartPointer.h>

#include <QShowEvent>
#include <QTimer>
#include <array>
#include <cmath>

LightboxWidget::LightboxWidget(QWidget* parent)
	: QWidget(parent)
{
	ui.setupUi(this);

	ui.YZView->setViewOrientation(ImageFrameWidget::VIEW_ORIENTATION_YZ);
	ui.XZView->setViewOrientation(ImageFrameWidget::VIEW_ORIENTATION_XZ);
	ui.XYView->setViewOrientation(ImageFrameWidget::VIEW_ORIENTATION_XY);

	// Defer default image until after the widget is realized/context ready
	QTimer::singleShot(0, this, [this]() { setDefaultImage(); });

	connectSliceSynchronization();
	connectSelectionCoordination();

	// Wire maximize/restore now that UI children exist
	connectMaximizeSignals();

	// Optional: choose a default selected/highlighted view
	if (ui.XYView) ui.XYView->setSelected(true);
}

void LightboxWidget::showEvent(QShowEvent* e)
{
	QWidget::showEvent(e);
	// Ensure connections exist even if UI was re-created
	connectMaximizeSignals();
}

void LightboxWidget::setDefaultImage() {
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

void LightboxWidget::setImageData(vtkImageData* image) {
	ui.YZView->setImageData(image);
	ui.XZView->setImageData(image);
	ui.XYView->setImageData(image);
	ui.volumeView->setImageData(image);
}

void LightboxWidget::setYZSlice(int index) {
	ui.YZView->setSliceIndex(index);
}

void LightboxWidget::setXZSlice(int index) {
	ui.XZView->setSliceIndex(index);
}

void LightboxWidget::setXYSlice(int index) {
	ui.XYView->setSliceIndex(index);
}

QPixmap LightboxWidget::grabFramebuffer() {
	return this->grab();
}

SliceView* LightboxWidget::getYZView() const { return ui.YZView; }
SliceView* LightboxWidget::getXZView() const { return ui.XZView; }
SliceView* LightboxWidget::getXYView() const { return ui.XYView; }
VolumeView* LightboxWidget::getVolumeView() const { return ui.volumeView; }

void LightboxWidget::connectSliceSynchronization() {
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

void LightboxWidget::connectSelectionCoordination() {
	using SF = SelectionFrameWidget;
	std::array<SF*, 3> views{ { ui.YZView, ui.XZView, ui.XYView } };

	for (SF* v : views) {
		connect(v, &SF::selectedChanged, this, [this, v](bool on) {
			if (!on) return;

			// Unselect all other views so only one title bar is highlighted
			std::array<SF*, 3> others{ { ui.YZView, ui.XZView, ui.XYView } };
			for (SF* o : others) {
				if (o && o != v) {
					o->setSelected(false);
				}
			}
			// Ensure focus follows selection regardless of source (title bar or menu button)
			if (v) {
				v->setFocus(Qt::OtherFocusReason);
			}
		});
	}
}

// New: wire up maximize/restore signals from all frames
void LightboxWidget::connectMaximizeSignals()
{
	auto connectOne = [this](SelectionFrameWidget* w) {
		if (!w) return;
		connect(w, SIGNAL(requestMaximize(SelectionFrameWidget*)),
				this, SLOT(onRequestMaximize(SelectionFrameWidget*)),
				Qt::UniqueConnection);
		connect(w, SIGNAL(requestRestore(SelectionFrameWidget*)),
				this, SLOT(onRequestRestore(SelectionFrameWidget*)),
				Qt::UniqueConnection);
		};

	connectOne(ui.YZView);
	connectOne(ui.XZView);
	connectOne(ui.XYView);
	connectOne(ui.volumeView);
}

// Maximize one child: hide all siblings so the chosen one fills the layout
void LightboxWidget::onRequestMaximize(SelectionFrameWidget* w)
{
	if (!w) return;
	if (m_maximized && m_maximized != w) m_maximized->setMaximized(false);

	const std::array<SelectionFrameWidget*, 4> frames{ { ui.YZView, ui.XZView, ui.XYView, ui.volumeView } };
	for (auto* f : frames) {
		if (!f) continue;
		f->setVisible(f == w);
		if (f == w) f->setMaximized(true);
	}
	m_isMaximized = true;
	m_maximized = w;
}

// Restore all children: show all frames again
void LightboxWidget::onRequestRestore(SelectionFrameWidget*)
{
	const std::array<SelectionFrameWidget*, 4> frames{ { ui.YZView, ui.XZView, ui.XYView, ui.volumeView } };
	for (auto* f : frames) {
		if (!f) continue;
		f->setVisible(true);
		f->setMaximized(false);
	}
	m_isMaximized = false;
	m_maximized = nullptr;
}


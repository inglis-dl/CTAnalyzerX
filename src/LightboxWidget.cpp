#include "LightboxWidget.h"
#include "SliceView.h"
#include "VolumeView.h"
#include "SelectionFrameWidget.h"

#include <vtkImageSinusoidSource.h>
#include <vtkSmartPointer.h>

#include <QShowEvent>
#include <QTimer>
#include <QLabel>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QParallelAnimationGroup>   // ADDED
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

// Utility: map a child frame geometry into this widget's coordinate system
QRect LightboxWidget::mapToThis(SelectionFrameWidget* w) const
{
	if (!w) return {};
	const QPoint topLeft = w->mapTo(const_cast<LightboxWidget*>(this), QPoint(0, 0));
	return QRect(topLeft, w->size());
}

// Create and run a geometry-based expansion/collapse overlay animation
void LightboxWidget::startExpandAnimation(SelectionFrameWidget* target, const QRect& /*from*/, const QRect& /*to*/, bool toMaximized)
{
	// New implementation: animate ALL frames simultaneously using per-frame overlays.
	clearAnimOverlay();

	const std::array<SelectionFrameWidget*, 4> frames{ { ui.YZView, ui.XZView, ui.XYView, ui.volumeView } };

	// Build/save start rects on maximize so we can restore later.
	if (toMaximized) {
		m_savedRects.clear();
		for (auto* f : frames) {
			if (!f) continue;
			m_savedRects.insert(f, mapToThis(f));
		}
	}

	// If we don't have saved rects (unexpected), synthesize current ones.
	if (m_savedRects.isEmpty()) {
		for (auto* f : frames) {
			if (!f) continue;
			m_savedRects.insert(f, mapToThis(f));
		}
	}

	// Create overlays and parallel animations
	m_animGroup = new QParallelAnimationGroup(this);
	m_animOverlays.clear();
	m_animOverlays.reserve(int(frames.size()));

	// Hide all real frames during the animation to avoid duplicates
	for (auto* f : frames) {
		if (f) f->setVisible(false);
	}

	const QRect fullRect = this->rect();

	for (auto* f : frames) {
		if (!f) continue;

		// Overlay setup
		auto* overlay = new QLabel(this);
		overlay->setObjectName("MaximizeAnimOverlay");
		overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
		overlay->setScaledContents(true);

		QPixmap pm = f->grab();
		if (!pm.isNull()) {
			overlay->setPixmap(pm);
		}
		else {
			overlay->setStyleSheet("background: palette(window); border: 1px solid palette(dark);");
		}

		const QRect startRect = toMaximized ? m_savedRects.value(f)                                   // current layout -> maximize
			: (f == target ? fullRect
						   : QRect(m_savedRects.value(f).center(), QSize(0, 0))); // restore

		overlay->setGeometry(startRect);
		overlay->show();

		// Target/end rects
		QRect endRect;
		if (toMaximized) {
			endRect = (f == target) ? fullRect
				: QRect(m_savedRects.value(f).center(), QSize(0, 0)); // shrink others into their centers
		}
		else {
			endRect = m_savedRects.value(f); // restore everyone to original rectangles
		}

		// Animation
		auto* anim = new QPropertyAnimation(overlay, "geometry", m_animGroup);
		anim->setDuration(200);
		anim->setStartValue(startRect);
		anim->setEndValue(endRect);
		anim->setEasingCurve(QEasingCurve::InOutCubic);
		m_animGroup->addAnimation(anim);

		m_animOverlays.push_back(overlay);
	}

	connect(m_animGroup, &QParallelAnimationGroup::finished, this, [this, target, toMaximized, frames]() {
		clearAnimOverlay();

		// Apply final visibility/state
		if (toMaximized) {
			for (auto* f : frames) {
				if (!f) continue;
				const bool isTarget = (f == target);
				f->setVisible(isTarget);
				f->setMaximized(isTarget);
			}
			m_isMaximized = true;
			m_maximized = target;
			// m_savedRects kept for restore
		}
		else {
			for (auto* f : frames) {
				if (!f) continue;
				f->setVisible(true);
				f->setMaximized(false);
			}
			m_isMaximized = false;
			m_maximized = nullptr;
			m_savedRects.clear();
		}
	});

	m_animGroup->start(QAbstractAnimation::DeleteWhenStopped);
}

// Remove any existing overlay/animation
void LightboxWidget::clearAnimOverlay()
{
	// Stop and delete any running parallel group
	if (m_animGroup) {
		m_animGroup->stop();
		m_animGroup->deleteLater();
		m_animGroup = nullptr;
	}

	// Remove all per-frame overlays
	for (auto* lbl : m_animOverlays) {
		if (!lbl) continue;
		lbl->hide();
		lbl->deleteLater();
	}
	m_animOverlays.clear();

	// Back-compat cleanup (legacy single overlay if any)
	if (m_anim) { m_anim->stop(); m_anim = nullptr; }
	if (m_animOverlay) { m_animOverlay->hide(); m_animOverlay->deleteLater(); m_animOverlay = nullptr; }
}

// ADD: Implement the missing slots so moc can link them.

void LightboxWidget::onRequestMaximize(SelectionFrameWidget* w)
{
	if (!w) return;
	if (m_maximized == w && m_isMaximized) return;

	// The new startExpandAnimation collects current rects and animates all frames in parallel.
	startExpandAnimation(w, QRect(), QRect(), /*toMaximized*/ true);
}

void LightboxWidget::onRequestRestore(SelectionFrameWidget* w)
{
	Q_UNUSED(w);

	// If nothing maximized, just ensure all frames are visible.
	if (!m_isMaximized || !m_maximized) {
		const std::array<SelectionFrameWidget*, 4> frames{ { ui.YZView, ui.XZView, ui.XYView, ui.volumeView } };
		for (auto* f : frames) {
			if (!f) continue;
			f->setVisible(true);
			f->setMaximized(false);
		}
		m_isMaximized = false;
		m_maximized = nullptr;
		m_savedRects.clear();
		return;
	}

	// Animate restore for all frames simultaneously.
	startExpandAnimation(m_maximized, QRect(), QRect(), /*toMaximized*/ false);
}


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
void LightboxWidget::startExpandAnimation(SelectionFrameWidget* target, const QRect& from, const QRect& to, bool toMaximized)
{
	// Create overlay
	clearAnimOverlay();
	m_animOverlay = new QLabel(this);
	m_animOverlay->setObjectName("MaximizeAnimOverlay");
	m_animOverlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
	m_animOverlay->setScaledContents(true);

	// Try to snapshot the target (works for non-native content). Fallback to flat color.
	QPixmap pm = target ? target->grab() : QPixmap();
	if (!pm.isNull()) {
		m_animOverlay->setPixmap(pm);
	}
	else {
		m_animOverlay->setStyleSheet("background: palette(window); border: 1px solid palette(dark);");
	}

	m_animOverlay->setGeometry(from);
	m_animOverlay->show();
	m_anim = new QPropertyAnimation(m_animOverlay, "geometry", this);
	m_anim->setDuration(200);
	m_anim->setStartValue(from);
	m_anim->setEndValue(to);
	m_anim->setEasingCurve(QEasingCurve::InOutCubic);

	// Temporarily hide target to avoid duplicate during animation
	if (target) target->setVisible(false);

	connect(m_anim, &QPropertyAnimation::finished, this, [this, target, toMaximized]() {
		clearAnimOverlay();

		// Apply final visibility state
		const std::array<SelectionFrameWidget*, 4> frames{ { ui.YZView, ui.XZView, ui.XYView, ui.volumeView } };
		if (toMaximized) {
			for (auto* f : frames) {
				if (!f) continue;
				f->setVisible(f == target);
				f->setMaximized(f == target);
			}
			m_isMaximized = true;
			m_maximized = target;
		}
		else {
			for (auto* f : frames) {
				if (!f) continue;
				f->setVisible(true);
				f->setMaximized(false);
			}
			m_isMaximized = false;
			m_maximized = nullptr;
		}
	});

	m_anim->start(QAbstractAnimation::DeleteWhenStopped);
}

// Remove any existing overlay/animation
void LightboxWidget::clearAnimOverlay()
{
	if (m_anim) { m_anim->stop(); m_anim = nullptr; }
	if (m_animOverlay) { m_animOverlay->hide(); m_animOverlay->deleteLater(); m_animOverlay = nullptr; }
}

// Maximize one child: expand from its current rect to fill the Lightbox area.
// The perceived expansion direction depends on the frame's quadrant:
// TL -> down-right, TR -> down-left, BL -> up-right, BR -> up-left.
void LightboxWidget::onRequestMaximize(SelectionFrameWidget* w)
{
	if (!w) return;
	if (m_maximized == w && m_isMaximized) return;

	// Save original geometry of target for animated restore later
	m_savedTargetRect = mapToThis(w);

	// Prepare target/others visibility before anim (keep everyone visible so overlay sits on top)
	const QRect from = m_savedTargetRect;
	const QRect to = this->rect();

	startExpandAnimation(w, from, to, /*toMaximized*/ true);
}

// Restore all children: collapse the maximized frame back to its original rect.
void LightboxWidget::onRequestRestore(SelectionFrameWidget* /*w*/)
{
	if (!m_isMaximized || !m_maximized) {
		// Nothing maximized: just ensure all visible
		const std::array<SelectionFrameWidget*, 4> frames{ { ui.YZView, ui.XZView, ui.XYView, ui.volumeView } };
		for (auto* f : frames) if (f) f->setVisible(true);
		m_isMaximized = false;
		m_maximized = nullptr;
		return;
	}

	// Current full rect is the Lightbox area; collapse back to saved rect of target.
	const QRect from = this->rect();
	const QRect to = m_savedTargetRect;
	startExpandAnimation(m_maximized, from, to, /*toMaximized*/ false);
}


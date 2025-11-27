#include "LightboxWidget.h"
#include "SliceView.h"
#include "VolumeView.h"
#include "SelectionFrameWidget.h"
#include "WindowLevelController.h"
#include "WindowLevelBridge.h"

#include <vtkImageSinusoidSource.h>
#include <vtkSmartPointer.h>
#include <vtkImageProperty.h>

#include <QShowEvent>
#include <QTimer>
#include <QLabel>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QParallelAnimationGroup>
#include <array>
#include <cmath>
#include <QColor>

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>

#include <QGridLayout>
#include <QLayoutItem>


namespace {
	void swapWidgets(QGridLayout* grid, QWidget* a, QWidget* b)
	{
		if (!grid || !a || !b || a == b)
			return;

		int aRow = -1, aCol = -1, aRowSpan = 1, aColSpan = 1;
		int bRow = -1, bCol = -1, bRowSpan = 1, bColSpan = 1;
		QLayoutItem* aItem = nullptr;
		QLayoutItem* bItem = nullptr;

		// Find positions of a and b in the grid
		for (int i = 0; i < grid->count(); ++i) {
			int row, col, rowSpan, colSpan;
			QLayoutItem* item = grid->itemAt(i);
			QWidget* w = item ? item->widget() : nullptr;
			grid->getItemPosition(i, &row, &col, &rowSpan, &colSpan);
			if (w == a) {
				aRow = row; aCol = col; aRowSpan = rowSpan; aColSpan = colSpan; aItem = item;
			}
			if (w == b) {
				bRow = row; bCol = col; bRowSpan = rowSpan; bColSpan = colSpan; bItem = item;
			}
		}

		if (!aItem || !bItem)
			return;

		// Remove both widgets from the layout
		grid->removeWidget(a);
		grid->removeWidget(b);

		// Add them back at swapped positions
		grid->addWidget(a, bRow, bCol, bRowSpan, bColSpan);
		grid->addWidget(b, aRow, aCol, aRowSpan, aColSpan);
	}
} // namespace


LightboxWidget::LightboxWidget(QWidget* parent)
	: QWidget(parent)
{
	ui.setupUi(this);

	// Allow drag/drop reordering
	this->setAcceptDrops(true);

	// Safety: UI may be created from Designer; guard null children where appropriate.
	if (ui.YZView) ui.YZView->setViewOrientation(ImageFrameWidget::VIEW_ORIENTATION_YZ);
	if (ui.XZView) ui.XZView->setViewOrientation(ImageFrameWidget::VIEW_ORIENTATION_XZ);
	if (ui.XYView) ui.XYView->setViewOrientation(ImageFrameWidget::VIEW_ORIENTATION_XY);

	// Defer default image until after the widget is realized/context ready
	QTimer::singleShot(0, this, [this]() { setDefaultImage(); });

	connectSliceSynchronization();
	connectSelectionCoordination();

	// Wire maximize/restore now that UI children exist
	connectMaximizeSignals();

	// Optional: choose a default selected/highlighted view
	if (ui.XYView) ui.XYView->setSelected(true);

	// Minimal, safe encapsulation:
	// create WindowLevelController and WindowLevelBridge here, parented to this widget.
	// MainWindow will only take the controller widget to insert into its layout.
	if (!m_wlController) {
		m_wlController = new WindowLevelController(this);
	}
	if (!m_wlBridge) {
		// Bridge targets the volume view; slice-to-bridge connections are wired below.
		m_wlBridge = new WindowLevelBridge(getVolumeView(), nullptr, this);
	}

	// Connect controller -> local propagator (so controller updates slices + volume)
	connect(m_wlController, &WindowLevelController::windowLevelChanged, this, [this](double w, double l) {
		if (m_propagatingWindowLevel) return;
		m_propagatingWindowLevel = true;

		// Apply to volume via bridge (native domain)
		if (m_wlBridge) m_wlBridge->onWindowLevelChanged(w, l);

		// Also apply to all slice views (mapped via each slice's mapping)
		if (auto* yz = getYZView()) yz->setWindowLevelNative(w, l);
		if (auto* xz = getXZView()) xz->setWindowLevelNative(w, l);
		if (auto* xy = getXYView()) xy->setWindowLevelNative(w, l);

		m_propagatingWindowLevel = false;
	}, Qt::UniqueConnection);

	connect(m_wlController, &WindowLevelController::windowLevelCommitted, this, [this](double w, double l) {
		if (m_propagatingWindowLevel) return;
		m_propagatingWindowLevel = true;

		// call the bridge's changed handler directly (committed semantics treated same)
		if (m_wlBridge) m_wlBridge->onWindowLevelChanged(w, l);

		if (auto* yz = getYZView()) yz->setWindowLevelNative(w, l);
		if (auto* xz = getXZView()) xz->setWindowLevelNative(w, l);
		if (auto* xy = getXYView()) xy->setWindowLevelNative(w, l);

		m_propagatingWindowLevel = false;
	}, Qt::UniqueConnection);

	// Keep controller UI in sync when the active VolumeView emits windowLevelChanged
	if (auto* vol = getVolumeView()) {
		connect(vol, &VolumeView::windowLevelChanged, this, [this](double w, double l) {
			// When a VolumeView resets WL (e.g. user pressed 'r' in Volume mode)
			// we must propagate that reset to the controller, the bridge (volume),
			// and all slice views so they stay synchronized.
			if (m_propagatingWindowLevel) return;
			m_propagatingWindowLevel = true;

			// Update controller UI
			if (m_wlController) {
				m_wlController->setWindow(w);
				m_wlController->setLevel(l);
			}

			// Notify bridge (volume side) and all slices (native-domain mapping per slice)
			if (m_wlBridge) m_wlBridge->onWindowLevelChanged(w, l);
			if (auto* yz = getYZView()) yz->setWindowLevelNative(w, l);
			if (auto* xz = getXZView()) xz->setWindowLevelNative(w, l);
			if (auto* xy = getXYView()) xy->setWindowLevelNative(w, l);

			m_propagatingWindowLevel = false;
		}, Qt::UniqueConnection);
	}

	// Hook slice -> local propagator so slice-driven WL updates siblings + volume
	if (auto* yz = getYZView()) {
		connect(yz, &SliceView::windowLevelChanged, this, [this, yz](double w, double l) {
			if (m_propagatingWindowLevel) return;
			m_propagatingWindowLevel = true;

			// Update volume via bridge
			if (m_wlBridge) m_wlBridge->onWindowLevelFromSlice(w, l);

			// Update controller UI so spinboxes reflect interactive slice changes
			if (m_wlController) {
				m_wlController->setWindow(w);
				m_wlController->setLevel(l);
			}

			// Update other slices (slaves)
			if (auto* xz = getXZView()) { if (xz != yz) xz->setWindowLevelNative(w, l); }
			if (auto* xy = getXYView()) { if (xy != yz) xy->setWindowLevelNative(w, l); }

			m_propagatingWindowLevel = false;
		}, Qt::UniqueConnection);
	}
	if (auto* xz = getXZView()) {
		connect(xz, &SliceView::windowLevelChanged, this, [this, xz](double w, double l) {
			if (m_propagatingWindowLevel) return;
			m_propagatingWindowLevel = true;

			if (m_wlBridge) m_wlBridge->onWindowLevelFromSlice(w, l);

			if (m_wlController) {
				m_wlController->setWindow(w);
				m_wlController->setLevel(l);
			}

			if (auto* yz = getYZView()) { if (yz != xz) yz->setWindowLevelNative(w, l); }
			if (auto* xy = getXYView()) { if (xy != xz) xy->setWindowLevelNative(w, l); }

			m_propagatingWindowLevel = false;
		}, Qt::UniqueConnection);
	}
	if (auto* xy = getXYView()) {
		connect(xy, &SliceView::windowLevelChanged, this, [this, xy](double w, double l) {
			if (m_propagatingWindowLevel) return;
			m_propagatingWindowLevel = true;

			if (m_wlBridge) m_wlBridge->onWindowLevelFromSlice(w, l);

			if (m_wlController) {
				m_wlController->setWindow(w);
				m_wlController->setLevel(l);
			}

			if (auto* yz = getYZView()) { if (yz != xy) yz->setWindowLevelNative(w, l); }
			if (auto* xz = getXZView()) { if (xz != xy) xz->setWindowLevelNative(w, l); }

			m_propagatingWindowLevel = false;
		}, Qt::UniqueConnection);
	}

	// Wire controller reset request to propagate to our child views
	connect(m_wlController, &WindowLevelController::requestResetWindowLevel,
			this, &LightboxWidget::resetWindowLevel, Qt::UniqueConnection);

	if (auto* yz = getYZView()) {
		connect(yz, &SliceView::requestResetWindowLevel, this, &LightboxWidget::resetWindowLevel, Qt::UniqueConnection);
	}
	if (auto* xz = getXZView()) {
		connect(xz, &SliceView::requestResetWindowLevel, this, &LightboxWidget::resetWindowLevel, Qt::UniqueConnection);
	}
	if (auto* xy = getXYView()) {
		connect(xy, &SliceView::requestResetWindowLevel, this, &LightboxWidget::resetWindowLevel, Qt::UniqueConnection);
	}
}

void LightboxWidget::showEvent(QShowEvent* e)
{
	QWidget::showEvent(e);
	// Ensure connections exist even if UI was re-created
	connectMaximizeSignals();
}

void LightboxWidget::setDefaultImage()
{
	// Provide a simple textured default image when no input is available.
	auto sinusoid = vtkSmartPointer<vtkImageSinusoidSource>::New();
	sinusoid->SetPeriod(32);
	sinusoid->SetPhase(0);
	sinusoid->SetAmplitude(255);
	sinusoid->SetWholeExtent(0, 63, 0, 127, 0, 31);
	sinusoid->SetDirection(0.5, -0.5, 1.0 / std::sqrt(2.0));
	// Ensure source has produced concrete vtkImageData so we can pass it as direct input.
	sinusoid->Update();

	// Prefer passing concrete image data (SetInputData path) to child views so
	// ImageFrameWidget::setImageData() can install it via SetInputData on the
	// internal shift/scale filter. This avoids calling Update() on filters that
	// have no upstream connections (prevents "input port 0 has 0 connections" errors).
	vtkImageData* img = vtkImageData::SafeDownCast(sinusoid->GetOutput());
	if (img) {
		// Pass the produced image directly to views (safe, immediate metadata available).
		setImageData(img);
	}
	else {
		// Fallback: attach as a pipeline connection if output is not a concrete image.
		setInputConnection(sinusoid->GetOutputPort(), true);
	}
}

void LightboxWidget::setImageData(vtkImageData* image)
{
	// Forward to child views if they exist
	if (ui.YZView) ui.YZView->setImageData(image);
	if (ui.XZView) ui.XZView->setImageData(image);
	if (ui.XYView) ui.XYView->setImageData(image);
	if (ui.volumeView) ui.volumeView->setImageData(image);

	// Re-install shared image property to ensure all slices use the same vtkImageProperty
	// (setImageData above may have created per-view imageProperty instances).
	if (m_sharedImageProperty) {
		if (ui.YZView) ui.YZView->setSharedImageProperty(m_sharedImageProperty);
		if (ui.XZView) ui.XZView->setSharedImageProperty(m_sharedImageProperty);
		if (ui.XYView) ui.XYView->setSharedImageProperty(m_sharedImageProperty);
	}
}

void LightboxWidget::setYZSlice(int index)
{
	if (ui.YZView) ui.YZView->setSliceIndex(index);
}

void LightboxWidget::setXZSlice(int index)
{
	if (ui.XZView) ui.XZView->setSliceIndex(index);
}

void LightboxWidget::setXYSlice(int index)
{
	if (ui.XYView) ui.XYView->setSliceIndex(index);
}

QPixmap LightboxWidget::grabFramebuffer()
{
	return this->grab();
}

SliceView* LightboxWidget::getYZView() const { return ui.YZView; }
SliceView* LightboxWidget::getXZView() const { return ui.XZView; }
SliceView* LightboxWidget::getXYView() const { return ui.XYView; }
VolumeView* LightboxWidget::getVolumeView() const { return ui.volumeView; }

void LightboxWidget::connectSliceSynchronization()
{
	if (!ui.YZView || !ui.XZView || !ui.XYView || !ui.volumeView) return;

	connect(ui.YZView, &SliceView::sliceChanged, this, [this](int index) {
		if (ui.volumeView) ui.volumeView->updateSlicePlanes(index, ui.XZView->getSliceIndex(), ui.XYView->getSliceIndex());
	});
	connect(ui.XZView, &SliceView::sliceChanged, this, [this](int index) {
		if (ui.volumeView) ui.volumeView->updateSlicePlanes(ui.YZView->getSliceIndex(), index, ui.XYView->getSliceIndex());
	});
	connect(ui.XYView, &SliceView::sliceChanged, this, [this](int index) {
		if (ui.volumeView) ui.volumeView->updateSlicePlanes(ui.YZView->getSliceIndex(), ui.XZView->getSliceIndex(), index);
	});
}

void LightboxWidget::connectSelectionCoordination()
{
	using SF = SelectionFrameWidget;
	std::array<SF*, 3> views{ { ui.YZView, ui.XZView, ui.XYView } };

	for (SF* v : views) {
		if (!v) continue;
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
		}, Qt::UniqueConnection);
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
		anim->setEasingCurve(QEasingCurve::Linear);
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

void LightboxWidget::resetWindowLevel()
{
	if (m_propagatingWindowLevel) return;
	m_propagatingWindowLevel = true;

	if (auto* yz = getYZView()) yz->resetWindowLevel();
	if (auto* xz = getXZView()) xz->resetWindowLevel();
	if (auto* xy = getXYView()) xy->resetWindowLevel();
	if (auto* vol = getVolumeView()) vol->resetWindowLevel();

	m_propagatingWindowLevel = false;
}

void LightboxWidget::setInputConnection(vtkAlgorithmOutput* port, bool newImg)
{
	// Forward to child views if they exist
	if (ui.YZView) ui.YZView->setInputConnection(port, newImg);
	if (ui.XZView) ui.XZView->setInputConnection(port, newImg);
	if (ui.XYView) ui.XYView->setInputConnection(port, newImg);
	if (ui.volumeView) ui.volumeView->setInputConnection(port, newImg);

	// Ensure shared property is re-applied after new pipeline/input is connected.
	if (m_sharedImageProperty) {
		if (ui.YZView) ui.YZView->setSharedImageProperty(m_sharedImageProperty);
		if (ui.XZView) ui.XZView->setSharedImageProperty(m_sharedImageProperty);
		if (ui.XYView) ui.XYView->setSharedImageProperty(m_sharedImageProperty);
	}
}

// drag/drop handlers ------------------------------------------------------

void LightboxWidget::dragEnterEvent(QDragEnterEvent* e)
{
	if (e->mimeData() && e->mimeData()->hasFormat("application/x-selectionframe")) {
		e->acceptProposedAction();
	}
	else {
		e->ignore();
	}
}

void LightboxWidget::dragMoveEvent(QDragMoveEvent* e)
{
	if (e->mimeData() && e->mimeData()->hasFormat("application/x-selectionframe")) {
		e->acceptProposedAction();
	}
	else {
		e->ignore();
	}
}

void LightboxWidget::dropEvent(QDropEvent* e)
{
	if (!e->mimeData() || !e->mimeData()->hasFormat("application/x-selectionframe")) {
		e->ignore();
		return;
	}

	const QByteArray ba = e->mimeData()->data("application/x-selectionframe");
	const QString num = QString::fromUtf8(ba);
	bool ok = false;
	const quint64 v = num.toULongLong(&ok);
	if (!ok) { e->ignore(); return; }

	SelectionFrameWidget* src = reinterpret_cast<SelectionFrameWidget*>(static_cast<quintptr>(v));
	if (!src) { e->ignore(); return; }

	// Find target frame under drop position
	QWidget* child = childAt(e->pos());
	SelectionFrameWidget* target = nullptr;
	for (QWidget* w = child; w; w = w->parentWidget()) {
		if (auto* sf = qobject_cast<SelectionFrameWidget*>(w)) { target = sf; break; }
	}

	if (!target || target == src) { e->ignore(); return; }

	// Swap in layout if possible
	QGridLayout* grid = qobject_cast<QGridLayout*>(this->layout());
	if (grid) {
		swapWidgets(grid, src, target);
	}
	else {
		// fallback: exchange geometries
		QRect aGeo = src->geometry();
		QRect bGeo = target->geometry();
		src->setGeometry(bGeo);
		target->setGeometry(aGeo);
		src->update();
		target->update();
	}

	// keep selection/focus and update menus for both frames
	auto setMenuFor = [&](SelectionFrameWidget* w) {
		if (!w) return;
		if (auto* sv = qobject_cast<SliceView*>(w)) {
			switch (sv->viewOrientation()) {
				case ImageFrameWidget::VIEW_ORIENTATION_XY: w->setCurrentItem(QStringLiteral("XY")); break;
				case ImageFrameWidget::VIEW_ORIENTATION_XZ: w->setCurrentItem(QStringLiteral("XZ")); break;
				case ImageFrameWidget::VIEW_ORIENTATION_YZ: w->setCurrentItem(QStringLiteral("YZ")); break;
				default: break;
			}
		}
		else if (auto* vv = qobject_cast<VolumeView*>(w)) {
			w->setCurrentItem(vv->orthoPlanesVisible() ? QStringLiteral("OrthoPlanes") : QStringLiteral("Volume"));
		}
		};

	target->setSelected(true);
	src->setSelected(false);
	target->setFocus(Qt::OtherFocusReason);

	setMenuFor(target);
	setMenuFor(src);

	e->acceptProposedAction();
}

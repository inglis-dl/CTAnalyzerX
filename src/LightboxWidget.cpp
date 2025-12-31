#include "LightboxWidget.h"
#include "VolumeView.h"
#include "SliceView.h"
#include "SelectionFrameWidget.h"
#include "WindowLevelController.h"
#include "WindowLevelBridge.h"

#include <vtkAlgorithmOutput.h>
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
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>


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

	// Forward image extents from any child frame to LightboxWidget so it can mediate updates.
	auto forwardExtentsFrom = [this](ImageFrameWidget* v) {
		if (!v) return;
		// connect derived-class emission to LightboxWidget forwarding signal
		connect(v, &ImageFrameWidget::imageExtentsChanged,
				this, &LightboxWidget::imageExtentsChanged, Qt::UniqueConnection);
		};

	forwardExtentsFrom(ui.YZView);
	forwardExtentsFrom(ui.XZView);
	forwardExtentsFrom(ui.XYView);
	forwardExtentsFrom(ui.volumeView);

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

	// Create the WindowLevelBridge now (bridge is owned by Lightbox)
	if (!m_wlBridge) {
		m_wlBridge = new WindowLevelBridge(getVolumeView(), nullptr, this);
	}

	// NOTE:
	// Do NOT create a WindowLevelController here. The controller may be provided by
	// the WorkflowPanelWidget UI and registered via setWindowLevelController().
	// All controller-related wiring is performed in setWindowLevelController().
	// This avoids duplicate controllers and duplicate connections.
}

// New: install an externally-owned WindowLevelController instance (Lightbox does not take ownership)
void LightboxWidget::setWindowLevelController(WindowLevelController* ctrl)
{
	// If the controller is identical, nothing to do.
	if (m_wlController == ctrl) return;

	// Disconnect and unwire previous controller if any
	if (m_wlController) {
		disconnect(m_wlController, nullptr, this, nullptr);
		// Do not delete the controller; its lifetime is owned by the WorkflowPanelWidget (caller).
		m_wlController = nullptr;
	}

	// Adopt new controller reference (Lightbox does NOT take ownership)
	m_wlController = ctrl;
	if (!m_wlController) return;

	// Ensure we have a bridge (bridge remains parented to this Lightbox)
	if (!m_wlBridge) {
		m_wlBridge = new WindowLevelBridge(getVolumeView(), nullptr, this);
	}

	// Controller -> Lightbox propagation (apply to volume via bridge and to all slice views)
	connect(m_wlController, &WindowLevelController::windowLevelChanged, this, [this](double w, double l) {
		if (m_propagatingWindowLevel) return;
		m_propagatingWindowLevel = true;

		if (m_wlBridge) m_wlBridge->onWindowLevelChanged(w, l);

		if (auto* yz = getYZView()) yz->setWindowLevelNative(w, l);
		if (auto* xz = getXZView()) xz->setWindowLevelNative(w, l);
		if (auto* xy = getXYView()) xy->setWindowLevelNative(w, l);

		m_propagatingWindowLevel = false;
	}, Qt::UniqueConnection);

	connect(m_wlController, &WindowLevelController::windowLevelCommitted, this, [this](double w, double l) {
		if (m_propagatingWindowLevel) return;
		m_propagatingWindowLevel = true;

		if (m_wlBridge) m_wlBridge->onWindowLevelChanged(w, l);

		if (auto* yz = getYZView()) yz->setWindowLevelNative(w, l);
		if (auto* xz = getXZView()) xz->setWindowLevelNative(w, l);
		if (auto* xy = getXYView()) xy->setWindowLevelNative(w, l);

		m_propagatingWindowLevel = false;
	}, Qt::UniqueConnection);

	// Controller reset -> Lightbox reset propagation
	connect(m_wlController, &WindowLevelController::requestResetWindowLevel,
			this, &LightboxWidget::resetWindowLevel, Qt::UniqueConnection);

	// Volume -> controller update (keep controller UI synchronized if the volume changes WL)
	if (auto* vol = getVolumeView()) {
		connect(vol, &VolumeView::windowLevelChanged, this, [this](double w, double l) {
			if (m_propagatingWindowLevel) return;
			m_propagatingWindowLevel = true;

			if (m_wlController) {
				m_wlController->setWindow(w);
				m_wlController->setLevel(l);
			}
			if (m_wlBridge) m_wlBridge->onWindowLevelChanged(w, l);
			if (auto* yz = getYZView()) yz->setWindowLevelNative(w, l);
			if (auto* xz = getXZView()) xz->setWindowLevelNative(w, l);
			if (auto* xy = getXYView()) xy->setWindowLevelNative(w, l);

			m_propagatingWindowLevel = false;
		}, Qt::UniqueConnection);
	}

	// Hook slice -> local propagator so slice-driven WL updates siblings + volume + controller
	if (auto* yz = getYZView()) {
		connect(yz, &SliceView::windowLevelChanged, this, [this, yz](double w, double l) {
			if (m_propagatingWindowLevel) return;
			m_propagatingWindowLevel = true;

			if (m_wlBridge) m_wlBridge->onWindowLevelFromSlice(w, l);

			if (m_wlController) {
				m_wlController->setWindow(w);
				m_wlController->setLevel(l);
			}
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

	// Ensure reset requests from slices still reach Lightbox (idempotent because of UniqueConnection)
	if (auto* yz = getYZView()) connect(yz, &SliceView::requestResetWindowLevel, this, &LightboxWidget::resetWindowLevel, Qt::UniqueConnection);
	if (auto* xz = getXZView()) connect(xz, &SliceView::requestResetWindowLevel, this, &LightboxWidget::resetWindowLevel, Qt::UniqueConnection);
	if (auto* xy = getXYView()) connect(xy, &SliceView::requestResetWindowLevel, this, &LightboxWidget::resetWindowLevel, Qt::UniqueConnection);
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

	// Build JSON metadata for the synthetic default image and emit so ImageInfoWidget can update.
	QJsonObject meta;
	meta.insert(QStringLiteral("fileName"), QStringLiteral("memory"));
	meta.insert(QStringLiteral("fileType"), QStringLiteral("Synthetic (sinusoid)"));
	meta.insert(QStringLiteral("date"), QDateTime::currentDateTime().toString(Qt::ISODate));

	if (img) {
		// scalar range
		double range[2] = { 0.0, 0.0 };
		img->GetScalarRange(range);
		QJsonArray rangeA;
		rangeA.append(range[0]);
		rangeA.append(range[1]);
		meta.insert(QStringLiteral("range"), rangeA);

		// dimensions
		int dims[3] = { 0, 0, 0 };
		img->GetDimensions(dims);
		QJsonArray dimsA;
		dimsA.append(dims[0]);
		dimsA.append(dims[1]);
		dimsA.append(dims[2]);
		meta.insert(QStringLiteral("dims"), dimsA);

		// origin
		double origin[3] = { 0.0, 0.0, 0.0 };
		img->GetOrigin(origin);
		QJsonArray originA;
		originA.append(origin[0]);
		originA.append(origin[1]);
		originA.append(origin[2]);
		meta.insert(QStringLiteral("origin"), originA);

		// spacing
		double spacing[3] = { 1.0, 1.0, 1.0 };
		img->GetSpacing(spacing);
		QJsonArray spacingA;
		spacingA.append(spacing[0]);
		spacingA.append(spacing[1]);
		spacingA.append(spacing[2]);
		meta.insert(QStringLiteral("spacing"), spacingA);

		// scalar type
		const char* st = img->GetScalarTypeAsString();
		if (st && *st) meta.insert(QStringLiteral("scalarType"), QString::fromUtf8(st));
		else meta.insert(QStringLiteral("scalarType"), QStringLiteral("unknown"));
	}
	else {
		// fallback placeholders if concrete data is not available
		meta.insert(QStringLiteral("range"), QJsonArray{ 0.0, 0.0 });
		meta.insert(QStringLiteral("dims"), QJsonArray{ 0, 0, 0 });
		meta.insert(QStringLiteral("origin"), QJsonArray{ 0.0, 0.0, 0.0 });
		meta.insert(QStringLiteral("spacing"), QJsonArray{ 1.0, 1.0, 1.0 });
		meta.insert(QStringLiteral("scalarType"), QStringLiteral("unknown"));
	}

	// Emit the metadata so consumers (ImageInfoWidget) can call updateFromMeta.
	emit metaReady(meta);
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

	if (m_wlController) {
		m_wlController->setImageData(image);
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

void LightboxWidget::setCroppingRegion(int xMin, int xMax,
									   int yMin, int yMax,
									   int zMin, int zMax)
{
	// Forward cropping region to the VolumeView if present.
	if (ui.volumeView) {
		ui.volumeView->setCroppingRegion(xMin, xMax, yMin, yMax, zMin, zMax);
	}

	// Compute center indices for each axis and clamp to provided ranges.
	auto clamp = [](int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); };
	int xCenter = clamp((xMin + xMax) / 2, xMin, xMax);
	int yCenter = clamp((yMin + yMax) / 2, yMin, yMax);
	int zCenter = clamp((zMin + zMax) / 2, zMin, zMax);

	// Map axes to views:
	//  - YZ view is the X-axis slice
	//  - XZ view is the Y-axis slice
	//  - XY view is the Z-axis slice
	// Only change a view's current slice if it falls outside the requested crop region.
	if (auto* yz = getYZView()) {
		const int cur = yz->getSliceIndex();
		if (cur < xMin || cur > xMax) setYZSlice(xCenter);
	}
	if (auto* xz = getXZView()) {
		const int cur = xz->getSliceIndex();
		if (cur < yMin || cur > yMax) setXZSlice(yCenter);
	}
	if (auto* xy = getXYView()) {
		const int cur = xy->getSliceIndex();
		if (cur < zMin || cur > zMax) setXYSlice(zCenter);
	}
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

	// Ask each view to restore its retained baseline (they will apply mapped/native baselines and render).
	if (auto* yz = getYZView()) yz->resetWindowLevel();
	if (auto* xz = getXZView()) xz->resetWindowLevel();
	if (auto* xy = getXYView()) xy->resetWindowLevel();
	if (auto* vol = getVolumeView()) vol->resetWindowLevel();

	// Ensure the controller UI (spinboxes) and the interactive nodes reflect the baseline we just applied.
	// Prefer VolumeView baseline if present (most authoritative), otherwise fall back to any slice view.
	double baselineW = std::numeric_limits<double>::quiet_NaN();
	double baselineL = std::numeric_limits<double>::quiet_NaN();

	if (auto* vol = getVolumeView()) {
		baselineW = vol->baselineWindowNative();
		baselineL = vol->baselineLevelNative();
	}
	if (!std::isfinite(baselineW) || !std::isfinite(baselineL)) {
		if (auto* xy = getXYView()) {
			baselineW = xy->baselineWindowNative();
			baselineL = xy->baselineLevelNative();
		}
	}
	if (!std::isfinite(baselineW) || !std::isfinite(baselineL)) {
		if (auto* xz = getXZView()) {
			baselineW = xz->baselineWindowNative();
			baselineL = xz->baselineLevelNative();
		}
	}
	if (!std::isfinite(baselineW) || !std::isfinite(baselineL)) {
		if (auto* yz = getYZView()) {
			baselineW = yz->baselineWindowNative();
			baselineL = yz->baselineLevelNative();
		}
	}

	// If we found a valid baseline, update the registered controller (if any).
	// Keep m_propagatingWindowLevel==true while doing this so the normal signal-path lambdas
	// that would re-propagate to views are temporarily suppressed (we already applied resets).
	if (m_wlController && std::isfinite(baselineW) && std::isfinite(baselineL)) {
		// Direct setter updates spinboxes and calls applyWindowLevelToNodes, but does not emit change signals
		// because setWindow/setLevel use QSignalBlocker internally.
		m_wlController->setWindow(baselineW);
		m_wlController->setLevel(baselineL);

		// Also emit a committed event so any listeners can react to the coordinated reset.
		emit m_wlController->windowLevelCommitted(baselineW, baselineL);
	}

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

	if (m_wlController) {
		vtkImageData* image = nullptr;
		if (port) {
			vtkAlgorithm* producer = port->GetProducer();
			if (producer) {
				// Try to read the producer's current output data object without forcing a pipeline update.
				vtkDataObject* out = producer->GetOutputDataObject(port->GetIndex());
				image = vtkImageData::SafeDownCast(out);

				// Fallback: if output is not a concrete vtkImageData, attempt to update the producer
				// and re-check. This forces pipeline execution which may be expensive, so it's only
				// done as a fallback.
				if (!image) {
					producer->Update();
					out = producer->GetOutputDataObject(port->GetIndex());
					image = vtkImageData::SafeDownCast(out);
				}
			}
		}
		m_wlController->setImageData(image); // image may be nullptr if concrete data is unavailable
	}
}

// drag/drop handlers ------------------------------------------------------

// add helper to find a SelectionFrameWidget under a point (place near anonymous namespace)
static SelectionFrameWidget* frameAtPoint(QWidget* root, const QPoint& p)
{
	if (!root) return nullptr;
	QWidget* child = root->childAt(p);
	for (QWidget* w = child; w; w = w->parentWidget()) {
		if (auto* sf = qobject_cast<SelectionFrameWidget*>(w)) return sf;
	}
	return nullptr;
}

// update drag handlers to highlight target
void LightboxWidget::dragEnterEvent(QDragEnterEvent* e)
{
	if (e->mimeData() && e->mimeData()->hasFormat("application/x-selectionframe")) {
		e->acceptProposedAction();

		SelectionFrameWidget* target = frameAtPoint(this, e->pos());
		if (target != m_dragHover) {
			if (m_dragHover) m_dragHover->setDragHighlight(false);
			m_dragHover = target;
			if (m_dragHover) m_dragHover->setDragHighlight(true);
		}
	}
	else {
		e->ignore();
	}
}

void LightboxWidget::dragMoveEvent(QDragMoveEvent* e)
{
	if (e->mimeData() && e->mimeData()->hasFormat("application/x-selectionframe")) {
		e->acceptProposedAction();

		// Highlight target under the current pointer (keep behavior matching dragEnterEvent)
		SelectionFrameWidget* target = frameAtPoint(this, e->pos());
		if (target != m_dragHover) {
			if (m_dragHover) m_dragHover->setDragHighlight(false);
			m_dragHover = target;
			if (m_dragHover) m_dragHover->setDragHighlight(true);
		}
	}
	else {
		e->ignore();
	}
}

void LightboxWidget::dragLeaveEvent(QDragLeaveEvent* /*e*/)
{
	// Clear any hover highlight when the drag leaves the widget
	if (m_dragHover) {
		m_dragHover->setDragHighlight(false);
		m_dragHover = nullptr;
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

	// Clear hover highlight after completing the drop so visual cuing is removed.
	if (m_dragHover) {
		m_dragHover->setDragHighlight(false);
		m_dragHover = nullptr;
	}
}
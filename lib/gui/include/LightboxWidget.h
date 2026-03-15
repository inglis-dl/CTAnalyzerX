#pragma once

#include <QWidget>
#include "ui_LightboxWidget.h"
#include <QHash>
#include <QList>
#include <QParallelAnimationGroup>
#include <QJsonObject> // added for metaReady signal

class SliceView;
class VolumeView;
class SelectionFrameWidget;
class WindowLevelWidget;
class WindowLevelBridge;

class QLabel;
class QPropertyAnimation;

class LightboxWidget : public QWidget {
	Q_OBJECT

public:
	explicit LightboxWidget(QWidget* parent = nullptr);

	void setImageData(vtkImageData* image);
	void setInputConnection(vtkAlgorithmOutput* port, bool newImg = true);
	void setDefaultImage();

	void setYZSlice(int index);
	void setXZSlice(int index);
	void setXYSlice(int index);
	QPixmap grabFramebuffer();

	SliceView* getYZView() const;
	SliceView* getXZView() const;
	SliceView* getXYView() const;
	VolumeView* getVolumeView() const;

	// Accept an externally-owned WindowLevelWidget (LightboxWidget does NOT take ownership).
	// MainWindow will create the controller, give ownership to WorkflowPanelWidget and register
	// the same controller instance here so LightboxWidget can route reset requests and avoid
	// creating a duplicate controller.
	void setWindowLevelWidget(WindowLevelWidget* ctrl);

	// Minimal accessor so MainWindow can place the controller in its layout or inspect it
	WindowLevelWidget* windowLevelWidget() const { return m_wlController; }

public slots:
	// Propagate a reset request to all child image frames (slices + volume)
	void resetWindowLevel();

	// Forward cropping regions from external UI (e.g., CropWidget).
	// LightboxWidget will apply the region to VolumeView and update slice indices.
	void setCroppingRegion(int xMin, int xMax,
						   int yMin, int yMax,
						   int zMin, int zMax);

signals:
	// Forwarded extents notifications from child views (same format as ImageFrameWidget)
	void imageExtentsChanged(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);

	// Emit JSON metadata for the current/default image so ImageInfoWidget can update.
	void metaReady(const QJsonObject& meta);

private slots:
	// Handle maximize/restore requests from child frames
	void onRequestMaximize(SelectionFrameWidget* w);
	void onRequestRestore(SelectionFrameWidget* w);

protected:
	void showEvent(QShowEvent* e) override;

	// drag/drop to reorder frames by dragging title/header bars
	void dragEnterEvent(QDragEnterEvent* e) override;
	void dragMoveEvent(QDragMoveEvent* e) override;
	void dropEvent(QDropEvent* e) override;
	void dragLeaveEvent(QDragLeaveEvent* e) override;

private:
	void connectSliceSynchronization();
	void connectSelectionCoordination();
	void connectMaximizeSignals();

	// Expansion animation helpers
	QRect mapToThis(SelectionFrameWidget* w) const;
	void startExpandAnimation(SelectionFrameWidget* target, const QRect& from, const QRect& to, bool toMaximized);
	void clearAnimOverlay();

	Ui::LightboxWidget ui;

	// Maximize state
	bool m_isMaximized = false;
	SelectionFrameWidget* m_maximized = nullptr;

	// Geometry-based expand/collapse animation overlay
	QLabel* m_animOverlay = nullptr;                 // kept for compatibility, not used in new multi-anim
	QPropertyAnimation* m_anim = nullptr;            // kept for compatibility, not used in new multi-anim

	// NEW: multi-overlay parallel animation state
	QList<QLabel*> m_animOverlays;                   // one overlay per frame
	QParallelAnimationGroup* m_animGroup = nullptr;  // run all animations simultaneously
	QHash<SelectionFrameWidget*, QRect> m_savedRects; // original rects (for restore)
	QRect m_savedTargetRect; // original rect of maximized frame relative to this

	// shared image property used by all SliceView instances (always present)
	vtkSmartPointer<vtkImageProperty> m_sharedImageProperty;

	// External Window/Level controller (NOT owned by LightboxWidget).
	// The controller is created by MainWindow and owned by WorkflowPanelWidget (via Qt parent-child).
	WindowLevelWidget* m_wlController = nullptr;
	WindowLevelBridge* m_wlBridge = nullptr;

	// Guard to prevent feedback loops while propagating WL changes
	bool m_propagatingWindowLevel = false;

	SelectionFrameWidget* m_dragHover = nullptr;
};

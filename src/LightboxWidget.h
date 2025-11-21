#pragma once

#include <QWidget>
#include "ui_LightboxWidget.h"
#include <QHash>
#include <QList>
#include <QParallelAnimationGroup>

class SliceView;
class VolumeView;
class SelectionFrameWidget;
class WindowLevelController;
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

	// Minimal accessor so MainWindow can place the controller in its layout
	WindowLevelController* windowLevelController() const { return m_wlController; }

protected:
	void showEvent(QShowEvent* e) override;

public slots:
	// Propagate a reset request to all child image frames (slices + volume)
	void resetWindowLevel();

signals:

private slots:
	// Handle maximize/restore requests from child frames
	void onRequestMaximize(SelectionFrameWidget* w);
	void onRequestRestore(SelectionFrameWidget* w);

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

	// Encapsulated Window/Level controller + bridge (owned by LightboxWidget)
	WindowLevelController* m_wlController = nullptr;
	WindowLevelBridge* m_wlBridge = nullptr;

	// Guard to prevent feedback loops while propagating WL changes
	bool m_propagatingWindowLevel = false;
};

#pragma once

#include <QWidget>
#include "ui_LightboxWidget.h"
#include <QHash>                 // ADDED
#include <QList>                 // ADDED
#include <QParallelAnimationGroup>   // ADDED

class SliceView;
class VolumeView;
class SelectionFrameWidget;

class QLabel;
class QPropertyAnimation;

class LightboxWidget : public QWidget {
	Q_OBJECT
		// Expose linked window/level mode to Qt (Designer, bindings, property animations)
		Q_PROPERTY(bool linkedWindowLevel READ linkedWindowLevel WRITE setLinkedWindowLevel NOTIFY linkedWindowLevelChanged)

public:
	explicit LightboxWidget(QWidget* parent = nullptr);

	void setImageData(vtkImageData* image);
	void setDefaultImage();

	void setYZSlice(int index);
	void setXZSlice(int index);
	void setXYSlice(int index);
	QPixmap grabFramebuffer();

	SliceView* getYZView() const;
	SliceView* getXZView() const;
	SliceView* getXYView() const;
	VolumeView* getVolumeView() const;

protected:
	void showEvent(QShowEvent* e) override;

public slots:
	void setLinkedWindowLevel(bool linked);
	bool linkedWindowLevel() const { return m_linkWindowLevel; }

signals:
	// Notify when linked window/level mode toggles
	void linkedWindowLevelChanged(bool linked);

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

	bool m_linkWindowLevel = false;
	vtkSmartPointer<vtkImageProperty> m_sharedImageProperty;
};

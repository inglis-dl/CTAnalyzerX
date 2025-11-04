#pragma once

#include <QWidget>
#include "ui_LightboxWidget.h"

class SliceView;
class VolumeView;
class SelectionFrameWidget;

class QLabel;
class QPropertyAnimation;

class LightboxWidget : public QWidget {
	Q_OBJECT
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

private:
	Ui::LightboxWidget ui;

	// Maximize state
	bool m_isMaximized = false;
	SelectionFrameWidget* m_maximized = nullptr;

	// Geometry-based expand/collapse animation overlay
	QLabel* m_animOverlay = nullptr;
	QPropertyAnimation* m_anim = nullptr;
	QRect m_savedTargetRect; // original rect of maximized frame relative to this
};

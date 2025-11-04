#ifndef LIGHTBOXWIDGET_H
#define LIGHTBOXWIDGET_H

#include <QWidget>
#include "ui_LightboxWidget.h"

class SliceView;
class VolumeView;
class SelectionFrameWidget; // forward decl for maximize/restore wiring

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
	// Hook to wire maximize/restore after UI is built
	void showEvent(QShowEvent* e) override;

private slots:
	// Handle maximize/restore requests from child frames
	void onRequestMaximize(SelectionFrameWidget* w);
	void onRequestRestore(SelectionFrameWidget* w);

private:
	Ui::LightboxWidget ui;

	// Existing helpers preserved
	void connectSliceSynchronization();
	void connectSelectionCoordination();

	// New: connect maximize/restore signals from child frames
	void connectMaximizeSignals();

	// Track maximize state (preserves existing members/API)
	bool m_isMaximized = false;
	SelectionFrameWidget* m_maximized = nullptr;
};

#endif // LIGHTBOXWIDGET_H

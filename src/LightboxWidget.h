#ifndef LIGHTBOXWIDGET_H
#define LIGHTBOXWIDGET_H

#include <QWidget>
#include "ui_LightboxWidget.h"

class SliceView;
class VolumeView;

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

private:
	Ui::LightboxWidget ui;
	void connectSliceSynchronization();
	void connectSelectionCoordination();
};

#endif // LIGHTBOXWIDGET_H

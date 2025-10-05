#ifndef LIGHTBOXWIDGET_H
#define LIGHTBOXWIDGET_H

#include <QWidget>
#include "ui_LightBoxWidget.h"

class SliceView;
class VolumeView;

class LightBoxWidget : public QWidget {
	Q_OBJECT
public:
	explicit LightBoxWidget(QWidget* parent = nullptr);

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
	Ui::LightBoxWidget ui;
	void connectSliceSynchronization();

};

#endif // LIGHTBOXWIDGET_H

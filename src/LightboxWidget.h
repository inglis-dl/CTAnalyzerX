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

	void setAxialSlice(int index);
	void setSagittalSlice(int index);
	void setCoronalSlice(int index);
	QPixmap grabFramebuffer();

	SliceView* getAxialView() const;
	SliceView* getSagittalView() const;
	SliceView* getCoronalView() const;
	VolumeView* getVolumeView() const;

private:
	Ui::LightBoxWidget ui;
	void connectSliceSynchronization();

};

#endif // LIGHTBOXWIDGET_H

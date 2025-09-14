#ifndef SLICEVIEW_H
#define SLICEVIEW_H

#include <QWidget>
#include "ui_SliceView.h"

#include <vtkSmartPointer.h>

class vtkRenderer;
class vtkImageData;
class vtkImageViewer2;
class vtkGenericOpenGLRenderWindow;
class QVTKOpenGLNativeWidget;

class SliceView : public QFrame {
	Q_OBJECT;
	Q_PROPERTY(Orientation orientation READ getOrientation WRITE setOrientation)

public:
	enum Orientation { Axial, Sagittal, Coronal };
	Q_ENUM(Orientation);

	explicit SliceView(QWidget* parent = nullptr, Orientation orientation = Axial);
	void setImageData(vtkImageData* image);
	void setSliceIndex(int index);
	int getSliceIndex() const;
	vtkRenderWindow* GetRenderWindow() const;

	// Property accessors
	Orientation getOrientation() const;
	void setOrientation(Orientation orientation);
	void resetCameraForOrientation();

	void setAxialOrientation();
	void setSagittalOrientation();
	void setCoronalOrientation();

signals:
	void sliceChanged(int index);

protected:
	void wheelEvent(QWheelEvent* event) override;

private:
	Orientation orientation;
	QVTKOpenGLNativeWidget* openGLNativeWidget;
	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow;
	vtkSmartPointer<vtkImageViewer2> viewer;

	int currentSlice;
	int maxSlice;
	void updateSlice();

	Ui::SliceView ui;
};

#endif // SLICEVIEW_H

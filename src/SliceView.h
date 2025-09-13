
#ifndef SLICEVIEW_H
#define SLICEVIEW_H

#include <QVTKOpenGLNativeWidget.h>
#include <vtkSmartPointer.h>

class vtkRenderer;
class vtkImageData;
class vtkImageViewer2;
class vtkGenericOpenGLRenderWindow;


class SliceView : public QVTKOpenGLNativeWidget {
	Q_OBJECT
public:
	enum Orientation { Axial, Sagittal, Coronal };

	explicit SliceView(Orientation orientation, QWidget* parent = nullptr);
	void setImageData(vtkImageData* image);
	void setSliceIndex(int index);
	int getSliceIndex() const;
	vtkRenderWindow* GetRenderWindow() const;

signals:
	void sliceChanged(int index);

protected:
	void wheelEvent(QWheelEvent* event) override;

private:
	Orientation orientation;
	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow;
	vtkSmartPointer<vtkImageViewer2> viewer;
	int currentSlice;
	int maxSlice;
	void updateSlice();
};

#endif // SLICEVIEW_H

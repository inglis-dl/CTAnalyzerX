
#ifndef SLICEVIEW_H
#define SLICEVIEW_H

#include <QVTKOpenGLNativeWidget.h>
#include <vtkSmartPointer.h>
#include <vtkImageViewer2.h>
#include <vtkImageData.h>
#include <vtkRenderWindowInteractor.h>

class SliceView : public QVTKOpenGLNativeWidget {
	Q_OBJECT
public:
	enum Orientation { Axial, Sagittal, Coronal };

	explicit SliceView(Orientation orientation, QWidget* parent = nullptr);
	void setImageData(vtkImageData* image);
	void setSliceIndex(int index);
	int getSliceIndex() const;

signals:
	void sliceChanged(int index);

protected:
	void wheelEvent(QWheelEvent* event) override;

private:
	Orientation orientation;
	vtkSmartPointer<vtkImageViewer2> viewer;
	int currentSlice;
	int maxSlice;
	void updateSlice();
};

#endif // SLICEVIEW_H

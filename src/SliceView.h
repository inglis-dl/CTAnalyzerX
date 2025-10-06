#ifndef SLICEVIEW_H
#define SLICEVIEW_H

#include <QFrame>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyleImage.h>
#include <vtkImageMapToWindowLevelColors.h>
#include <vtkImageActor.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageShiftScale.h>

class vtkEventQtSlotConnect;


namespace Ui { class SliceView; }

class SliceView : public QFrame
{
	Q_OBJECT;
	Q_PROPERTY(Orientation orientation READ getOrientation WRITE setOrientation)
		Q_PROPERTY(Interpolation interpolation READ getInterpolation WRITE setInterpolation)

public:
	enum Orientation { Udefined = -1, YZ, XZ, XY };
	Q_ENUM(Orientation);

	enum Interpolation { Nearest, Linear, Cubic };
	Q_ENUM(Interpolation);

	explicit SliceView(QWidget* parent = nullptr, Orientation orientation = XY);
	~SliceView();

	void setImageData(vtkImageData* image);
	void setSliceIndex(int index);
	int getSliceIndex() const;

	void setOrientation(Orientation newOrientation);
	Orientation getOrientation() const;
	void setOrientationToYZ();
	void setOrientationToXZ();
	void setOrientationToXY();

	void setInterpolation(Interpolation newInterpolation);
	Interpolation getInterpolation() const;
	void setInterpolationToNearest();
	void setInterpolationToLinear();
	void setInterpolationToCubic();

	vtkRenderWindow* GetRenderWindow() const;

	vtkImageSlice* getImageSlice() { return imageSlice; }

	int getMinSliceIndex() const;
	int getMaxSliceIndex() const;

public slots:
	void trapSpin(vtkObject*);

signals:
	void sliceChanged(int);
	void orientationChanged(Orientation);
	void interpolationChanged(Interpolation);


private:
	void updateCamera();
	void updateSlice();
	void updateSliceRange();

	Ui::SliceView* ui = nullptr;
	Orientation orientation;
	Interpolation interpolation = Linear;
	int currentSlice = 0;
	int minSlice = 0;
	int maxSlice = 0;
	bool imageInitialized = false;

	vtkSmartPointer<vtkImageData> imageData;
	vtkSmartPointer<vtkRenderer> renderer;
	vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow;
	vtkSmartPointer<vtkInteractorStyleImage> interactorStyle;

	vtkSmartPointer<vtkImageMapToWindowLevelColors> windowLevelFilter;
	vtkSmartPointer<vtkImageShiftScale> shiftScaleFilter;
	vtkSmartPointer<vtkImageSliceMapper> sliceMapper;
	vtkSmartPointer<vtkImageSlice> imageSlice;
	vtkSmartPointer<vtkImageProperty> imageProperty;

	vtkSmartPointer<vtkEventQtSlotConnect> qvtkConnection;
};

#endif // SLICEVIEW_H
#ifndef SLICEVIEW_H
#define SLICEVIEW_H

#include "SceneFrameWidget.h"

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
class vtkObject; // forward declare for slot
class QLineEdit;
class QLabel;    // added

namespace Ui { class SliceView; }

class SliceView : public SceneFrameWidget
{
	Q_OBJECT
		Q_PROPERTY(Interpolation interpolation READ getInterpolation WRITE setInterpolation)

public:
	enum Interpolation { Nearest, Linear, Cubic };
	Q_ENUM(Interpolation)

		explicit SliceView(QWidget* parent = nullptr, ViewOrientation orientation = VIEW_ORIENTATION_XY);
	~SliceView();

	void setImageData(vtkImageData* image);
	void setSliceIndex(int index);
	int getSliceIndex() const;

	void setViewOrientation(ViewOrientation orient) override;

	void setInterpolation(Interpolation newInterpolation);
	Interpolation getInterpolation() const;
	void setInterpolationToNearest();
	void setInterpolationToLinear();
	void setInterpolationToCubic();

	int getMaxSliceIndex() const;
	int getMinSliceIndex() const;

signals:
	void sliceChanged(int);
	void interpolationChanged(Interpolation);

protected:
	// SceneFrameWidget overrides
	vtkRenderWindow* getRenderWindow() const override;
	void resetCamera() override;
	void orthogonalizeView() override;
	void flipHorizontal() override;
	void flipVertical() override;
	void rotateCamera(double degrees) override;

	// Optional capability hints (used by action enabling)
	bool canFlipHorizontal() const override { return true; }
	bool canFlipVertical() const override { return true; }
	bool canRotate() const override { return true; }

private:
	void updateCamera();
	void updateSlice();
	void updateSliceRange();

	Ui::SliceView* ui = nullptr;
	Interpolation m_interpolation = Linear;
	int m_currentSlice = 0;
	int m_minSlice = 0;
	int m_maxSlice = 0;
	bool m_imageInitialized = false;

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

	QLineEdit* m_editSliceIndex = nullptr;
	QLabel* m_labelMinSlice = nullptr; // added
	QLabel* m_labelMaxSlice = nullptr; // added

	// Build a bottom bar: [minLabel] [slider] [maxLabel] [lineEdit]
	void buildSliderBar(QWidget* rootContent);

private slots:
	// Must be a Qt slot for vtkEventQtSlotConnect
	void trapSpin(vtkObject* obj);
};

#endif // SLICEVIEW_H
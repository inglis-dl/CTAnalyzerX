#ifndef SLICEVIEW_H
#define SLICEVIEW_H

#include "ImageFrameWidget.h"

#include <QFrame>

#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyleImage.h>
#include <vtkImageActor.h>
#include <vtkImageSliceMapper.h>

class vtkEventQtSlotConnect;
class vtkObject; // forward declare for slot
class QLineEdit;
class QLabel;    // added

namespace Ui { class SliceView; }

class SliceView : public ImageFrameWidget
{
	Q_OBJECT

public:
	explicit SliceView(QWidget* parent = nullptr, ViewOrientation orientation = VIEW_ORIENTATION_XY);
	~SliceView();

	void setImageData(vtkImageData* image) override;
	void setSliceIndex(int index);
	int getSliceIndex() const;

	void setInterpolation(Interpolation newInterpolation) override;
	void setViewOrientation(ViewOrientation orient) override;

	int getMaxSliceIndex() const;
	int getMinSliceIndex() const;

	// Apply Window/Level specified in the image's native scalar domain.
	// This method maps to the vtkImageProperty domain using the view's m_scalarShift/m_scalarScale
	// and updates the interactor style baseline so plain 'r' will restore it.
	void setWindowLevelNative(double window, double level);

signals:
	void sliceChanged(int);
	void interpolationChanged(Interpolation);

protected:
	void resetCamera() override;
	void rotateCamera(double degrees) override;
	void flipHorizontal();
	void flipVertical();

	// Ensure Qt shortcuts don't steal keys intended for VTK
	bool eventFilter(QObject* watched, QEvent* event) override;

	void createMenuAndActions();

	// Apply retained baseline WL (native -> mapped) on reset
	void resetWindowLevel() override;

private:
	void updateCamera();
	void updateSlice();
	void updateSliceRange();

	Ui::SliceView* ui = nullptr;
	int m_currentSlice = 0;
	int m_minSlice = 0;
	int m_maxSlice = 0;

	vtkSmartPointer<vtkInteractorStyleImage> interactorStyle;
	vtkSmartPointer<vtkImageSliceMapper> sliceMapper;
	vtkSmartPointer<vtkImageSlice> imageSlice;
	vtkSmartPointer<vtkImageProperty> imageProperty;
	vtkSmartPointer<vtkEventQtSlotConnect> qvtkConnection;

	QLineEdit* m_editSliceIndex = nullptr;
	QLabel* m_labelMinSlice = nullptr;
	QLabel* m_labelMaxSlice = nullptr;

	// Build a bottom bar: [minLabel] [slider] [maxLabel] [lineEdit]
	void buildSliderBar(QWidget* rootContent);

	double m_windowLevelInitial[2];
	int m_windowLevelStartPosition[2];
	int m_windowLevelCurrentPosition[2];

private slots:
	// Must be a Qt slot for vtkEventQtSlotConnect
	void trapSpin(vtkObject*);

	// Handle ResetWindowLevelEvent from vtkInteractorStyleImage
	void onResetWindowLevel(vtkObject* obj);

	// Handle interactive WindowLevelEvent from vtkInteractorStyleImage
	void onInteractorWindowLevel(vtkObject* obj);

	// Handle StartWindowLevelEvent and EndWindowLevelEvent so we can update UI/baseline
	void onInteractorStartWindowLevel(vtkObject* obj);
	void onInteractorEndWindowLevel(vtkObject* obj);
};

#endif // SLICEVIEW_H
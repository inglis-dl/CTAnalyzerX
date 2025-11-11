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
#include <vtkImageProperty.h>

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

	// Preserve / restore transient view state when upstream image content changes
	void captureDerivedViewState() override;
	void restoreDerivedViewState() override;

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

	// install a shared vtkImageProperty (sharedProp may be the same instance across views)
	void setSharedImageProperty(vtkImageProperty* sharedProp);

	// restore an independent imageProperty (fresh copy) if caller wants to un-link
	void clearSharedImageProperty();

	// Expose resetWindowLevel as public so LightboxWidget can call it
	void resetWindowLevel() override;

public slots:
	void updateData() override;

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

private:
	void updateCamera();
	void updateSlice();
	void updateSliceRange();

	Ui::SliceView* ui = nullptr;
	int m_currentSlice = 0;
	int m_minSlice = 0;
	int m_maxSlice = 0;

	// Saved transient state used by capture/restore hooks
	vtkSmartPointer<vtkCamera> m_savedCamera;
	// store the saved slice as a 3D world coordinate (physical point)
	double m_savedSliceWorld[3] = { 0.0, 0.0, 0.0 };
	double m_savedMappedWindow = std::numeric_limits<double>::quiet_NaN();
	double m_savedMappedLevel = std::numeric_limits<double>::quiet_NaN();
	bool m_hasSavedState = false;

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

	// Preserve the original baseline (native domain) computed at setImageData().
	// These must remain constant until the next setImageData() call.
	bool m_originalBaselineValid = false;
	double m_originalBaselineWindowNative = std::numeric_limits<double>::quiet_NaN();
	double m_originalBaselineLevelNative = std::numeric_limits<double>::quiet_NaN();

	// helper: ensure vtkInteractorStyleImage internal baseline values reflect imageProperty
	void updateInteractorWindowLevelBaseline();

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

	// Explicit editor handlers (replaced lambdas to improve diagnosability)
	void onEditorEditingFinished();
	void onEditorReturnPressed();
};

#endif // SLICEVIEW_H
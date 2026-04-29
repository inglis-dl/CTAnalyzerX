#pragma once

#include "ImageFrameWidget.h"

#include <QFrame>

#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageActor.h>
#include <vtkImageData.h>
#include <vtkImageProperty.h>
#include <vtkImageSliceMapper.h>
#include <vtkInteractorStyleImage.h>
#include <vtkSmartPointer.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

class QLabel;
class QLineEdit;
class vtkActor;
class vtkEventQtSlotConnect;
class vtkImageOrthoPlanes;
class vtkImageSlicePointPlacer;
class vtkObject;
class vtkPolyDataMapper;
class vtkSliceOutlineSource;

namespace Ui { class SliceView; }

class SliceView : public ImageFrameWidget
{
	Q_OBJECT
		Q_PROPERTY(bool outlineVisible READ outlineVisible WRITE setOutlineVisible)
		Q_PROPERTY(QColor outlineColor READ outlineColor WRITE setOutlineColor NOTIFY outlineColorChanged)

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

	// Expose resetWindowLevel as public so LightboxWidget can call it
	void resetWindowLevel() override;

	bool outlineVisible() const { return m_outlineVisible; }
	QColor outlineColor() const { return m_outlineColor; }

	vtkImageSlicePointPlacer* pointPlacer() const;

	// Register a vtkImageOrthoPlanes instance owned by the companion VolumeView.
	// When set, every slice change automatically repositions the ortho plane that
	// corresponds to this view's current orientation.  Pass nullptr to detach.
	void setOrthoPlanes(vtkSmartPointer<vtkImageOrthoPlanes> planes);

public slots:
	void updateData() override;
	void setOutlineVisible(bool visible);
	void setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);
	void setOutlineColor(const QColor& color);

signals:
	void sliceChanged(int);
	void interpolationChanged(Interpolation);
	// Emitted when the user requests a reset (e.g. presses 'r').  LightboxWidget
	// or a central controller should perform the coordinated reset on all views.
	void requestResetWindowLevel();
	void outlineColorChanged(const QColor& color);

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

	vtkSmartPointer<vtkInteractorStyleImage> m_interactorStyle;
	vtkSmartPointer<vtkImageSliceMapper> m_sliceMapper;
	vtkSmartPointer<vtkImageSlice> m_imageSlice;
	vtkSmartPointer<vtkImageProperty> m_imageProperty;
	vtkSmartPointer<vtkEventQtSlotConnect> m_qvtkConnection;
	vtkSmartPointer<vtkImageSlicePointPlacer> m_pointPlacer;

	vtkSmartPointer<vtkSliceOutlineSource> m_outlineSource;
	vtkSmartPointer<vtkPolyDataMapper>   m_outlineMapper;
	vtkSmartPointer<vtkActor>            m_outlineActor;
	bool m_outlineVisible = false;
	QColor m_outlineColor = QColor(255, 0, 0);

	bool m_requestedCroppingEnabled = false;
	int  m_requestedCroppingRegion[6] = { 0, -1, 0, -1, 0, -1 };

	QLineEdit* m_editSliceIndex = nullptr;
	QLabel* m_labelMinSlice = nullptr;
	QLabel* m_labelMaxSlice = nullptr;

	// Build a bottom bar: [minLabel] [slider] [maxLabel] [lineEdit]
	void buildSliderBar(QWidget* rootContent);

	vtkSmartPointer<vtkImageOrthoPlanes> m_linkedOrthoPlanes;

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

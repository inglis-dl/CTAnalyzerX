#pragma once

#include "ImageFrameWidget.h"

#include <vtkSmartPointer.h>

class vtkEventQtSlotConnect;
class vtkVolume;
class vtkVolumeProperty;
class vtkGPUVolumeRayCastMapper;
class vtkImagePlaneWidget;
class vtkImageData;
class vtkColorTransferFunction;
class vtkPiecewiseFunction;
class vtkInteractorStyleTrackballCamera;
class vtkRenderWindowInteractor;
class vtkCommand;
class vtkObject;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkCamera;
class vtkImageSliceMapper;
class vtkImageSlice;
class vtkCallbackCommand;
class vtkActor;
class vtkImageOrthoPlanes;

namespace Ui { class VolumeView; }

class VolumeView : public ImageFrameWidget
{
	Q_OBJECT
		Q_PROPERTY(bool orthoPlanesVisible READ orthoPlanesVisible WRITE setOrthoPlanesVisible NOTIFY orthoPlanesVisibleChanged)
		Q_PROPERTY(bool shadingEnabled READ shadingEnabled WRITE setShadingEnabled)

public:
	explicit VolumeView(QWidget* parent = nullptr);
	~VolumeView();

	void setInterpolation(Interpolation newInterpolation) override;

	void setViewOrientation(ViewOrientation orientation) override;

	Q_INVOKABLE void setColorWindowLevel(double window, double level) override;

	// Apply a native-domain window/level to the orthogonal image-slice actors
	// (used when a SliceView changes WL so the 3D slice actors match the 2D slices).
	void setSliceWindowLevelNative(double window, double level);

	void updateSlicePlanes(int x, int y, int z);

	bool orthoPlanesVisible() const { return m_orthoPlanesVisible; }

	bool shadingEnabled() const { return m_shadingEnabled; }
	void setShadingEnabled(bool on);

	void createMenuAndActions();

signals:
	void imageExtentsChanged(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);
	void orthoPlanesVisibleChanged(bool visible);
	// Emitted when the effective cropping enabled state changes (e.g. reset to false on new image)
	void croppingEnabledChanged(bool enabled);

public slots:
	// Expose as a slot so UI widgets (e.g., VolumeControlsWidget) can connect directly
	void setOrthoPlanesVisible(bool visible);

	void setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);
	void resetCamera() override;
	void resetWindowLevel() override;

	void updateData() override;

	// Preserve / restore transient view state when upstream image content changes
	void captureDerivedViewState() override;
	void restoreDerivedViewState() override;

private:
	Ui::VolumeView* ui = nullptr;

	vtkSmartPointer<vtkEventQtSlotConnect> m_qvtk;
	bool m_orthoPlanesVisible = false;
	bool m_shadingEnabled = false;

	vtkSmartPointer<vtkGPUVolumeRayCastMapper> m_mapper;
	vtkSmartPointer<vtkVolumeProperty>         m_volumeProperty;
	vtkSmartPointer<vtkVolume>                 m_volume;

	vtkSmartPointer<vtkColorTransferFunction>  m_actualColorTF;
	vtkSmartPointer<vtkColorTransferFunction>  m_colorTF;
	vtkSmartPointer<vtkPiecewiseFunction>      m_actualScalarOpacity;
	vtkSmartPointer<vtkPiecewiseFunction>      m_scalarOpacity;

	void updateMappedOpacityFromActual();
	void updateMappedColorsFromActual();
	void initializeDefaultTransferFunctions();

	vtkSmartPointer<vtkImageOrthoPlanes> m_orthoPlanes;

	// Saved transient state used by capture/restore hooks
	vtkSmartPointer<vtkCamera> m_savedCamera;
	// store each orthogonal slice as a 3D world coordinate (point on the plane)
	double m_savedSliceWorldX[3] = { 0.0, 0.0, 0.0 }; // X-normal plane (YZ) world point
	double m_savedSliceWorldY[3] = { 0.0, 0.0, 0.0 }; // Y-normal plane (XZ) world point
	double m_savedSliceWorldZ[3] = { 0.0, 0.0, 0.0 }; // Z-normal plane (XY) world point
	bool m_savedOrthoPlanesVisible = false;
	vtkSmartPointer<vtkColorTransferFunction> m_savedActualColorTF;
	vtkSmartPointer<vtkPiecewiseFunction> m_savedActualScalarOpacity;
	bool m_hasSavedState = false;

private slots:
	// Full-signature observer to optionally abort the event
	void onInteractorChar(vtkObject* caller, unsigned long eventId, void* clientData, void* callData, vtkCommand* command);
	void onCameraModified(vtkObject* caller);
};
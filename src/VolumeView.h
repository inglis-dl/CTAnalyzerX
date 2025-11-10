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

namespace Ui { class VolumeView; }

class VolumeView : public ImageFrameWidget
{
	Q_OBJECT
		Q_PROPERTY(bool slicePlanesVisible READ slicePlanesVisible WRITE setSlicePlanesVisible NOTIFY slicePlanesVisibleChanged)
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

	bool slicePlanesVisible() const { return m_slicePlanesVisible; }

	bool shadingEnabled() const { return m_shadingEnabled; }
	void setShadingEnabled(bool on);

	void createMenuAndActions();

signals:
	void imageExtentsChanged(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);
	void slicePlanesVisibleChanged(bool visible);
	// Emitted when the effective cropping enabled state changes (e.g. reset to false on new image)
	void croppingEnabledChanged(bool enabled);

public slots:
	// Expose as a slot so UI widgets (e.g., VolumeControlsWidget) can connect directly
	void setSlicePlanesVisible(bool visible);

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
	bool m_slicePlanesVisible = false;
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

	// Orthogonal slice actors used in 3D "slice planes" mode (replaces vtkImagePlaneWidget usage).
	vtkSmartPointer<vtkImageSliceMapper> m_sliceMapperYZ;
	vtkSmartPointer<vtkImageSliceMapper> m_sliceMapperXZ;
	vtkSmartPointer<vtkImageSliceMapper> m_sliceMapperXY;
	vtkSmartPointer<vtkImageSlice>       m_imageSliceYZ;
	vtkSmartPointer<vtkImageSlice>       m_imageSliceXZ;
	vtkSmartPointer<vtkImageSlice>       m_imageSliceXY;

	// Outline actors for per-slice bounding rectangles (one per orthogonal slice)
	vtkSmartPointer<vtkPolyData>         m_outlinePolyYZ;
	vtkSmartPointer<vtkPolyData>         m_outlinePolyXZ;
	vtkSmartPointer<vtkPolyData>         m_outlinePolyXY;

	vtkSmartPointer<vtkPolyDataMapper>   m_outlineMapperYZ;
	vtkSmartPointer<vtkPolyDataMapper>   m_outlineMapperXZ;
	vtkSmartPointer<vtkPolyDataMapper>   m_outlineMapperXY;

	vtkSmartPointer<vtkActor>            m_outlineActorYZ;
	vtkSmartPointer<vtkActor>            m_outlineActorXZ;
	vtkSmartPointer<vtkActor>            m_outlineActorXY;

	// Helpers to create / update per-slice outline geometry
	void createSliceOutlineActors();
	void updateSliceOutlineYZ(int cx);
	void updateSliceOutlineXZ(int cy);
	void updateSliceOutlineXY(int cz);

	// Saved transient state used by capture/restore hooks
	vtkSmartPointer<vtkCamera> m_savedCamera;
	int m_savedSliceX = 0;
	int m_savedSliceY = 0;
	int m_savedSliceZ = 0;
	bool m_savedSlicePlanesVisible = false;
	vtkSmartPointer<vtkColorTransferFunction> m_savedActualColorTF;
	vtkSmartPointer<vtkPiecewiseFunction> m_savedActualScalarOpacity;
	bool m_hasSavedState = false;

private slots:
	// Full-signature observer to optionally abort the event
	void onInteractorChar(vtkObject* caller, unsigned long eventId, void* clientData, void* callData, vtkCommand* command);
	void onCameraModified(vtkObject* caller);
};
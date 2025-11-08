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
class vtkCamera;
class vtkImageSliceMapper;
class vtkImageSlice;
class vtkCallbackCommand;

namespace Ui { class VolumeView; }

class VolumeView : public ImageFrameWidget
{
	Q_OBJECT
		Q_PROPERTY(bool slicePlanesVisible READ slicePlanesVisible WRITE setSlicePlanesVisible NOTIFY slicePlanesVisibleChanged)
		Q_PROPERTY(bool shadingEnabled READ shadingEnabled WRITE setShadingEnabled)

public:
	explicit VolumeView(QWidget* parent = nullptr);
	~VolumeView();

	void setImageData(vtkImageData* image) override;
	void setInterpolation(Interpolation newInterpolation) override;

	void setViewOrientation(ViewOrientation orientation) override;

	Q_INVOKABLE void setColorWindowLevel(double window, double level) override;

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

private:
	Ui::VolumeView* ui = nullptr;

	vtkSmartPointer<vtkGPUVolumeRayCastMapper> m_mapper;
	vtkSmartPointer<vtkVolumeProperty>         m_volumeProperty;
	vtkSmartPointer<vtkVolume>                 m_volume;

	vtkSmartPointer<vtkColorTransferFunction>  m_actualColorTF;
	vtkSmartPointer<vtkColorTransferFunction>  m_colorTF;
	vtkSmartPointer<vtkPiecewiseFunction>      m_actualScalarOpacity;
	vtkSmartPointer<vtkPiecewiseFunction>      m_scalarOpacity;

	// Orthogonal slice actors used in 3D "slice planes" mode (replaces vtkImagePlaneWidget usage).
	vtkSmartPointer<vtkImageSliceMapper> m_sliceMapperYZ;
	vtkSmartPointer<vtkImageSliceMapper> m_sliceMapperXZ;
	vtkSmartPointer<vtkImageSliceMapper> m_sliceMapperXY;
	vtkSmartPointer<vtkImageSlice>       m_imageSliceYZ;
	vtkSmartPointer<vtkImageSlice>       m_imageSliceXZ;
	vtkSmartPointer<vtkImageSlice>       m_imageSliceXY;

	vtkSmartPointer<vtkEventQtSlotConnect> m_qvtk;

	bool m_slicePlanesVisible = false;
	bool m_shadingEnabled = false;

	void updateMappedOpacityFromActual();
	void updateMappedColorsFromActual();
	void initializeDefaultTransferFunctions();

private slots:
	// Full-signature observer to optionally abort the event
	void onInteractorChar(vtkObject* caller, unsigned long eventId, void* clientData, void* callData, vtkCommand* command);
	void onCameraModified(vtkObject* caller);

};


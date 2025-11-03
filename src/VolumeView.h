#pragma once

#include "ImageFrameWidget.h"

#include <vtkSmartPointer.h>

class vtkVolume;
class vtkVolumeProperty;
class vtkGPUVolumeRayCastMapper;
class vtkImagePlaneWidget;
class vtkImageData;
class vtkColorTransferFunction;
class vtkPiecewiseFunction;
class vtkInteractorStyleTrackballCamera;
class vtkRenderWindowInteractor;

namespace Ui { class VolumeView; }

class VolumeView : public ImageFrameWidget
{
	Q_OBJECT
		Q_PROPERTY(bool slicePlanesVisible READ slicePlanesVisible WRITE setSlicePlanesVisible NOTIFY slicePlanesVisibleChanged)
		// Optional convenience: expose shading toggle (off by default)
		Q_PROPERTY(bool shadingEnabled READ shadingEnabled WRITE setShadingEnabled)

public:
	explicit VolumeView(QWidget* parent = nullptr);
	~VolumeView();

	void setImageData(vtkImageData* image) override;
	void setInterpolation(Interpolation newInterpolation) override;

	void setViewOrientation(ViewOrientation orientation) override;

	// Window/level for volume rendering (updates opacity/color TFs)
	Q_INVOKABLE void setColorWindowLevel(double window, double level) override;

	// Slice planes (for cross-hairs / link to SliceView)
	void updateSlicePlanes(int x, int y, int z);

	// Property accessors
	bool slicePlanesVisible() const { return m_slicePlanesVisible; }
	void setSlicePlanesVisible(bool visible);

	// Optional shading control
	bool shadingEnabled() const { return m_shadingEnabled; }
	void setShadingEnabled(bool on);

	void createMenuAndActions();

signals:
	// Emitted when a new image is set
	void imageExtentsChanged(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);
	void slicePlanesVisibleChanged(bool visible);

public slots:
	// Cropping region for the volume mapper
	void setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);

	// Recenter camera on the volume while preserving current rotation
	void resetCamera() override;

private:
	Ui::VolumeView* ui = nullptr;

	// Pipeline
	vtkSmartPointer<vtkGPUVolumeRayCastMapper> m_mapper;
	vtkSmartPointer<vtkVolumeProperty>         m_volumeProperty;
	vtkSmartPointer<vtkVolume>                 m_volume;

	// Transfer functions: maintain both actual and mapped (post shift/scale) like vtkVolumeScene
	vtkSmartPointer<vtkColorTransferFunction>  m_actualColorTF;
	vtkSmartPointer<vtkColorTransferFunction>  m_colorTF;
	vtkSmartPointer<vtkPiecewiseFunction>      m_actualScalarOpacity;
	vtkSmartPointer<vtkPiecewiseFunction>      m_scalarOpacity;

	// Plane widgets
	vtkSmartPointer<vtkImagePlaneWidget> m_yzPlane; // normal X
	vtkSmartPointer<vtkImagePlaneWidget> m_xzPlane; // normal Y
	vtkSmartPointer<vtkImagePlaneWidget> m_xyPlane; // normal Z

	bool m_slicePlanesVisible = false;
	bool m_shadingEnabled = false; // new

	void updateMappedOpacityFromActual();
	void updateMappedColorsFromActual();
	void initializeDefaultTransferFunctions();
};


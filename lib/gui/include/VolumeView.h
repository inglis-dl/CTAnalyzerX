#pragma once

#include "ImageFrameWidget.h"

#include <vtkSmartPointer.h>

class vtkActor;
class vtkCamera;
class vtkColorTransferFunction;
class vtkCommand;
class vtkEventQtSlotConnect;
class vtkGPUVolumeRayCastMapper;
class vtkImageData;
class vtkImageOrthoPlanes;
class vtkObject;
class vtkPiecewiseFunction;
class vtkPolyData;
class vtkPolyDataMapper;
class vtkVolume;
class vtkVolumeOutlineSource;
class vtkVolumeProperty;

namespace Ui { class VolumeView; }

class VolumeView : public ImageFrameWidget
{
	Q_OBJECT
		Q_PROPERTY(bool orthoPlanesVisible READ orthoPlanesVisible WRITE setOrthoPlanesVisible NOTIFY orthoPlanesVisibleChanged)
		Q_PROPERTY(bool shadingEnabled READ shadingEnabled WRITE setShadingEnabled)
		Q_PROPERTY(bool outlineVisible READ outlineVisible WRITE setOutlineVisible)
		Q_PROPERTY(QColor outlineColor READ outlineColor WRITE setOutlineColor NOTIFY outlineColorChanged)

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

	bool contentHidden() const { return m_contentHidden; }

	bool outlineVisible() const { return m_outlineVisible; }

	QColor outlineColor() const { return m_outlineColor; }

	bool shadingEnabled() const { return m_shadingEnabled; }

	void setShadingEnabled(bool on);

	void createMenuAndActions();

	vtkPiecewiseFunction* actualScalarOpacity() const;

signals:
	void orthoPlanesVisibleChanged(bool visible);

	// Emitted when the effective cropping enabled state changes (e.g. reset to false on new image)
	void croppingEnabledChanged(bool enabled);

	void outlineColorChanged(const QColor& color);

	void actualScalarOpacityUpdated();

public slots:
	// Expose as a slot so UI widgets can connect directly
	void setOrthoPlanesVisible(bool visible);

	void setOutlineVisible(bool visible);

	void setOutlineColor(const QColor& color);

	void setCroppingRegion(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);

	void resetCamera() override;

	void resetWindowLevel() override;

	void updateData() override;

	// Preserve / restore transient view state when upstream image content changes
	void captureDerivedViewState() override;

	void restoreDerivedViewState() override;

	// Persist/load volume-specific settings (override of ImageFrameWidget)
	void readSettings() override;

	void writeSettings() const override;

	void hideAllContent();

private slots:
	// Full-signature observer to optionally abort the event
	void onInteractorChar(vtkObject* caller, unsigned long eventId, void* clientData, void* callData, vtkCommand* command);

	void onCameraModified(vtkObject* caller);

private:
	Ui::VolumeView* ui = nullptr;

	vtkSmartPointer<vtkEventQtSlotConnect> m_qvtk;
	bool m_orthoPlanesVisible = false;
	bool m_shadingEnabled = false;
	bool m_contentHidden = false;

	vtkSmartPointer<vtkGPUVolumeRayCastMapper> m_mapper;
	vtkSmartPointer<vtkVolumeProperty>         m_volumeProperty;
	vtkSmartPointer<vtkVolume>                 m_volume;

	vtkSmartPointer<vtkColorTransferFunction>  m_actualColorTF;
	vtkSmartPointer<vtkColorTransferFunction>  m_colorTF;
	vtkSmartPointer<vtkPiecewiseFunction>      m_actualScalarOpacity;
	vtkSmartPointer<vtkPiecewiseFunction>      m_scalarOpacity;

	vtkSmartPointer<vtkVolumeOutlineSource> m_outlineSource;
	vtkSmartPointer<vtkPolyDataMapper>   m_outlineMapper;
	vtkSmartPointer<vtkActor>            m_outlineActor;
	bool m_outlineVisible = false;
	QColor m_outlineColor = QColor(255, 0, 0);

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

	double m_minTFNodeX;
};
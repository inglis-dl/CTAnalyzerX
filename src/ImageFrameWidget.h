#pragma once

#include "SelectionFrameWidget.h"

#include <QWidget>
#include <limits>

class vtkImageData;
class vtkRenderer;
class vtkGenericOpenGLRenderWindow;
class vtkRenderWindow;
class vtkImageShiftScale;

// forward-declare VTK classes used by the orientation marker
class vtkOrientationMarkerWidget;
class vtkActor;
class vtkPropAssembly;

#include <vtkSmartPointer.h>

class ImageFrameWidget : public SelectionFrameWidget
{
	Q_OBJECT
		// Properties
		Q_PROPERTY(ViewOrientation viewOrientation READ viewOrientation WRITE setViewOrientation NOTIFY viewOrientationChanged)
		Q_PROPERTY(Interpolation interpolation READ interpolation WRITE setInterpolation NOTIFY interpolationChanged)
		Q_PROPERTY(LinkPropagationMode linkPropagationMode READ linkPropagationMode WRITE setLinkPropagationMode NOTIFY linkPropagationModeChanged)

public:
	enum Interpolation { Nearest, Linear, Cubic };
	Q_ENUM(Interpolation)

		enum ViewOrientation { VIEW_ORIENTATION_YZ = 0, VIEW_ORIENTATION_XZ = 1, VIEW_ORIENTATION_XY = 2 };
	Q_ENUM(ViewOrientation)

		// Propagation mode for linked WL
		enum LinkPropagationMode { Disabled, EndOnly, Live };
	Q_ENUM(LinkPropagationMode)

		explicit ImageFrameWidget(QWidget* parent = nullptr);
	~ImageFrameWidget() override;

	// Rendering entry point
	Q_INVOKABLE void render();

	// Reset WL to the retained baseline computed at setImageData() time.
	// VolumeView uses this directly; SliceView overrides to apply mapped WL.
	Q_INVOKABLE virtual void resetWindowLevel();

	// Access to retained baseline in native scalar domain
	double baselineWindowNative() const { return m_baselineWindowNative; }
	double baselineLevelNative()  const { return m_baselineLevelNative; }

	// Orientation API (now strongly-typed)
	ViewOrientation viewOrientation() const { return m_viewOrientation; }
	virtual void setViewOrientation(ViewOrientation orientation);
	Q_INVOKABLE void setViewOrientationToXY() { setViewOrientation(VIEW_ORIENTATION_XY); }
	Q_INVOKABLE void setViewOrientationToYZ() { setViewOrientation(VIEW_ORIENTATION_YZ); }
	Q_INVOKABLE void setViewOrientationToXZ() { setViewOrientation(VIEW_ORIENTATION_XZ); }

	// Interpolation
	virtual void setInterpolation(Interpolation newInterpolation) {};
	Interpolation interpolation() const { return m_interpolation; }
	Q_INVOKABLE void setInterpolationToNearest() { setInterpolation(Nearest); };
	Q_INVOKABLE void setInterpolationToLinear() { setInterpolation(Linear); };
	Q_INVOKABLE void setInterpolationToCubic() { setInterpolation(Cubic); };

	// Common image setter: stores the image then calls the derived hook.
	virtual void setImageData(vtkImageData* image) {};
	vtkImageData* imageData() const { return m_imageData; }

	// Abstract hook: views implement with their own pipeline logic
	// The bus uses native domain (original image scalar domain).
	virtual void setColorWindowLevel(double window, double level) {};

	// Return the canonical orientation when the main camera's view-normal is within
	// `maxAngleDeg` degrees of a principal axis. Returns one of ViewOrientation values
	// (VIEW_ORIENTATION_YZ=0, VIEW_ORIENTATION_XZ=1, VIEW_ORIENTATION_XY=2) or -1 if none match.
	int cameraAlignedOrientation(double maxAngleDeg) const;

	// WL propagation mode
	void setLinkPropagationMode(LinkPropagationMode mode) {
		if (m_linkPropagationMode == mode) return;
		m_linkPropagationMode = mode;
		emit linkPropagationModeChanged(m_linkPropagationMode);
	}
	LinkPropagationMode linkPropagationMode() const { return m_linkPropagationMode; }

	// helpers to convert baseline WL to mapped domain
	void setBaselineWindowLevel(double windowNative, double levelNative);
	std::pair<double, double> mapWindowLevelToMapped(double windowNative, double levelNative) const;
	std::pair<double, double> baselineMapped() const;

	// Orientation marker control (wireframe cube + positive-axis halves)
	void setOrientationMarkerVisible(bool visible);
	bool orientationMarkerVisible() const { return m_orientationMarkerVisible; }

public slots:
	virtual void updateData() {};

signals:
	void viewOrientationChanged(ViewOrientation);
	void interpolationChanged(Interpolation);
	void windowLevelChanged(double window, double level);
	void linkPropagationModeChanged(LinkPropagationMode mode);

protected:
	// SceneFrameWidget override: used by render() and tooling.
	vtkRenderWindow* getRenderWindow() const;

	// Helper to install the scene content into the SelectionFrameWidget body.
	void setSceneContent(QWidget* content) { setCentralWidget(content); }

	// Camera helpers with safe defaults (shared by derived classes).
	virtual void resetCamera();
	virtual void rotateCamera(double degrees) {}

	// Hook from SelectionFrameWidget to gate VTK interactivity on selection
	void onSelectionChanged(bool selected) override;

	// Access to the shared renderer and render window for derived classes.
	vtkRenderer* renderer() const { return m_renderer; }
	vtkGenericOpenGLRenderWindow* genericRenderWindow() const { return m_renderWindow; }

	// Optional: allow derived classes to adjust default renderer config.
	virtual void initializeRendererDefaults();

	// Map orientation <-> label
	QString orientationLabel(ViewOrientation orient) const;
	ViewOrientation labelToOrientation(const QString& label) const;

	// for derived classes that set m_viewOrientation directly
	void notifyViewOrientationChanged();

	ViewOrientation  m_viewOrientation = VIEW_ORIENTATION_XY;
	Interpolation    m_interpolation = Linear;
	LinkPropagationMode m_linkPropagationMode = Disabled;

	vtkSmartPointer<vtkImageData>                   m_imageData;
	vtkSmartPointer<vtkRenderer>                    m_renderer;
	vtkSmartPointer<vtkGenericOpenGLRenderWindow>   m_renderWindow;
	vtkSmartPointer<vtkImageShiftScale>             m_shiftScaleFilter;

	// Mapping info derived from input
	int    m_nativeScalarType = -1;
	double m_scalarRangeMin = 0.0;
	double m_scalarRangeMax = 1.0;
	double m_scalarShift = 0.0;  // shift applied by shiftScaleFilter
	double m_scalarScale = 1.0;  // scale applied by shiftScaleFilter
	void computeShiftScaleFromInput(vtkImageData* image);

	bool m_imageInitialized = false;

	// Retained baseline WL in native image domain
	double m_baselineWindowNative = std::numeric_limits<double>::quiet_NaN();
	double m_baselineLevelNative = std::numeric_limits<double>::quiet_NaN();

	// Orientation marker state (VTK)
	vtkSmartPointer<vtkOrientationMarkerWidget> m_orientationWidget;
	vtkSmartPointer<vtkPropAssembly>             m_orientationAssembly; // cube + axes
	vtkSmartPointer<vtkActor>                    m_orientationCubeActor;
	// Overlay renderer used to draw a small orientation marker without affecting the main renderer's camera.
	vtkSmartPointer<vtkRenderer>                 m_orientationRenderer;
	bool                                         m_orientationMarkerVisible = true;

	// Ensure the marker is created once the interactor is available
	void ensureOrientationMarkerInitialized();
};


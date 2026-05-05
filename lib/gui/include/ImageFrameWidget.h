#pragma once
#include "SelectionFrameWidget.h"

#include <QWidget>
#include <QColor>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include <vtkSmartPointer.h>

class vtkActor;
class vtkAlgorithm;
class vtkAlgorithmOutput;
class vtkDataObject;
class vtkGenericOpenGLRenderWindow;
class vtkImageData;
class vtkImageShiftScale;
class vtkOrientationMarkerWidget;
class vtkProp;
class vtkPropAssembly;
class vtkRenderer;
class vtkRenderWindow;

class ImageFrameWidget : public SelectionFrameWidget
{
	Q_OBJECT
		// Properties
		Q_PROPERTY(ViewOrientation viewOrientation READ viewOrientation WRITE setViewOrientation NOTIFY viewOrientationChanged)
		Q_PROPERTY(Interpolation interpolation READ interpolation WRITE setInterpolation NOTIFY interpolationChanged)

		// New visual properties for background/foreground gradient control
		Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY backgroundColorChanged)
		Q_PROPERTY(QColor foregroundColor READ foregroundColor WRITE setForegroundColor NOTIFY foregroundColorChanged)
		Q_PROPERTY(bool gradientBackground READ gradientBackground WRITE setGradientBackground NOTIFY gradientBackgroundChanged)

public:
	enum Interpolation { Nearest, Linear, Cubic };
	Q_ENUM(Interpolation)

		enum ViewOrientation { VIEW_ORIENTATION_YZ = 0, VIEW_ORIENTATION_XZ = 1, VIEW_ORIENTATION_XY = 2 };
	Q_ENUM(ViewOrientation)

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
	virtual void setImageData(vtkImageData* image);
	vtkImageData* imageData() const { return m_imageData; }

	// Pipeline-aware setter: attach a producer port to this view's internal pipeline.
	// Default implementation connects the port to `m_shiftScaleFilter`.
	virtual void setInputConnection(vtkAlgorithmOutput* port, bool newImg = true);

	// Abstract hook: views implement with their own pipeline logic
	// The bus uses native domain (original image scalar domain).
	virtual void setColorWindowLevel(double window, double level) {};

	// Return the canonical orientation when the main camera's view-normal is within
	// `maxAngleDeg` degrees of a principal axis. Returns one of ViewOrientation values
	// (VIEW_ORIENTATION_YZ=0, VIEW_ORIENTATION_XZ=1, VIEW_ORIENTATION_XY=2) or -1 if none match.
	int cameraAlignedOrientation(double maxAngleDeg) const;

	// helpers to convert baseline WL to mapped domain
	void setBaselineWindowLevel(double windowNative, double levelNative);
	std::pair<double, double> mapWindowLevelToMapped(double windowNative, double levelNative) const;
	std::pair<double, double> baselineMapped() const;

	// Orientation marker control (wireframe cube + positive-axis halves)
	void setOrientationMarkerVisible(bool visible);
	bool orientationMarkerVisible() const { return m_orientationMarkerVisible; }

	// New: background/foreground accessors that operate on the underlying vtkRenderer.
	// Implemented inline for convenience and to mirror jswqAbstractView style.
	void setBackgroundColor(const QColor& c);
	QColor backgroundColor() const;
	void setForegroundColor(const QColor& c);
	QColor foregroundColor() const;
	void setGradientBackground(bool on);
	bool gradientBackground() const;

	vtkRenderWindow* renderWindow() const;
	vtkRenderer* renderer() const;

	// ── Named auxiliary prop management ──────────────────────────────────────
	//
	// Auxiliary props are arbitrary vtkProp instances (actors, volumes,
	// assemblies, text actors, legend scales, etc.) fully integrated into the
	// renderer's 3D world.  Props are grouped under a caller-chosen string key
	// so they can be managed as a unit.  Applies to both 2D and 3D props.
	//
	// The primary visualization pipeline of each derived class (vtkVolume,
	// vtkImageSlice, ortho-planes) is entirely separate and is never affected
	// by these operations.
	//
	// Example keys: "pca_axes", "landmark_spheres", "island_surfaces", "seeds"
	//
	// All methods must be called on the GUI thread.

	// Append one prop to the named group and add it to the renderer immediately.
	// Creates the group if it does not already exist.
	void addAuxProp(const std::string& key, vtkSmartPointer<vtkProp> prop);

	// Atomically replace the entire named group.
	// Existing props for the key are removed from the renderer first.
	// Passing an empty vector is equivalent to removeAuxProps(key).
	void setAuxProps(const std::string& key,
					 std::vector<vtkSmartPointer<vtkProp>> props);

	// Remove all props under the given key from the renderer and erase the group.
	// No-op if the key does not exist.
	void removeAuxProps(const std::string& key);

	// Remove every group from the renderer and clear the map.
	void clearAuxProps();

	// Show or hide all props in the named group without removing them.
	// No-op if the key does not exist.
	void setAuxPropsVisible(const std::string& key, bool visible);

	// Returns true when the key exists and contains at least one prop.
	bool hasAuxProps(const std::string& key) const;

	// Returns the number of props registered under the key (0 if absent).
	int auxPropCount(const std::string& key) const;

public slots:
	virtual void updateData() {};

	// Refresh the rendering endpoint when the upstream image content changed
	// but the input connection remains the same. Default implementation:
	//  - captures main camera,
	//  - calls captureDerivedViewState() (hook for derived classes to save slice/WL),
	//  - updates m_imageData/pipeline (computeShiftScaleFromInput/cacheImageGeometry),
	//  - restores camera and calls restoreDerivedViewState(),
	//  - renders the result.
	//
	// Derived classes override captureDerivedViewState()/restoreDerivedViewState()
	// to preserve view-specific state (current slice, window/level, etc.).
	virtual void refreshEndpointFromUpstream();

	// Per-view settings I/O. Default implementations read/write common view keys.
	// Subclasses may override to add view-specific keys.
	virtual void readSettings();
	virtual void writeSettings() const;

signals:
	void viewOrientationChanged(ViewOrientation);
	void interpolationChanged(Interpolation);
	void windowLevelChanged(double window, double level);

	// New signals for property change notification
	void backgroundColorChanged(const QColor& c);
	void foregroundColorChanged(const QColor& c);
	void gradientBackgroundChanged(bool on);

	// Notifies listeners when the image extents (voxel indices) changed on this view.
	// Format: xMin, xMax, yMin, yMax, zMin, zMax
	void imageExtentsChanged(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);

protected:
	// Helper to install the scene content into the SelectionFrameWidget body.
	void setSceneContent(QWidget* content) { setCentralWidget(content); }

	// Camera helpers with safe defaults (shared by derived classes).
	virtual void resetCamera();
	virtual void rotateCamera(double degrees) {}

	// Hooks for derived classes to save/restore per-view transient state
	// (slice number, window/level, mapping state). Default implementations are no-ops.
	virtual void captureDerivedViewState() {}
	virtual void restoreDerivedViewState() {}

	// Hook from SelectionFrameWidget to gate VTK interactivity on selection
	void onSelectionChanged(bool selected) override;

	// Optional: allow derived classes to adjust default renderer config.
	virtual void initializeRendererDefaults();

	// Map orientation <-> label
	QString orientationLabel(ViewOrientation orient) const;
	ViewOrientation labelToOrientation(const QString& label) const;

	// for derived classes that set m_viewOrientation directly
	void notifyViewOrientationChanged();

	ViewOrientation  m_viewOrientation = VIEW_ORIENTATION_XY;
	Interpolation    m_interpolation = Linear;

	vtkSmartPointer<vtkImageData>                   m_imageData;
	vtkSmartPointer<vtkRenderer>                    m_renderer;
	vtkSmartPointer<vtkGenericOpenGLRenderWindow>   m_renderWindow;
	vtkSmartPointer<vtkImageShiftScale>             m_shiftScaleFilter;

	// Mapping info derived from input
	int    m_nativeScalarType = -1;
	double m_scalarRangeMin = 0.0;
	double m_scalarRangeMax = 255.0;
	double m_mappedDataMin = 0.0;
	double m_mappedDataMax = 255.0;

	double m_scalarShift = 0.0;  // shift applied by shiftScaleFilter
	double m_scalarScale = 1.0;  // scale applied by shiftScaleFilter
	void computeShiftScaleFromInput();

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

	void cacheImageGeometry();
	int m_extent[6];
	double m_spacing[3];
	double m_origin[3];

	vtkImageData* upstreamInputImage() const;

	// Map widget objectName() -> canonical settings key.
	// Default implements heuristics for YZ/XZ/XY/volume. Subclasses may override.
	virtual QString settingsGroupKey() const;

private:

	// Keep a reference to the upstream producer so its output port stays valid.
	vtkSmartPointer<vtkAlgorithm> m_upstreamProducer;

	// Synchronize m_imageData with whatever is connected to m_shiftScaleFilter.
	// This will set m_imageData to the vtkImageData produced by the upstream producer
	// or the raw input data object if SetInputData was used.
	void refreshImageDataFromPipeline();

	// ── Named auxiliary prop storage ──────────────────────────────────────────
	// key -> ordered list of props managed as a named group.
	// Entirely separate from the primary visualization pipeline members.
	// Declared last so it is destroyed first, before m_renderer is released.
	std::unordered_map<std::string, std::vector<vtkSmartPointer<vtkProp>>> m_auxProps;
};

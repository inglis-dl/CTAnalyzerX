#ifndef vtkImageOrthoPlanes_h
#define vtkImageOrthoPlanes_h

#include <vtkProp3D.h>
#include <vtkSmartPointer.h>

/**
 * @class   vtkImageOrthoPlanes
 * @brief   Convenience prop to display three orthogonal image slices with outlines
 *
 * Brief:
 * Manages three orthogonal image slices (X, Y, Z) using vtkImageSlice and
 * vtkImageSliceMapper and presents them together via a vtkPropAssembly.
 * Provides an owned vtkImageProperty with optional shared image property,
 * window/level controls, interpolation settings, per-plane visibility,
 * and computation of world-space bounds based on the underlying vtkImageData.
 * Outline actors for each plane are exposed for styling.
 *
 * Full summary:
 * vtkImageOrthoPlanes is a high-level rendering helper intended to simplify
 * displaying orthogonal slice views of a vtkImageData volume within VTK
 * renderers. The class internally constructs three vtkImageSlice instances and
 * their corresponding vtkImageSliceMapper objects for each principal axis and
 * wraps them into a single vtkPropAssembly so they behave as one renderable
 * prop. Input can be provided as a pipeline connection (SetInputConnection)
 * or as a direct vtkImageData (SetInputData). Window/level and interpolation
 * settings apply to the effective image property (shared or owned). The
 * Update() method computes accurate world-space outlines for each visible
 * plane by transforming integer image indices to physical coordinates and
 * caches the union bounds; GetBounds() returns these cached bounds.
 *
 * @sa vtkImageSlice vtkImageSliceMapper vtkImageProperty vtkPropAssembly
 */

class vtkImageSliceMapper;
class vtkImageSlice;
class vtkImageProperty;
class vtkPolyData;
class vtkActor;
class vtkPropAssembly;
class vtkAlgorithmOutput;
class vtkImageData;
class vtkViewport;
class vtkWindow;

class vtkImageOrthoPlanes : public vtkProp3D
{
public:
	static vtkImageOrthoPlanes* New();
	vtkTypeMacro(vtkImageOrthoPlanes, vtkProp3D);
	void PrintSelf(ostream& os, vtkIndent indent) override;

	// Input wiring (pipeline or direct image)
	void SetInputConnection(vtkAlgorithmOutput* port);
	void SetInputData(vtkImageData* image);

	// Shared property support
	void SetSharedImageProperty(vtkImageProperty* prop);
	vtkImageProperty* GetSharedImageProperty();

	// Window/level API (applies to shared or owned property)
	void SetColorWindow(double window);
	void SetColorLevel(double level);
	void SetColorWindowLevel(double window, double level);
	double GetColorWindow() const;
	double GetColorLevel() const;

	// Native-window/level helper (mapped = (native + shift) * scale)
	void SetWindowLevelNative(double windowNative, double levelNative, double shift, double scale);

	// Slice control: set all three slice indices at once (image index space)
	void SetSliceNumbers(int x, int y, int z);
	void SetSliceNumbers(const int center[3]);

	// Get current slice numbers
	void GetSliceNumbers(int center[3]) const;

	// Set center using a world/physical coordinate (point of intersection of the three planes).
	// The implementation will map worldPt -> continuous image indices and set discrete slice numbers.
	void SetCenterWorld(const double worldPt[3]);

	// Interpolation helpers (applied to effective property)
	void SetInterpolationToNearest();
	void SetInterpolationToLinear();
	void SetInterpolationToCubic();

	// Per-plane visibility
	void SetPlaneVisibility(bool arg) { SetPlaneVisibility(arg, arg, arg); };
	void SetPlaneVisibility(bool vx, bool vy, bool vz);
	void SetPlaneVisibilityX(bool on);
	void SetPlaneVisibilityY(bool on);
	void SetPlaneVisibilityZ(bool on);

	// Access outline actor for styling
	vtkActor* GetOutlineActorX();
	vtkActor* GetOutlineActorY();
	vtkActor* GetOutlineActorZ();

	// Update internal geometry / bounds (call after upstream changes)
	void Update();

	// vtkProp overrides delegated to internal assembly
	int RenderOpaqueGeometry(vtkViewport* viewport) override;
	int RenderTranslucentPolygonalGeometry(vtkViewport* viewport) override;
	int RenderOverlay(vtkViewport* viewport) override;
	int HasTranslucentPolygonalGeometry() override;
	void ReleaseGraphicsResources(vtkWindow* w) override;
	double* GetBounds() override;

	// Effective image property (shared if set, otherwise owned)
	vtkImageProperty* GetEffectiveImageProperty() const;

protected:
	vtkImageOrthoPlanes();
	~vtkImageOrthoPlanes() override;

private:
	vtkImageOrthoPlanes(const vtkImageOrthoPlanes&) = delete;
	void operator=(const vtkImageOrthoPlanes&) = delete;

	// Three orthogonal mappers / slices
	vtkSmartPointer<vtkImageSliceMapper> m_mapperX;
	vtkSmartPointer<vtkImageSliceMapper> m_mapperY;
	vtkSmartPointer<vtkImageSliceMapper> m_mapperZ;

	vtkSmartPointer<vtkImageSlice> m_sliceX;
	vtkSmartPointer<vtkImageSlice> m_sliceY;
	vtkSmartPointer<vtkImageSlice> m_sliceZ;

	// Owned property and optional shared property
	vtkSmartPointer<vtkImageProperty> m_ownedProperty;
	vtkSmartPointer<vtkImageProperty> m_sharedProperty;

	// Outline geometry + actors per plane
	vtkSmartPointer<vtkPolyData> m_outlinePolyX;
	vtkSmartPointer<vtkPolyData> m_outlinePolyY;
	vtkSmartPointer<vtkPolyData> m_outlinePolyZ;

	vtkSmartPointer<vtkActor> m_outlineActorX;
	vtkSmartPointer<vtkActor> m_outlineActorY;
	vtkSmartPointer<vtkActor> m_outlineActorZ;

	// Single assembly presenting all slices + outlines
	vtkSmartPointer<vtkPropAssembly> m_assembly;

	// Cached bounds
	double m_bounds[6];

	// Internal helpers
	void UpdateOutlineFromBounds(vtkPolyData* poly, const double bounds[6], int planeAxis);
	vtkImageData* GetImageDataFromMappers() const;
	void SetSliceNumberForAxes(int sx, int sy, int sz);
};

#endif // vtkImageOrthoPlanes_h

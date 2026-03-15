#ifndef vtkImageSlicePointPlacer_h
#define vtkImageSlicePointPlacer_h

#include <vtkPointPlacer.h>
#include <vtkSmartPointer.h>

class vtkBoundedPlanePointPlacer;
class vtkImageSliceMapper;
class vtkImageSlice;

/// Custom point placer that constrains seed placement to the visible image slice.
/// This ensures seeds are placed only on the 2D image plane, not in empty space.
/// Uses vtkBoundedPlanePointPlacer internally for robust bounds enforcement.
class vtkImageSlicePointPlacer : public vtkPointPlacer
{
public:
	static vtkImageSlicePointPlacer* New();
	vtkTypeMacro(vtkImageSlicePointPlacer, vtkPointPlacer);
	void PrintSelf(ostream& os, vtkIndent indent) override;

	/// Set the image slice mapper to constrain placement to its bounds
	virtual void SetImageSliceMapper(vtkImageSliceMapper* mapper);
	vtkGetObjectMacro(ImageSliceMapper, vtkImageSliceMapper);

	/// Set the image slice actor (optional, for more precise picking)
	virtual void SetImageSlice(vtkImageSlice* slice);
	vtkGetObjectMacro(ImageSlice, vtkImageSlice);

	/// Compute world position constrained to the current slice plane
	int ComputeWorldPosition(vtkRenderer* ren,
						   double displayPos[2],
						   double worldPos[3],
						   double worldOrient[9]) override;

	/// Compute world position with reference position constrained to the current slice plane
	int ComputeWorldPosition(vtkRenderer* ren,
					   double displayPos[2],
					   double* refWorldPos,
					   double worldPos[3],
					   double worldOrient[9]) override;

	/// Validate that display position is within image bounds
	int ValidateDisplayPosition(vtkRenderer* ren, double displayPos[2]) override;

	/// Validate world position is on the current slice
	int ValidateWorldPosition(double worldPos[3]) override;

	/// Validate world position with orientation
	int ValidateWorldPosition(double worldPos[3], double* worldOrient) override;

	/// Update internal plane equation when slice changes
	int UpdateWorldPosition(vtkRenderer* ren,
						  double worldPos[3],
						  double worldOrient[9]) override;

	/// Set world tolerance (delegates to internal placer)
	void SetWorldTolerance(double tol) override;

	/// Set optional bounds to further constrain placement (useful for ROI)
	/// Set to VTK_DOUBLE_MAX/MIN to use full image bounds
	void SetBounds(double xmin, double xmax, double ymin, double ymax, double zmin, double zmax);
	void SetBounds(double bounds[6]);
	double* GetBounds();
	void GetBounds(double bounds[6]);

protected:
	vtkImageSlicePointPlacer();
	~vtkImageSlicePointPlacer() override;

	/// Update internal bounded plane placer state when slice/bounds change
	int UpdateInternalState();

	vtkImageSliceMapper* ImageSliceMapper;
	vtkImageSlice* ImageSlice;
	vtkBoundedPlanePointPlacer* Placer;

	// Cached state to detect changes
	double SavedBounds[6];
	int SavedSliceNumber;
	int SavedOrientation;

	// Optional user-specified bounds (initialized to extrema = use full image bounds)
	double Bounds[6];

private:
	vtkImageSlicePointPlacer(const vtkImageSlicePointPlacer&) = delete;
	void operator=(const vtkImageSlicePointPlacer&) = delete;
};

#endif // vtkImageSlicePointPlacer_h
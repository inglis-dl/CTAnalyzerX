#ifndef vtkSliceOutlineSource_h
#define vtkSliceOutlineSource_h

#include "vtkPolyDataAlgorithm.h"
#include "vtkSmartPointer.h"

class vtkImageSliceMapper;
class vtkPolyData;
class vtkPoints;
class vtkCellArray;

class vtkSliceOutlineSource : public vtkPolyDataAlgorithm
{
public:
	static vtkSliceOutlineSource* New();
	vtkTypeMacro(vtkSliceOutlineSource, vtkPolyDataAlgorithm);
	void PrintSelf(ostream& os, vtkIndent indent) override;

	///@{
	/// Provide a slice mapper whose bounds reflect cropping; preferred source for bounds.
	virtual void SetSliceMapper(vtkImageSliceMapper* mapper);
	vtkImageSliceMapper* GetSliceMapper() { return this->SliceMapper; }
	///@}

	/// If no mapper provided, outline will use explicit bounds set by SetBounds(...)
	void SetBounds(double xmin, double xmax, double ymin, double ymax, double zmin, double zmax);
	void GetBounds(double outBounds[6]);

protected:
	vtkSliceOutlineSource();
	~vtkSliceOutlineSource() override;

	int RequestData(vtkInformation* request,
					vtkInformationVector** inputVector,
					vtkInformationVector* outputVector) override;

	vtkImageSliceMapper* SliceMapper;
	double Bounds[6];
	bool HaveExplicitBounds;

private:
	vtkSliceOutlineSource(const vtkSliceOutlineSource&) = delete;
	void operator=(const vtkSliceOutlineSource&) = delete;
};

#endif // vtkSliceOutlineSource_h

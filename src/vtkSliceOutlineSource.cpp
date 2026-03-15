#include "vtkSliceOutlineSource.h"

#include <vtkImageSliceMapper.h>
#include <vtkPolyData.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkSmartPointer.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkObjectFactory.h>
#include <vtkDataObject.h>
#include <vtkStreamingDemandDrivenPipeline.h>

vtkStandardNewMacro(vtkSliceOutlineSource);

vtkSliceOutlineSource::vtkSliceOutlineSource()
	: SliceMapper(nullptr)
	, HaveExplicitBounds(false)
{
	this->SetNumberOfInputPorts(0);
	for (int i = 0; i < 6; ++i) this->Bounds[i] = 0.0;
}

vtkSliceOutlineSource::~vtkSliceOutlineSource()
{
	if (this->SliceMapper) {
		this->SliceMapper->UnRegister(this);
		this->SliceMapper = nullptr;
	}
}

void vtkSliceOutlineSource::SetSliceMapper(vtkImageSliceMapper* mapper)
{
	if (this->SliceMapper == mapper) return;
	if (this->SliceMapper) {
		this->SliceMapper->UnRegister(this);
	}
	this->SliceMapper = mapper;
	if (this->SliceMapper) {
		this->SliceMapper->Register(this);
	}
	this->Modified();
}

void vtkSliceOutlineSource::SetBounds(double xmin, double xmax, double ymin, double ymax, double zmin, double zmax)
{
	this->Bounds[0] = xmin;
	this->Bounds[1] = xmax;
	this->Bounds[2] = ymin;
	this->Bounds[3] = ymax;
	this->Bounds[4] = zmin;
	this->Bounds[5] = zmax;
	this->HaveExplicitBounds = true;
	this->Modified();
}

void vtkSliceOutlineSource::GetBounds(double outBounds[6])
{
	for (int i = 0; i < 6; ++i) outBounds[i] = this->Bounds[i];
}

void vtkSliceOutlineSource::PrintSelf(ostream& os, vtkIndent indent)
{
	this->Superclass::PrintSelf(os, indent);
	os << indent << "HaveExplicitBounds: " << (this->HaveExplicitBounds ? "On\n" : "Off\n");
	os << indent << "Bounds: "
		<< this->Bounds[0] << " " << this->Bounds[1] << " "
		<< this->Bounds[2] << " " << this->Bounds[3] << " "
		<< this->Bounds[4] << " " << this->Bounds[5] << "\n";
}

int vtkSliceOutlineSource::RequestData(vtkInformation* vtkNotUsed(request),
									  vtkInformationVector** vtkNotUsed(inputVector),
									  vtkInformationVector* outputVector)
{
	vtkInformation* outInfo = outputVector->GetInformationObject(0);
	vtkPolyData* output = vtkPolyData::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

	double b[6];
	bool have = false;
	if (this->SliceMapper) {
		// Ensure mapper information is up-to-date (safe no-op if not required)
		// Note: do not call Update() on the whole pipeline here to avoid side-effects.
		double const* mb = this->SliceMapper->GetBounds();
		if (mb) {
			for (int i = 0; i < 6; ++i) b[i] = mb[i];
			have = true;
		}
	}
	if (!have && this->HaveExplicitBounds) {
		for (int i = 0; i < 6; ++i) b[i] = this->Bounds[i];
		have = true;
	}

	if (!have) {
		// nothing to produce, return empty polydata
		output->Initialize();
		return 1;
	}

	// Compute extents (lengths) per axis
	double len[3] = { fabs(b[1] - b[0]), fabs(b[3] - b[2]), fabs(b[5] - b[4]) };

	// Choose slice axis.
	// Prefer the mapper's declared orientation (when available) because it
	// explicitly indicates which axis is the collapsed/slice direction.
	// Fall back to the smallest-extent heuristic only when no mapper is provided.
	int sliceAxis = 0;
	if (this->SliceMapper) {
		sliceAxis = this->SliceMapper->GetOrientation() % 3;
	}
	else {
		if (len[1] < len[sliceAxis]) sliceAxis = 1;
		if (len[2] < len[sliceAxis]) sliceAxis = 2;
	}

	// If all extents are non-zero but one is significantly smaller, still pick that one.
	// Use the midpoint for the slice coordinate (robust when min==max).
	double sliceCoord = 0.5 * (b[2 * sliceAxis] + b[2 * sliceAxis + 1]);

	// Determine the two in-plane axes u,v
	int u = (sliceAxis + 1) % 3;
	int v = (sliceAxis + 2) % 3;

	double uMin = b[2 * u];
	double uMax = b[2 * u + 1];
	double vMin = b[2 * v];
	double vMax = b[2 * v + 1];

	// Build four corner points in world coordinates (x,y,z order)
	vtkSmartPointer<vtkPoints> pts = vtkSmartPointer<vtkPoints>::New();
	double p[3];

	// corner 0: (uMin, vMin)
	p[0] = p[1] = p[2] = 0.0;
	p[u] = uMin; p[v] = vMin; p[sliceAxis] = sliceCoord;
	pts->InsertNextPoint(p[0], p[1], p[2]);

	// corner 1: (uMax, vMin)
	p[u] = uMax; p[v] = vMin; p[sliceAxis] = sliceCoord;
	pts->InsertNextPoint(p[0], p[1], p[2]);

	// corner 2: (uMax, vMax)
	p[u] = uMax; p[v] = vMax; p[sliceAxis] = sliceCoord;
	pts->InsertNextPoint(p[0], p[1], p[2]);

	// corner 3: (uMin, vMax)
	p[u] = uMin; p[v] = vMax; p[sliceAxis] = sliceCoord;
	pts->InsertNextPoint(p[0], p[1], p[2]);

	// closed polyline (4 + repeat first)
	vtkSmartPointer<vtkCellArray> lines = vtkSmartPointer<vtkCellArray>::New();
	vtkIdType ids[5] = { 0,1,2,3,0 };
	lines->InsertNextCell(5, ids);

	output->SetPoints(pts);
	output->SetLines(lines);
	output->Modified();
	return 1;
}

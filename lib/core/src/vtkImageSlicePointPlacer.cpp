#include "vtkImageSlicePointPlacer.h"

#include <vtkBoundedPlanePointPlacer.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageSlice.h>
#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkPlane.h>
#include <vtkObjectFactory.h>

vtkStandardNewMacro(vtkImageSlicePointPlacer);

//------------------------------------------------------------------------------
vtkImageSlicePointPlacer::vtkImageSlicePointPlacer()
	: ImageSliceMapper(nullptr)
	, ImageSlice(nullptr)
	, SavedSliceNumber(-1)
	, SavedOrientation(-1)
{
	// Create internal bounded plane placer
	this->Placer = vtkBoundedPlanePointPlacer::New();

	// Initialize saved bounds to trigger update on first call
	for (int i = 0; i < 6; ++i)
	{
		this->SavedBounds[i] = 0.0;
	}

	// Initialize optional bounds to extrema (use full image bounds)
	this->Bounds[0] = this->Bounds[2] = this->Bounds[4] = VTK_DOUBLE_MAX;
	this->Bounds[1] = this->Bounds[3] = this->Bounds[5] = VTK_DOUBLE_MIN;
}

//------------------------------------------------------------------------------
vtkImageSlicePointPlacer::~vtkImageSlicePointPlacer()
{
	this->SetImageSliceMapper(nullptr);
	this->SetImageSlice(nullptr);
	this->Placer->Delete();
}

//------------------------------------------------------------------------------
void vtkImageSlicePointPlacer::SetImageSliceMapper(vtkImageSliceMapper* mapper)
{
	if (this->ImageSliceMapper != mapper)
	{
		vtkImageSliceMapper* temp = this->ImageSliceMapper;
		this->ImageSliceMapper = mapper;
		if (this->ImageSliceMapper) this->ImageSliceMapper->Register(this);
		if (temp) temp->UnRegister(this);
		this->Modified();

		// Force update on next use
		this->SavedSliceNumber = -1;
		this->SavedOrientation = -1;
	}
}

//------------------------------------------------------------------------------
void vtkImageSlicePointPlacer::SetImageSlice(vtkImageSlice* slice)
{
	if (this->ImageSlice != slice)
	{
		vtkImageSlice* temp = this->ImageSlice;
		this->ImageSlice = slice;
		if (this->ImageSlice) this->ImageSlice->Register(this);
		if (temp) temp->UnRegister(this);
		this->Modified();
	}
}

//------------------------------------------------------------------------------
int vtkImageSlicePointPlacer::UpdateInternalState()
{
	if (!this->ImageSliceMapper)
	{
		return 0;
	}

	vtkImageData* input = this->ImageSliceMapper->GetInput();
	if (!input)
	{
		return 0;
	}

	// Get current slice state
	int orientation = this->ImageSliceMapper->GetOrientation();
	int sliceNum = this->ImageSliceMapper->GetSliceNumber();

	// Get image geometry
	double spacing[3];
	double origin[3];
	input->GetSpacing(spacing);
	input->GetOrigin(origin);

	// Get image bounds and apply optional user bounds
	double bounds[6];
	input->GetBounds(bounds);

	if (this->Bounds[0] != VTK_DOUBLE_MAX)
	{
		bounds[0] = (bounds[0] < this->Bounds[0]) ? this->Bounds[0] : bounds[0];
		bounds[1] = (bounds[1] > this->Bounds[1]) ? this->Bounds[1] : bounds[1];
		bounds[2] = (bounds[2] < this->Bounds[2]) ? this->Bounds[2] : bounds[2];
		bounds[3] = (bounds[3] > this->Bounds[3]) ? this->Bounds[3] : bounds[3];
		bounds[4] = (bounds[4] < this->Bounds[4]) ? this->Bounds[4] : bounds[4];
		bounds[5] = (bounds[5] > this->Bounds[5]) ? this->Bounds[5] : bounds[5];
	}

	// Determine projection axis and position based on orientation
	int axis;
	double position;

	switch (orientation)
	{
		case 0: // YZ plane (X normal)
		axis = vtkBoundedPlanePointPlacer::XAxis;
		position = origin[0] + sliceNum * spacing[0];
		break;
		case 1: // XZ plane (Y normal)
		axis = vtkBoundedPlanePointPlacer::YAxis;
		position = origin[1] + sliceNum * spacing[1];
		break;
		case 2: // XY plane (Z normal)
		default:
		axis = vtkBoundedPlanePointPlacer::ZAxis;
		position = origin[2] + sliceNum * spacing[2];
		break;
	}

	// Check if state has changed
	bool needsUpdate = false;

	if (axis != this->SavedOrientation ||
		sliceNum != this->SavedSliceNumber)
	{
		needsUpdate = true;
	}

	for (int i = 0; i < 6; ++i)
	{
		if (bounds[i] != this->SavedBounds[i])
		{
			needsUpdate = true;
			this->SavedBounds[i] = bounds[i];
		}
	}

	// Update internal placer only if needed
	if (needsUpdate)
	{
		this->SavedOrientation = axis;
		this->SavedSliceNumber = sliceNum;

		// Configure projection plane
		this->Placer->SetProjectionNormal(axis);
		this->Placer->SetProjectionPosition(position);

		// Clear existing bounding planes
		this->Placer->RemoveAllBoundingPlanes();

		// Add 6 bounding planes (2 per axis, excluding projection axis)
		vtkPlane* plane;

		if (axis != vtkBoundedPlanePointPlacer::XAxis)
		{
			// X-min plane
			plane = vtkPlane::New();
			plane->SetOrigin(bounds[0], bounds[2], bounds[4]);
			plane->SetNormal(1.0, 0.0, 0.0);
			this->Placer->AddBoundingPlane(plane);
			plane->Delete();

			// X-max plane
			plane = vtkPlane::New();
			plane->SetOrigin(bounds[1], bounds[3], bounds[5]);
			plane->SetNormal(-1.0, 0.0, 0.0);
			this->Placer->AddBoundingPlane(plane);
			plane->Delete();
		}

		if (axis != vtkBoundedPlanePointPlacer::YAxis)
		{
			// Y-min plane
			plane = vtkPlane::New();
			plane->SetOrigin(bounds[0], bounds[2], bounds[4]);
			plane->SetNormal(0.0, 1.0, 0.0);
			this->Placer->AddBoundingPlane(plane);
			plane->Delete();

			// Y-max plane
			plane = vtkPlane::New();
			plane->SetOrigin(bounds[1], bounds[3], bounds[5]);
			plane->SetNormal(0.0, -1.0, 0.0);
			this->Placer->AddBoundingPlane(plane);
			plane->Delete();
		}

		if (axis != vtkBoundedPlanePointPlacer::ZAxis)
		{
			// Z-min plane
			plane = vtkPlane::New();
			plane->SetOrigin(bounds[0], bounds[2], bounds[4]);
			plane->SetNormal(0.0, 0.0, 1.0);
			this->Placer->AddBoundingPlane(plane);
			plane->Delete();

			// Z-max plane
			plane = vtkPlane::New();
			plane->SetOrigin(bounds[1], bounds[3], bounds[5]);
			plane->SetNormal(0.0, 0.0, -1.0);
			this->Placer->AddBoundingPlane(plane);
			plane->Delete();
		}

		this->Modified();
	}

	return 1;
}

//------------------------------------------------------------------------------
int vtkImageSlicePointPlacer::ComputeWorldPosition(
	vtkRenderer* ren,
	double displayPos[2],
	double worldPos[3],
	double worldOrient[9])
{
	if (!this->UpdateInternalState())
	{
		return 0;
	}

	return this->Placer->ComputeWorldPosition(ren, displayPos, worldPos, worldOrient);
}

//------------------------------------------------------------------------------
int vtkImageSlicePointPlacer::ComputeWorldPosition(
	vtkRenderer* ren,
	double displayPos[2],
	double* refWorldPos,
	double worldPos[3],
	double worldOrient[9])
{
	if (!this->UpdateInternalState())
	{
		return 0;
	}

	return this->Placer->ComputeWorldPosition(ren, displayPos, refWorldPos, worldPos, worldOrient);
}

//------------------------------------------------------------------------------
int vtkImageSlicePointPlacer::ValidateDisplayPosition(vtkRenderer* ren, double displayPos[2])
{
	if (!this->UpdateInternalState())
	{
		return 0;
	}

	return this->Placer->ValidateDisplayPosition(ren, displayPos);
}

//------------------------------------------------------------------------------
int vtkImageSlicePointPlacer::ValidateWorldPosition(double worldPos[3])
{
	if (!this->UpdateInternalState())
	{
		return 0;
	}

	return this->Placer->ValidateWorldPosition(worldPos);
}

//------------------------------------------------------------------------------
int vtkImageSlicePointPlacer::ValidateWorldPosition(double worldPos[3], double* worldOrient)
{
	if (!this->UpdateInternalState())
	{
		return 0;
	}

	return this->Placer->ValidateWorldPosition(worldPos, worldOrient);
}

//------------------------------------------------------------------------------
int vtkImageSlicePointPlacer::UpdateWorldPosition(
	vtkRenderer* ren,
	double worldPos[3],
	double worldOrient[9])
{
	if (!this->UpdateInternalState())
	{
		return 0;
	}

	return this->Placer->UpdateWorldPosition(ren, worldPos, worldOrient);
}

//------------------------------------------------------------------------------
void vtkImageSlicePointPlacer::SetWorldTolerance(double tol)
{
	if (this->WorldTolerance != (tol < 0.0 ? 0.0 : (tol > VTK_DOUBLE_MAX ? VTK_DOUBLE_MAX : tol)))
	{
		this->WorldTolerance = (tol < 0.0 ? 0.0 : (tol > VTK_DOUBLE_MAX ? VTK_DOUBLE_MAX : tol));
		this->Placer->SetWorldTolerance(tol);
		this->Modified();
	}
}

//------------------------------------------------------------------------------
void vtkImageSlicePointPlacer::SetBounds(double xmin, double xmax,
										 double ymin, double ymax,
										 double zmin, double zmax)
{
	if (this->Bounds[0] != xmin || this->Bounds[1] != xmax ||
		this->Bounds[2] != ymin || this->Bounds[3] != ymax ||
		this->Bounds[4] != zmin || this->Bounds[5] != zmax)
	{
		this->Bounds[0] = xmin;
		this->Bounds[1] = xmax;
		this->Bounds[2] = ymin;
		this->Bounds[3] = ymax;
		this->Bounds[4] = zmin;
		this->Bounds[5] = zmax;

		// Force update on next use
		this->SavedSliceNumber = -1;
		this->Modified();
	}
}

//------------------------------------------------------------------------------
void vtkImageSlicePointPlacer::SetBounds(double bounds[6])
{
	this->SetBounds(bounds[0], bounds[1], bounds[2], bounds[3], bounds[4], bounds[5]);
}

//------------------------------------------------------------------------------
double* vtkImageSlicePointPlacer::GetBounds()
{
	return this->Bounds;
}

//------------------------------------------------------------------------------
void vtkImageSlicePointPlacer::GetBounds(double bounds[6])
{
	for (int i = 0; i < 6; ++i)
	{
		bounds[i] = this->Bounds[i];
	}
}

//------------------------------------------------------------------------------
void vtkImageSlicePointPlacer::PrintSelf(ostream& os, vtkIndent indent)
{
	this->Superclass::PrintSelf(os, indent);

	os << indent << "ImageSliceMapper: " << this->ImageSliceMapper << "\n";
	os << indent << "ImageSlice: " << this->ImageSlice << "\n";

	const double* bounds = this->GetBounds();
	if (bounds != nullptr)
	{
		os << indent << "Bounds: \n";
		os << indent << "  Xmin,Xmax: (" << bounds[0] << ", " << bounds[1] << ")\n";
		os << indent << "  Ymin,Ymax: (" << bounds[2] << ", " << bounds[3] << ")\n";
		os << indent << "  Zmin,Zmax: (" << bounds[4] << ", " << bounds[5] << ")\n";
	}
	else
	{
		os << indent << "Bounds: (using full image bounds)\n";
	}

	os << indent << "Saved Slice Number: " << this->SavedSliceNumber << "\n";
	os << indent << "Saved Orientation: " << this->SavedOrientation << "\n";

	os << indent << "Internal Placer: " << this->Placer << "\n";
	if (this->Placer)
	{
		this->Placer->PrintSelf(os, indent.GetNextIndent());
	}
}
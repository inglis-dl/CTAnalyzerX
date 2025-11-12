#include "vtkImageOrthoPlanes.h"

#include <vtkActor.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkImageProperty.h>
#include <vtkPolyData.h>
#include <vtkPoints.h>
#include <vtkCellArray.h>
#include <vtkPolyDataMapper.h>
#include <vtkPropAssembly.h>
#include <vtkAlgorithmOutput.h>
#include <vtkImageData.h>
#include <vtkObjectFactory.h>
#include <vtkViewport.h>
#include <vtkWindow.h>
#include <vtkMath.h>
#include <vtkAlgorithm.h>
#include <vtkProperty.h>
#include <vtkBoundingBox.h> // added for bbox operations

vtkStandardNewMacro(vtkImageOrthoPlanes);

vtkImageOrthoPlanes::vtkImageOrthoPlanes()
{
	// Create mappers + slices
	this->m_mapperX = vtkSmartPointer<vtkImageSliceMapper>::New();
	this->m_mapperY = vtkSmartPointer<vtkImageSliceMapper>::New();
	this->m_mapperZ = vtkSmartPointer<vtkImageSliceMapper>::New();

	this->m_sliceX = vtkSmartPointer<vtkImageSlice>::New();
	this->m_sliceY = vtkSmartPointer<vtkImageSlice>::New();
	this->m_sliceZ = vtkSmartPointer<vtkImageSlice>::New();

	// Ensure each mapper is oriented to the correct axis so slices render on the intended planes.
	// m_mapperX -> X-normal (YZ plane)
	// m_mapperY -> Y-normal (XZ plane)
	// m_mapperZ -> Z-normal (XY plane)
	if (this->m_mapperX) this->m_mapperX->SetOrientationToX();
	if (this->m_mapperY) this->m_mapperY->SetOrientationToY();
	if (this->m_mapperZ) this->m_mapperZ->SetOrientationToZ();

	// Owned image property (used when no shared property provided)
	this->m_ownedProperty = vtkSmartPointer<vtkImageProperty>::New();

	this->m_sliceX->SetMapper(this->m_mapperX);
	this->m_sliceY->SetMapper(this->m_mapperY);
	this->m_sliceZ->SetMapper(this->m_mapperZ);

	this->m_sliceX->SetProperty(this->m_ownedProperty);
	this->m_sliceY->SetProperty(this->m_ownedProperty);
	this->m_sliceZ->SetProperty(this->m_ownedProperty);

	// Outline polys + actors
	auto makeOutline = []() -> vtkSmartPointer<vtkPolyData> {
		auto poly = vtkSmartPointer<vtkPolyData>::New();
		poly->SetPoints(vtkSmartPointer<vtkPoints>::New());
		poly->SetLines(vtkSmartPointer<vtkCellArray>::New());
		return poly;
		};

	this->m_outlinePolyX = makeOutline();
	this->m_outlinePolyY = makeOutline();
	this->m_outlinePolyZ = makeOutline();

	auto makeOutlineActor = [&](vtkSmartPointer<vtkPolyData> poly) -> vtkSmartPointer<vtkActor> {
		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputData(poly);
		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		auto prop = actor->GetProperty();
		prop->SetRepresentationToWireframe();
		prop->SetLineWidth(2.0);
		prop->SetLighting(false);
		prop->SetSpecular(0.0);
		prop->SetDiffuse(0.0);
		prop->SetAmbient(1.0);
		actor->PickableOff();
		return actor;
		};

	this->m_outlineActorX = makeOutlineActor(this->m_outlinePolyX);
	this->m_outlineActorY = makeOutlineActor(this->m_outlinePolyY);
	this->m_outlineActorZ = makeOutlineActor(this->m_outlinePolyZ);

	// Assembly containing all slices + outlines
	this->m_assembly = vtkSmartPointer<vtkPropAssembly>::New();
	this->m_assembly->AddPart(this->m_sliceX);
	this->m_assembly->AddPart(this->m_sliceY);
	this->m_assembly->AddPart(this->m_sliceZ);
	this->m_assembly->AddPart(this->m_outlineActorX);
	this->m_assembly->AddPart(this->m_outlineActorY);
	this->m_assembly->AddPart(this->m_outlineActorZ);

	// initialize bounds empty
	this->m_bounds[0] = this->m_bounds[2] = this->m_bounds[4] = 0.0;
	this->m_bounds[1] = this->m_bounds[3] = this->m_bounds[5] = -1.0;
}

vtkImageOrthoPlanes::~vtkImageOrthoPlanes() = default;

void vtkImageOrthoPlanes::PrintSelf(ostream& os, vtkIndent indent)
{
	this->Superclass::PrintSelf(os, indent);
	os << indent << "vtkImageOrthoPlanes\n";
}

void vtkImageOrthoPlanes::SetInputConnection(vtkAlgorithmOutput* port)
{
	if (port)
	{
		this->m_mapperX->SetInputConnection(port);
		this->m_mapperY->SetInputConnection(port);
		this->m_mapperZ->SetInputConnection(port);
	}
}

void vtkImageOrthoPlanes::SetInputData(vtkImageData* image)
{
	if (image)
	{
		this->m_mapperX->SetInputData(image);
		this->m_mapperY->SetInputData(image);
		this->m_mapperZ->SetInputData(image);
	}
}

void vtkImageOrthoPlanes::SetSharedImageProperty(vtkImageProperty* prop)
{
	if (prop)
	{
		this->m_sharedProperty = prop;
		this->m_sliceX->SetProperty(prop);
		this->m_sliceY->SetProperty(prop);
		this->m_sliceZ->SetProperty(prop);
	}
	else
	{
		this->m_sharedProperty = nullptr;
		this->m_sliceX->SetProperty(this->m_ownedProperty);
		this->m_sliceY->SetProperty(this->m_ownedProperty);
		this->m_sliceZ->SetProperty(this->m_ownedProperty);
	}
}

vtkImageProperty* vtkImageOrthoPlanes::GetSharedImageProperty()
{
	return this->m_sharedProperty;
}

vtkImageProperty* vtkImageOrthoPlanes::GetEffectiveImageProperty() const
{
	return this->m_sharedProperty ? this->m_sharedProperty : this->m_ownedProperty;
}

void vtkImageOrthoPlanes::SetColorWindow(double window)
{
	vtkImageProperty* prop = this->GetEffectiveImageProperty();
	if (prop) prop->SetColorWindow(window);
}

void vtkImageOrthoPlanes::SetColorLevel(double level)
{
	vtkImageProperty* prop = this->GetEffectiveImageProperty();
	if (prop) prop->SetColorLevel(level);
}

void vtkImageOrthoPlanes::SetColorWindowLevel(double window, double level)
{
	vtkImageProperty* prop = this->GetEffectiveImageProperty();
	if (prop)
	{
		prop->SetColorWindow(window);
		prop->SetColorLevel(level);
	}
}

double vtkImageOrthoPlanes::GetColorWindow() const
{
	vtkImageProperty* prop = const_cast<vtkImageOrthoPlanes*>(this)->m_sharedProperty ?
		this->m_sharedProperty.GetPointer() : this->m_ownedProperty.GetPointer();
	return prop ? prop->GetColorWindow() : 0.0;
}

double vtkImageOrthoPlanes::GetColorLevel() const
{
	vtkImageProperty* prop = const_cast<vtkImageOrthoPlanes*>(this)->m_sharedProperty ?
		this->m_sharedProperty.GetPointer() : this->m_ownedProperty.GetPointer();
	return prop ? prop->GetColorLevel() : 0.0;
}

void vtkImageOrthoPlanes::SetWindowLevelNative(double windowNative, double levelNative, double shift, double scale)
{
	double lowerNative = levelNative - 0.5 * std::fabs(windowNative);
	double upperNative = levelNative + 0.5 * std::fabs(windowNative);
	double lowerMapped = (lowerNative + shift) * scale;
	double upperMapped = (upperNative + shift) * scale;
	double mappedWindow = std::max(upperMapped - lowerMapped, 1.0);
	double mappedLevel = 0.5 * (upperMapped + lowerMapped);
	this->SetColorWindowLevel(mappedWindow, mappedLevel);
}

void vtkImageOrthoPlanes::SetSliceNumbers(int x, int y, int z)
{
	this->SetSliceNumberForAxes(x, y, z);
}

void vtkImageOrthoPlanes::SetSliceNumbers(const int center[3])
{
	if (!center) return;
	this->SetSliceNumberForAxes(center[0], center[1], center[2]);
}

void vtkImageOrthoPlanes::GetSliceNumbers(int center[3]) const
{
	if (!center) return;
	int sx = 0, sy = 0, sz = 0;
	if (this->m_mapperX) sx = this->m_mapperX->GetSliceNumber();
	if (this->m_mapperY) sy = this->m_mapperY->GetSliceNumber();
	if (this->m_mapperZ) sz = this->m_mapperZ->GetSliceNumber();
	center[0] = sx; center[1] = sy; center[2] = sz;
}

void vtkImageOrthoPlanes::SetCenterWorld(const double worldPt[3])
{
	if (!worldPt) return;
	vtkImageData* img = this->GetImageDataFromMappers();
	if (!img) return;

	double contIdx[3] = { 0.0, 0.0, 0.0 };
	// Transform physical/world -> continuous index (instance method)
	img->TransformPhysicalPointToContinuousIndex(worldPt, contIdx);

	// Round to nearest discrete slice index
	int sx = static_cast<int>(std::lround(contIdx[0]));
	int sy = static_cast<int>(std::lround(contIdx[1]));
	int sz = static_cast<int>(std::lround(contIdx[2]));

	this->SetSliceNumberForAxes(sx, sy, sz);
}

void vtkImageOrthoPlanes::SetSliceNumberForAxes(int sx, int sy, int sz)
{
	if (this->m_mapperX) this->m_mapperX->SetSliceNumber(sx);
	if (this->m_mapperY) this->m_mapperY->SetSliceNumber(sy);
	if (this->m_mapperZ) this->m_mapperZ->SetSliceNumber(sz);
}

void vtkImageOrthoPlanes::SetInterpolationToNearest() { auto p = this->GetEffectiveImageProperty(); if (p) p->SetInterpolationTypeToNearest(); }
void vtkImageOrthoPlanes::SetInterpolationToLinear() { auto p = this->GetEffectiveImageProperty(); if (p) p->SetInterpolationTypeToLinear(); }
void vtkImageOrthoPlanes::SetInterpolationToCubic() { auto p = this->GetEffectiveImageProperty(); if (p) p->SetInterpolationTypeToCubic(); }

void vtkImageOrthoPlanes::SetPlaneVisibility(bool vx, bool vy, bool vz)
{
	if (this->m_sliceX) this->m_sliceX->SetVisibility(vx);
	if (this->m_sliceY) this->m_sliceY->SetVisibility(vy);
	if (this->m_sliceZ) this->m_sliceZ->SetVisibility(vz);
	if (this->m_outlineActorX) this->m_outlineActorX->SetVisibility(vx);
	if (this->m_outlineActorY) this->m_outlineActorY->SetVisibility(vy);
	if (this->m_outlineActorZ) this->m_outlineActorZ->SetVisibility(vz);
}

void vtkImageOrthoPlanes::SetPlaneVisibilityX(bool on) { SetPlaneVisibility(on, this->m_sliceY->GetVisibility(), this->m_sliceZ->GetVisibility()); }
void vtkImageOrthoPlanes::SetPlaneVisibilityY(bool on) { SetPlaneVisibility(this->m_sliceX->GetVisibility(), on, this->m_sliceZ->GetVisibility()); }
void vtkImageOrthoPlanes::SetPlaneVisibilityZ(bool on) { SetPlaneVisibility(this->m_sliceX->GetVisibility(), this->m_sliceY->GetVisibility(), on); }

vtkActor* vtkImageOrthoPlanes::GetOutlineActorX() { return this->m_outlineActorX; }
vtkActor* vtkImageOrthoPlanes::GetOutlineActorY() { return this->m_outlineActorY; }
vtkActor* vtkImageOrthoPlanes::GetOutlineActorZ() { return this->m_outlineActorZ; }

void vtkImageOrthoPlanes::Update()
{
	// update each mapper
	if (this->m_mapperX) this->m_mapperX->Update();
	if (this->m_mapperY) this->m_mapperY->Update();
	if (this->m_mapperZ) this->m_mapperZ->Update();

	// Try to obtain underlying image data to compute accurate outlines using index->physical transforms
	vtkImageData* img = this->GetImageDataFromMappers();

	// Use vtkBoundingBox to aggregate per-plane bounds
	vtkBoundingBox bbox;

	// If we have image data, compute outline corners by transforming integer image indices
	if (img)
	{
		const int* extent = img->GetExtent();

		// X-plane (YZ) - i is slice index
		if (this->m_mapperX)
		{
			int sx = this->m_mapperX->GetSliceNumber();
			if (sx >= extent[0] && sx <= extent[1])
			{
				// corners: (sx, jmin, kmin), (sx, jmax, kmin), (sx, jmax, kmax), (sx, jmin, kmax)
				vtkSmartPointer<vtkPoints> pts = vtkSmartPointer<vtkPoints>::New();
				double p[3];
				img->TransformIndexToPhysicalPoint(sx, extent[2], extent[4], p);
				vtkIdType id0 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(sx, extent[3], extent[4], p);
				vtkIdType id1 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(sx, extent[3], extent[5], p);
				vtkIdType id2 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(sx, extent[2], extent[5], p);
				vtkIdType id3 = pts->InsertNextPoint(p);

				vtkSmartPointer<vtkCellArray> cells = vtkSmartPointer<vtkCellArray>::New();
				vtkIdType ids[5] = { id0, id1, id2, id3, id0 };
				cells->InsertNextCell(5, ids);

				this->m_outlinePolyX->SetPoints(pts);
				this->m_outlinePolyX->SetLines(cells);
			}
			else
			{
				this->m_outlinePolyX->Initialize();
			}
		}

		// Y-plane (XZ) - j is slice index
		if (this->m_mapperY)
		{
			int sy = this->m_mapperY->GetSliceNumber();
			if (sy >= extent[2] && sy <= extent[3])
			{
				vtkSmartPointer<vtkPoints> pts = vtkSmartPointer<vtkPoints>::New();
				double p[3];
				img->TransformIndexToPhysicalPoint(extent[0], sy, extent[4], p);
				vtkIdType id0 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(extent[1], sy, extent[4], p);
				vtkIdType id1 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(extent[1], sy, extent[5], p);
				vtkIdType id2 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(extent[0], sy, extent[5], p);
				vtkIdType id3 = pts->InsertNextPoint(p);

				vtkSmartPointer<vtkCellArray> cells = vtkSmartPointer<vtkCellArray>::New();
				vtkIdType ids[5] = { id0, id1, id2, id3, id0 };
				cells->InsertNextCell(5, ids);

				this->m_outlinePolyY->SetPoints(pts);
				this->m_outlinePolyY->SetLines(cells);
			}
			else
			{
				this->m_outlinePolyY->Initialize();
			}
		}

		// Z-plane (XY) - k is slice index
		if (this->m_mapperZ)
		{
			int sz = this->m_mapperZ->GetSliceNumber();
			if (sz >= extent[4] && sz <= extent[5])
			{
				vtkSmartPointer<vtkPoints> pts = vtkSmartPointer<vtkPoints>::New();
				double p[3];
				img->TransformIndexToPhysicalPoint(extent[0], extent[2], sz, p);
				vtkIdType id0 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(extent[1], extent[2], sz, p);
				vtkIdType id1 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(extent[1], extent[3], sz, p);
				vtkIdType id2 = pts->InsertNextPoint(p);
				img->TransformIndexToPhysicalPoint(extent[0], extent[3], sz, p);
				vtkIdType id3 = pts->InsertNextPoint(p);

				vtkSmartPointer<vtkCellArray> cells = vtkSmartPointer<vtkCellArray>::New();
				vtkIdType ids[5] = { id0, id1, id2, id3, id0 };
				cells->InsertNextCell(5, ids);

				this->m_outlinePolyZ->SetPoints(pts);
				this->m_outlinePolyZ->SetLines(cells);
			}
			else
			{
				this->m_outlinePolyZ->Initialize();
			}
		}

		// compute union bounds from transformed corners using vtkBoundingBox
		double bx[6], by[6], bz[6];
		this->m_outlinePolyX->GetBounds(bx);
		this->m_outlinePolyY->GetBounds(by);
		this->m_outlinePolyZ->GetBounds(bz);

		if (vtkBoundingBox::IsValid(bx))
		{
			bbox.AddBounds(bx);
		}
		if (vtkBoundingBox::IsValid(by))
		{
			bbox.AddBounds(by);
		}
		if (vtkBoundingBox::IsValid(bz))
		{
			bbox.AddBounds(bz);
		}

		if (bbox.IsValid())
		{
			bbox.GetBounds(this->m_bounds);
		}
		else
		{
			vtkMath::UninitializeBounds(this->m_bounds);
		}
		return;
	}

	// Fallback: if no image data available, keep previous behavior using slice actors' bounds
	double bx_f[6] = { 0,-1,0,-1,0,-1 };
	double by_f[6] = { 0,-1,0,-1,0,-1 };
	double bz_f[6] = { 0,-1,0,-1,0,-1 };
	if (this->m_sliceX) this->m_sliceX->GetBounds(bx_f);
	if (this->m_sliceY) this->m_sliceY->GetBounds(by_f);
	if (this->m_sliceZ) this->m_sliceZ->GetBounds(bz_f);

	if (vtkBoundingBox::IsValid(bx_f))
	{
		bbox.AddBounds(bx_f);
	}
	if (vtkBoundingBox::IsValid(by_f))
	{
		bbox.AddBounds(by_f);
	}
	if (vtkBoundingBox::IsValid(bz_f))
	{
		bbox.AddBounds(bz_f);
	}

	if (bbox.IsValid())
	{
		bbox.GetBounds(this->m_bounds);
	}
	else
	{
		vtkMath::UninitializeBounds(this->m_bounds);
	}
}

int vtkImageOrthoPlanes::RenderOpaqueGeometry(vtkViewport* viewport)
{
	if (!this->m_assembly) return 0;
	return this->m_assembly->RenderOpaqueGeometry(viewport);
}

int vtkImageOrthoPlanes::RenderTranslucentPolygonalGeometry(vtkViewport* viewport)
{
	if (!this->m_assembly) return 0;
	return this->m_assembly->RenderTranslucentPolygonalGeometry(viewport);
}

int vtkImageOrthoPlanes::RenderOverlay(vtkViewport* viewport)
{
	if (!this->m_assembly) return 0;
	return this->m_assembly->RenderOverlay(viewport);
}

int vtkImageOrthoPlanes::HasTranslucentPolygonalGeometry()
{
	if (!this->m_assembly) return 0;
	return this->m_assembly->HasTranslucentPolygonalGeometry();
}

void vtkImageOrthoPlanes::ReleaseGraphicsResources(vtkWindow* w)
{
	if (this->m_assembly) this->m_assembly->ReleaseGraphicsResources(w);
}

double* vtkImageOrthoPlanes::GetBounds()
{
	if (this->m_assembly)
	{
		double* b = this->m_assembly->GetBounds();
		if (b) {
			for (int i = 0; i < 6; ++i) this->m_bounds[i] = b[i];
		}
	}
	return this->m_bounds;
}

// Helper: attempt to get a vtkImageData backing the mappers' input.
// Checks SetInputData path first, then input connection / producer output.
vtkImageData* vtkImageOrthoPlanes::GetImageDataFromMappers() const
{
	// Try mapper X first (arbitrary)
	auto tryGetFromMapper = [](vtkImageSliceMapper* mapper) -> vtkImageData* {
		if (!mapper) return nullptr;
		// Try direct input (SetInputData)
		vtkImageData* inData = vtkImageData::SafeDownCast(mapper->GetInput());
		if (inData) return inData;
		// Try input connection -> producer -> output data
		vtkAlgorithmOutput* conn = mapper->GetInputConnection(0, 0);
		if (!conn) return nullptr;
		vtkAlgorithm* prod = conn->GetProducer();
		if (!prod) return nullptr;
		prod->UpdateInformation();
		prod->Update();
		vtkDataObject* outObj = prod->GetOutputDataObject(0);
		return vtkImageData::SafeDownCast(outObj);
		};

	vtkImageData* img = tryGetFromMapper(this->m_mapperX);
	if (img) return img;
	img = tryGetFromMapper(this->m_mapperY);
	if (img) return img;
	return tryGetFromMapper(this->m_mapperZ);
}
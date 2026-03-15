#include "vtkTransformRepresentation.h"

#include "vtkAbstractTransform.h"
#include "vtkActor.h"
#include "vtkAxes.h"
#include "vtkBox.h"
#include "vtkCellData.h"
#include "vtkFloatArray.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkLinearTransform.h"
#include "vtkObjectFactory.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkSmartPointer.h"
#include "vtkSphereHandleRepresentation.h"
#include "vtkTransform.h"
#include "vtkTransformPolyDataFilter.h"
#include <vtkMatrix4x4.h>
#include <vtkMath.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>

#include <algorithm>
#include <cstring>
#include <ostream>

vtkStandardNewMacro(vtkTransformRepresentation);

// Note: The file originally included many headers; keep the rest as before.
// We'll patch the implementation to initialize the new flags and guard translations/rotations.




static void ApplyRotationAboutAxis(vtkTransform* transform, const double origin[3], const double axis[3], double angleDeg)
{
	// Translate to origin
	transform->PostMultiply();
	transform->Translate(-origin[0], -origin[1], -origin[2]);
	// Rotate about axis
	transform->RotateWXYZ(angleDeg, axis);
	// Translate back
	transform->Translate(origin[0], origin[1], origin[2]);
}

static void ApplyTranslation(vtkTransform* dynamicTransform, vtkTransform* staticTransform, const double delta[3])
{
	// Apply translation to both transforms
	if (dynamicTransform)
	{
		dynamicTransform->PostMultiply();
		dynamicTransform->Translate(delta);
	}
	if (staticTransform)
	{
		staticTransform->PostMultiply();
		staticTransform->Translate(delta);
	}
}


// Utility: Convert display (x, y, z) to world coordinates using the renderer
static void DisplayToWorldOnZ(vtkRenderer* renderer, double x, double y, double displayZ, double worldPt[3])
{
	if (!renderer)
	{
		worldPt[0] = worldPt[1] = worldPt[2] = 0.0;
		return;
	}
	double displayPt[3] = { x, y, displayZ };
	renderer->SetDisplayPoint(displayPt);
	renderer->DisplayToWorld();
	double* world = renderer->GetWorldPoint();
	if (world[3] != 0.0)
	{
		worldPt[0] = world[0] / world[3];
		worldPt[1] = world[1] / world[3];
		worldPt[2] = world[2] / world[3];
	}
	else
	{
		worldPt[0] = worldPt[1] = worldPt[2] = 0.0;
	}
}

// Utility: Convert world coordinates to display Z using the renderer
static double WorldToDisplayZ(vtkRenderer* renderer, const double worldPt[3])
{
	if (!renderer)
	{
		return 0.0;
	}
	double displayPt[3];
	renderer->SetWorldPoint(worldPt[0], worldPt[1], worldPt[2], 1.0);
	renderer->WorldToDisplay();
	renderer->GetDisplayPoint(displayPt);
	return displayPt[2];
}

//------------------------------------------------------------------------------
// ColorAxesByDominantComponentCell: assign colors to axes polydata cells
// (to match expected/requested behavior, add here rather than modifying vtkAxes)
void ColorAxesByDominantComponentCell(vtkAxes* axes)
{
	if (!axes)
		return;

	// Get the output polydata from the axes source
	axes->Update();
	vtkPolyData* poly = axes->GetOutput();
	if (!poly)
		return;

	// Create a per-cell color array
	vtkSmartPointer<vtkUnsignedCharArray> colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
	colors->SetName("Colors");
	colors->SetNumberOfComponents(3);
	colors->SetNumberOfTuples(poly->GetNumberOfCells());

	// Assign colors: X=red, Y=green, Z=blue (VTK axes order)
	for (vtkIdType i = 0; i < poly->GetNumberOfCells(); ++i)
	{
		unsigned char color[3] = { 255, 255, 255 };
		// For simplicity, assign by cell index: 0=X, 1=Y, 2=Z
		if (i == 0) { color[0] = 255; color[1] = 0;   color[2] = 0; } // X - red
		else if (i == 1) { color[0] = 0;   color[1] = 255; color[2] = 0; } // Y - green
		else if (i == 2) { color[0] = 0;   color[1] = 0;   color[2] = 255; } // Z - blue
		colors->SetTypedTuple(i, color);
	}
	poly->GetCellData()->AddArray(colors);
	poly->GetCellData()->SetScalars(colors);
}


//------------------------------------------------------------------------------
// Constructor
vtkTransformRepresentation::vtkTransformRepresentation()
{
	// Create transforms first
	this->Transform = vtkTransform::New();
	this->Transform->Identity();
	this->StaticTransform = vtkTransform::New();
	this->StaticTransform->Identity();

	// Default enablement for translation and rotation
	this->TranslationEnabled = 1; // ON by default
	this->RotationEnabled = 1;    // ON by default

	// By default, use one of these handles
	this->OriginRepresentation = vtkSphereHandleRepresentation::New();
	this->SelectionXRepresentation = vtkSphereHandleRepresentation::New();
	this->SelectionYRepresentation = vtkSphereHandleRepresentation::New();
	this->SelectionZRepresentation = vtkSphereHandleRepresentation::New();

	// The axes: use vtkAxes (polydata source) + transform filter + mapper + actor
	this->StaticAxes = vtkAxes::New();
	this->StaticAxes->SymmetricOff(); // only positive axes (no synthetic negative cones)
	this->DynamicAxes = vtkAxes::New();
	this->DynamicAxes->SymmetricOff();

	// Color axes geometry per-cell so X=red, Y=green, Z=blue
	ColorAxesByDominantComponentCell(this->StaticAxes);
	ColorAxesByDominantComponentCell(this->DynamicAxes);

	this->StaticAxesFilter = vtkTransformPolyDataFilter::New();
	this->DynamicAxesFilter = vtkTransformPolyDataFilter::New();

	this->StaticAxesMapper = vtkPolyDataMapper::New();
	this->DynamicAxesMapper = vtkPolyDataMapper::New();

	// Configure mapper to use the per-cell "Colors" array as direct scalars
	this->StaticAxesMapper->SetScalarModeToUseCellData();
	this->StaticAxesMapper->SelectColorArray("Colors");
	this->StaticAxesMapper->SetColorModeToDirectScalars();
	this->StaticAxesMapper->ScalarVisibilityOn();

	this->DynamicAxesMapper->SetScalarModeToUseCellData();
	this->DynamicAxesMapper->SelectColorArray("Colors");
	this->DynamicAxesMapper->SetColorModeToDirectScalars();
	this->DynamicAxesMapper->ScalarVisibilityOn();

	this->StaticAxesActor = vtkActor::New();
	this->DynamicAxesActor = vtkActor::New();

	// hook up pipeline: source -> transform(filter) -> mapper -> actor
	this->StaticAxesFilter->SetInputConnection(this->StaticAxes->GetOutputPort());
	this->StaticAxesFilter->SetTransform(this->StaticTransform);
	this->StaticAxesMapper->SetInputConnection(this->StaticAxesFilter->GetOutputPort());
	this->StaticAxesActor->SetMapper(this->StaticAxesMapper);

	this->DynamicAxesFilter->SetInputConnection(this->DynamicAxes->GetOutputPort());
	this->DynamicAxesFilter->SetTransform(this->Transform);
	this->DynamicAxesMapper->SetInputConnection(this->DynamicAxesFilter->GetOutputPort());
	this->DynamicAxesActor->SetMapper(this->DynamicAxesMapper);

	// Default actor properties for visibility (users may override externally)
	// Turn off per-actor lighting/shading/reflection: set ambient-only and disable diffuse/specular.
	this->StaticAxesActor->GetProperty()->SetAmbient(1.0);
	this->StaticAxesActor->GetProperty()->SetDiffuse(0.0);
	this->StaticAxesActor->GetProperty()->SetSpecular(0.0);
	this->StaticAxesActor->GetProperty()->SetInterpolationToFlat();
	this->StaticAxesActor->GetProperty()->LightingOff();

	this->DynamicAxesActor->GetProperty()->SetAmbient(1.0);
	this->DynamicAxesActor->GetProperty()->SetDiffuse(0.0);
	this->DynamicAxesActor->GetProperty()->SetSpecular(0.0);
	this->DynamicAxesActor->GetProperty()->SetInterpolationToFlat();
	this->DynamicAxesActor->GetProperty()->LightingOff();

	// The bounding box
	this->BoundingBox = vtkBox::New();

	this->Tolerance = 5;

	this->InteractionState = Outside;

	// Initialize preserve-handles state
	this->PreserveSelectionHandles = 0;
	this->SavedSelectionTipX[0] = this->SavedSelectionTipX[1] = this->SavedSelectionTipX[2] = 0.0;
	this->SavedSelectionTipY[0] = this->SavedSelectionTipY[1] = this->SavedSelectionTipY[2] = 0.0;
	this->SavedSelectionTipZ[0] = this->SavedSelectionTipZ[1] = this->SavedSelectionTipZ[2] = 0.0;
}


//------------------------------------------------------------------------------
// Destructor
vtkTransformRepresentation::~vtkTransformRepresentation()
{
	this->OriginRepresentation->Delete();
	this->SelectionXRepresentation->Delete();
	this->SelectionYRepresentation->Delete();
	this->SelectionZRepresentation->Delete();

	// delete axes pipeline objects
	this->StaticAxes->Delete();
	this->DynamicAxes->Delete();

	this->StaticAxesFilter->Delete();
	this->DynamicAxesFilter->Delete();

	this->StaticAxesMapper->Delete();
	this->DynamicAxesMapper->Delete();

	this->StaticAxesActor->Delete();
	this->DynamicAxesActor->Delete();

	this->BoundingBox->Delete();

	this->Transform->Delete();
	this->StaticTransform->Delete();
}


//------------------------------------------------------------------------------
// SetOriginWorldPosition: respect TranslationEnabled flag when translating transforms.
void vtkTransformRepresentation::SetOriginWorldPosition(double x[3])
{
	if (this->OriginRepresentation)
	{
		this->OriginRepresentation->SetWorldPosition(x);

		// translate static transform so the static axes follow the origin
		if (this->TranslationEnabled)
		{
			this->StaticTransform->Identity();
			this->StaticTransform->Translate(x);
		}

		// Note: dynamic transform translation is handled by interaction routines
		// if TranslationEnabled is true. If disabled, we intentionally avoid
		// modifying transforms here.
	}
}

// Save current world positions of the selection tip handles.
void vtkTransformRepresentation::SaveSelectionHandleWorldPositions()
{
	if (this->SelectionXRepresentation)
	{
		this->SelectionXRepresentation->GetWorldPosition(this->SavedSelectionTipX);
	}
	if (this->SelectionYRepresentation)
	{
		this->SelectionYRepresentation->GetWorldPosition(this->SavedSelectionTipY);
	}
	if (this->SelectionZRepresentation)
	{
		this->SelectionZRepresentation->GetWorldPosition(this->SavedSelectionTipZ);
	}
}

// Convenience: save current tip positions and mark them to be preserved on the next BuildRepresentation().
void vtkTransformRepresentation::PreserveSelectionHandlesOnce()
{
	this->SaveSelectionHandleWorldPositions();
	this->PreserveSelectionHandles = 1;
}

// Add this static helper function at file scope (above WidgetInteraction or in the utility section)
static void LocalAxis(int idx, double axis[3])
{
	// idx: OnX=2, OnY=3, OnZ=4 (see enum in header)
	// Map to axis: X=(1,0,0), Y=(0,1,0), Z=(0,0,1)
	switch (idx)
	{
		case vtkTransformRepresentation::OnX:
		axis[0] = 1.0; axis[1] = 0.0; axis[2] = 0.0;
		break;
		case vtkTransformRepresentation::OnY:
		axis[0] = 0.0; axis[1] = 1.0; axis[2] = 0.0;
		break;
		case vtkTransformRepresentation::OnZ:
		axis[0] = 0.0; axis[1] = 0.0; axis[2] = 1.0;
		break;
		default:
		axis[0] = axis[1] = axis[2] = 0.0;
		break;
	}
}


//------------------------------------------------------------------------------
// Widget interaction: guard translation and rotation behavior using new flags.
void vtkTransformRepresentation::WidgetInteraction(double e[2])
{
	// Recompute world coordinates on plane of origin using origin's display Z
	if (!this->Renderer)
	{
		return;
	}

	// Get origin world and its display Z
	double origin[3];
	this->GetOriginWorldPosition(origin);
	double originDisplayZ = WorldToDisplayZ(this->Renderer, origin);

	// Compute last world point from last event display coords using origin's display Z
	double lastWorld[3];
	DisplayToWorldOnZ(this->Renderer, this->LastEventPosition[0], this->LastEventPosition[1], originDisplayZ, lastWorld);

	// Compute current world point from incoming event display coords
	double currWorld[3];
	DisplayToWorldOnZ(this->Renderer, e[0], e[1], originDisplayZ, currWorld);

	// Decide behavior based on InteractionState
	if (this->InteractionState == vtkTransformRepresentation::OnOrigin)
	{
		// Translate by cursor delta in world (only if translation enabled)
		if (this->TranslationEnabled)
		{
			double delta[3];
			delta[0] = currWorld[0] - lastWorld[0];
			delta[1] = currWorld[1] - lastWorld[1];
			delta[2] = currWorld[2] - lastWorld[2];

			// Apply translation to the dynamic transform (accumulate)
			if (this->Transform)
			{
				this->Transform->PostMultiply();
				this->Transform->Translate(delta);
			}

			// Make the origin handle match the current world point (authoritative)
			if (this->OriginRepresentation)
			{
				this->OriginRepresentation->SetWorldPosition(currWorld);
			}

			// Ensure static axes are positioned exactly at the new origin (avoid accumulation drift)
			if (this->StaticTransform)
			{
				this->StaticTransform->Identity();
				this->StaticTransform->Translate(currWorld);
			}

			// Update selection handles and other derived geometry
			this->BuildRepresentation();
		}
	}
	else if (this->InteractionState == vtkTransformRepresentation::OnX ||
			 this->InteractionState == vtkTransformRepresentation::OnY ||
			 this->InteractionState == vtkTransformRepresentation::OnZ)
	{
		// Rotate about selected axis through the origin (only if rotations enabled)
		if (this->RotationEnabled)
		{
			int idx = this->InteractionState;
			double localAxis[3];
			LocalAxis(idx, localAxis);

			// Get axis in world coordinates (rotation axis direction)
			double worldAxis[3];
			// Use transform to convert axis direction (rotation should respect existing orientation)
			this->Transform->TransformNormal(localAxis, worldAxis);
			vtkMath::Normalize(worldAxis);

			// Vectors from origin to last and current world points
			double vLast[3] = { lastWorld[0] - origin[0], lastWorld[1] - origin[1], lastWorld[2] - origin[2] };
			double vCurr[3] = { currWorld[0] - origin[0], currWorld[1] - origin[1], currWorld[2] - origin[2] };

			// Project these onto plane orthogonal to worldAxis
			double projLast[3], projCurr[3];
			double dotL = vtkMath::Dot(vLast, worldAxis);
			double dotC = vtkMath::Dot(vCurr, worldAxis);
			projLast[0] = vLast[0] - dotL * worldAxis[0];
			projLast[1] = vLast[1] - dotL * worldAxis[1];
			projLast[2] = vLast[2] - dotL * worldAxis[2];
			projCurr[0] = vCurr[0] - dotC * worldAxis[0];
			projCurr[1] = vCurr[1] - dotC * worldAxis[1];
			projCurr[2] = vCurr[2] - dotC * worldAxis[2];

			double nL = vtkMath::Norm(projLast);
			double nC = vtkMath::Norm(projCurr);

			if (nL > 1e-6 && nC > 1e-6)
			{
				// Normalize
				projLast[0] /= nL; projLast[1] /= nL; projLast[2] /= nL;
				projCurr[0] /= nC; projCurr[1] /= nC; projCurr[2] /= nC;

				double crossPC[3];
				vtkMath::Cross(projLast, projCurr, crossPC);
				double sinAngle = vtkMath::Norm(crossPC);
				double cosAngle = vtkMath::Dot(projLast, projCurr);
				double angleRad = std::atan2(sinAngle, cosAngle);
				double sign = vtkMath::Dot(worldAxis, crossPC);
				if (sign < 0.0)
				{
					angleRad = -angleRad;
				}
				double angleDeg = vtkMath::DegreesFromRadians(angleRad);


				// Translate to origin
				this->Transform->PostMultiply();
				this->Transform->Translate(-origin[0], -origin[1], -origin[2]);
				// Rotate about axis
				this->Transform->RotateWXYZ(angleDeg, worldAxis);
				// Translate back
				this->Transform->Translate(origin[0], origin[1], origin[2]);

				// After rotation update selection handles
				this->BuildRepresentation();
			}
		}
	}

	// Update stored last event to current display for next invocation
	this->LastEventPosition[0] = e[0];
	this->LastEventPosition[1] = e[1];
	this->LastEventPosition[2] = 0.0;
}


//------------------------------------------------------------------------------
// Minimal/required VTK virtual method implementations to resolve link errors.
// These implementations are intentionally conservative and can be expanded
// to provide richer behavior if needed.

// PrintSelf: print basic state
void vtkTransformRepresentation::PrintSelf(ostream& os, vtkIndent indent)
{
	this->Superclass::PrintSelf(os, indent);
	os << indent << "TranslationEnabled: " << (this->TranslationEnabled ? "On" : "Off") << "\n";
	os << indent << "RotationEnabled: " << (this->RotationEnabled ? "On" : "Off") << "\n";
}

// GetOriginWorldPosition (array variant)
void vtkTransformRepresentation::GetOriginWorldPosition(double pos[3])
{
	if (this->OriginRepresentation)
	{
		// vtkSphereHandleRepresentation exposes GetWorldPosition
		this->OriginRepresentation->GetWorldPosition(pos);
	}
	else
	{
		pos[0] = pos[1] = pos[2] = 0.0;
	}
}


// GetOriginWorldPosition (pointer-returning variant)
double* vtkTransformRepresentation::GetOriginWorldPosition()
{
	static double pos[3] = { 0.0, 0.0, 0.0 };
	this->GetOriginWorldPosition(pos);
	return pos;
}

// StartWidgetInteraction: record starting event position and compute/set interaction state
void vtkTransformRepresentation::StartWidgetInteraction(double e[2])
{
	// Store last event for subsequent WidgetInteraction calls
	this->LastEventPosition[0] = e[0];
	this->LastEventPosition[1] = e[1];
	this->LastEventPosition[2] = 0.0;

	// Compute and set interaction state immediately so widgets that call
	// StartWidgetInteraction (e.g. SelectAction) can query/GetInteractionState()
	// and act accordingly.
	int state = this->ComputeInteractionState(static_cast<int>(std::lround(e[0])),
											 static_cast<int>(std::lround(e[1])),
											 0);
	this->SetInteractionState(state);
}

// BuildRepresentation: update pipeline / handle positions (choose tips from transformed polydata)
void vtkTransformRepresentation::BuildRepresentation()
{
	// Update transform filters so mappers have up-to-date geometry
	if (this->StaticAxesFilter)
	{
		this->StaticAxesFilter->Update();
	}
	if (this->DynamicAxesFilter)
	{
		this->DynamicAxesFilter->Update();
	}

	// Ensure origin representation knows current position (best-effort)
	double origin[3] = { 0.0, 0.0, 0.0 };
	if (this->OriginRepresentation)
	{
		this->GetOriginWorldPosition(origin);
		this->OriginRepresentation->SetWorldPosition(origin);
	}

	// If the one-shot preserve flag is set, restore saved tip positions and clear the flag.
	// This is required so callers that save tip world positions before changing transforms
	// (e.g. to reset origin/transform) can keep the tip handles from snapping.
	if (this->PreserveSelectionHandles)
	{
		if (this->SelectionXRepresentation)
		{
			this->SelectionXRepresentation->SetWorldPosition(this->SavedSelectionTipX);
		}
		if (this->SelectionYRepresentation)
		{
			this->SelectionYRepresentation->SetWorldPosition(this->SavedSelectionTipY);
		}
		if (this->SelectionZRepresentation)
		{
			this->SelectionZRepresentation->SetWorldPosition(this->SavedSelectionTipZ);
		}

		// One-shot; consume the preserve request so subsequent builds behave normally.
		this->PreserveSelectionHandles = 0;
		return;
	}

	// Compute and place selection handles at the tips of the dynamic axes.
	// Use the transformed polydata produced by DynamicAxesFilter and pick the
	// point that has the largest projection along the world-space axis
	// direction (handles rotation + translation correctly).
	if (this->DynamicAxesFilter && this->DynamicAxesFilter->GetOutput() &&
		this->SelectionXRepresentation && this->SelectionYRepresentation && this->SelectionZRepresentation)
	{
		vtkPolyData* out = this->DynamicAxesFilter->GetOutput();
		vtkPoints* pts = out->GetPoints();
		vtkIdType n = pts ? pts->GetNumberOfPoints() : 0;

		auto pickTipAlongAxis = [&](const double localAxis[3], double outPt[3]) -> bool {
			// worldAxis = orientation of localAxis after applying the transform
			double worldAxis[3] = { localAxis[0], localAxis[1], localAxis[2] };
			if (this->Transform)
			{
				this->Transform->TransformNormal(const_cast<double*>(localAxis), worldAxis);
				vtkMath::Normalize(worldAxis);
			}

			if (n > 0)
			{
				double bestDot = VTK_DOUBLE_MIN;
				double candidate[3];
				for (vtkIdType i = 0; i < n; ++i)
				{
					double p[3];
					pts->GetPoint(i, p);
					// vector from origin to point
					double v[3] = { p[0] - origin[0], p[1] - origin[1], p[2] - origin[2] };
					double d = vtkMath::Dot(v, worldAxis);
					if (d > bestDot)
					{
						bestDot = d;
						candidate[0] = p[0];
						candidate[1] = p[1];
						candidate[2] = p[2];
					}
				}
				if (bestDot > VTK_DOUBLE_MIN)
				{
					outPt[0] = candidate[0];
					outPt[1] = candidate[1];
					outPt[2] = candidate[2];
					return true;
				}
			}

			// Fallback: transform a unit/local axis endpoint by the transform
			if (this->Transform)
			{
				this->Transform->TransformPoint(localAxis, outPt);
				return true;
			}

			return false;
			};

		double localX[3] = { 1.0, 0.0, 0.0 };
		double localY[3] = { 0.0, 1.0, 0.0 };
		double localZ[3] = { 0.0, 0.0, 1.0 };
		double tipX[3], tipY[3], tipZ[3];

		bool okX = pickTipAlongAxis(localX, tipX);
		bool okY = pickTipAlongAxis(localY, tipY);
		bool okZ = pickTipAlongAxis(localZ, tipZ);

		if (okX)
		{
			this->SelectionXRepresentation->SetWorldPosition(tipX);
		}
		if (okY)
		{
			this->SelectionYRepresentation->SetWorldPosition(tipY);
		}
		if (okZ)
		{
			this->SelectionZRepresentation->SetWorldPosition(tipZ);
		}
	}
	else
	{
		// As a final fallback, place handles at unit axes transformed by Transform
		if (this->Transform && this->SelectionXRepresentation && this->SelectionYRepresentation && this->SelectionZRepresentation)
		{
			double localX[3] = { 1.0, 0.0, 0.0 };
			double localY[3] = { 0.0, 1.0, 0.0 };
			double localZ[3] = { 0.0, 0.0, 1.0 };
			double worldX[3], worldY[3], worldZ[3];

			this->Transform->TransformPoint(localX, worldX);
			this->Transform->TransformPoint(localY, worldY);
			this->Transform->TransformPoint(localZ, worldZ);

			this->SelectionXRepresentation->SetWorldPosition(worldX);
			this->SelectionYRepresentation->SetWorldPosition(worldY);
			this->SelectionZRepresentation->SetWorldPosition(worldZ);
		}
	}
}

// AlignSelectionHandlesToStaticAxes: place the X/Y/Z selection handles at the ends of the StaticAxes.
// This uses the StaticAxesFilter output when available (picks the furthest point along each axis
// relative to the current origin). Falls back to transforming a unit axis endpoint using StaticTransform.
void vtkTransformRepresentation::AlignSelectionHandlesToStaticAxes()
{
	// Ensure pipeline is up-to-date
	if (this->StaticAxesFilter)
	{
		this->StaticAxesFilter->Update();
	}

	// Get origin for relative picking
	double origin[3] = { 0.0, 0.0, 0.0 };
	this->GetOriginWorldPosition(origin);

	// If we have valid output points from the static axes filter, pick tips by projecting
	// along the axis directions (local axes projected into world via StaticTransform).
	if (this->StaticAxesFilter && this->StaticAxesFilter->GetOutput() &&
		this->SelectionXRepresentation && this->SelectionYRepresentation && this->SelectionZRepresentation)
	{
		vtkPolyData* out = this->StaticAxesFilter->GetOutput();
		vtkPoints* pts = out ? out->GetPoints() : nullptr;
		vtkIdType n = pts ? pts->GetNumberOfPoints() : 0;

		auto pickTipAlongAxis = [&](const double localAxis[3], double outPt[3]) -> bool {
			// compute world axis direction (StaticTransform is translation-only but keep general)
			double worldAxis[3] = { localAxis[0], localAxis[1], localAxis[2] };
			if (this->StaticTransform)
			{
				this->StaticTransform->TransformNormal(const_cast<double*>(localAxis), worldAxis);
				vtkMath::Normalize(worldAxis);
			}

			if (n > 0)
			{
				double bestDot = VTK_DOUBLE_MIN;
				double candidate[3] = { 0.0, 0.0, 0.0 };
				for (vtkIdType i = 0; i < n; ++i)
				{
					double p[3];
					pts->GetPoint(i, p);
					double v[3] = { p[0] - origin[0], p[1] - origin[1], p[2] - origin[2] };
					double d = vtkMath::Dot(v, worldAxis);
					if (d > bestDot)
					{
						bestDot = d;
						candidate[0] = p[0];
						candidate[1] = p[1];
						candidate[2] = p[2];
					}
				}
				if (bestDot > VTK_DOUBLE_MIN)
				{
					outPt[0] = candidate[0];
					outPt[1] = candidate[1];
					outPt[2] = candidate[2];
					return true;
				}
			}

			// Fallback: transform a unit/local axis point by StaticTransform
			if (this->StaticTransform)
			{
				this->StaticTransform->TransformPoint(localAxis, outPt);
				return true;
			}

			return false;
			};

		double localX[3] = { 1.0, 0.0, 0.0 };
		double localY[3] = { 0.0, 1.0, 0.0 };
		double localZ[3] = { 0.0, 0.0, 1.0 };
		double tipX[3], tipY[3], tipZ[3];

		bool okX = pickTipAlongAxis(localX, tipX);
		bool okY = pickTipAlongAxis(localY, tipY);
		bool okZ = pickTipAlongAxis(localZ, tipZ);

		if (okX)
		{
			this->SelectionXRepresentation->SetWorldPosition(tipX);
		}
		if (okY)
		{
			this->SelectionYRepresentation->SetWorldPosition(tipY);
		}
		if (okZ)
		{
			this->SelectionZRepresentation->SetWorldPosition(tipZ);
		}
	}
	else
	{
		// Fallback: use StaticTransform to position unit axis endpoints in world space.
		if (this->StaticTransform && this->SelectionXRepresentation && this->SelectionYRepresentation && this->SelectionZRepresentation)
		{
			double localX[3] = { 1.0, 0.0, 0.0 };
			double localY[3] = { 0.0, 1.0, 0.0 };
			double localZ[3] = { 0.0, 0.0, 1.0 };
			double worldX[3], worldY[3], worldZ[3];

			this->StaticTransform->TransformPoint(localX, worldX);
			this->StaticTransform->TransformPoint(localY, worldY);
			this->StaticTransform->TransformPoint(localZ, worldZ);

			this->SelectionXRepresentation->SetWorldPosition(worldX);
			this->SelectionYRepresentation->SetWorldPosition(worldY);
			this->SelectionZRepresentation->SetWorldPosition(worldZ);
		}
	}
}

// ComputeInteractionState: basic hit-test against origin and axis tips (display-space tolerance)
int vtkTransformRepresentation::ComputeInteractionState(int X, int Y, int modify)
{
	// If no renderer, can't compute hit tests
	if (!this->Renderer)
	{
		(void)modify;
		return vtkTransformRepresentation::Outside;
	}

	// Get the display coordinate of the origin
	double originWorld[3];
	this->GetOriginWorldPosition(originWorld);

	// Project a world point to display
	auto WorldToDisplayPoint = [this](const double world[3], double display[3]) {
		this->Renderer->SetWorldPoint(world[0], world[1], world[2], 1.0);
		this->Renderer->WorldToDisplay();
		this->Renderer->GetDisplayPoint(display);
		};

	double originDisp[3];
	WorldToDisplayPoint(originWorld, originDisp);

	// Check origin proximity (tolerance is in pixels)
	double dx = static_cast<double>(X) - originDisp[0];
	double dy = static_cast<double>(Y) - originDisp[1];
	double dist2 = dx * dx + dy * dy;
	if (dist2 <= static_cast<double>(this->Tolerance) * static_cast<double>(this->Tolerance))
	{
		return vtkTransformRepresentation::OnOrigin;
	}

	// For axis tips, use the selection handle world positions (already updated in BuildRepresentation).
	// If they aren't available, fall back to approximating by transforming unit axes.
	double tipWorld[3];
	double tipDisp[3];
	// X tip
	if (this->SelectionXRepresentation)
	{
		this->SelectionXRepresentation->GetWorldPosition(tipWorld);
		WorldToDisplayPoint(tipWorld, tipDisp);
		dx = static_cast<double>(X) - tipDisp[0];
		dy = static_cast<double>(Y) - tipDisp[1];
		dist2 = dx * dx + dy * dy;
		if (dist2 <= static_cast<double>(this->Tolerance) * static_cast<double>(this->Tolerance))
		{
			return vtkTransformRepresentation::OnX;
		}
	}
	// Y tip
	if (this->SelectionYRepresentation)
	{
		this->SelectionYRepresentation->GetWorldPosition(tipWorld);
		WorldToDisplayPoint(tipWorld, tipDisp);
		dx = static_cast<double>(X) - tipDisp[0];
		dy = static_cast<double>(Y) - tipDisp[1];
		dist2 = dx * dx + dy * dy;
		if (dist2 <= static_cast<double>(this->Tolerance) * static_cast<double>(this->Tolerance))
		{
			return vtkTransformRepresentation::OnY;
		}
	}
	// Z tip
	if (this->SelectionZRepresentation)
	{
		this->SelectionZRepresentation->GetWorldPosition(tipWorld);
		WorldToDisplayPoint(tipWorld, tipDisp);
		dx = static_cast<double>(X) - tipDisp[0];
		dy = static_cast<double>(Y) - tipDisp[1];
		dist2 = dx * dx + dy * dy;
		if (dist2 <= static_cast<double>(this->Tolerance) * static_cast<double>(this->Tolerance))
		{
			return vtkTransformRepresentation::OnZ;
		}
	}

	(void)modify;

	return vtkTransformRepresentation::Outside;
}

// GetBounds: merge actor bounds; return static storage pointer (typical VTK pattern)
double* vtkTransformRepresentation::GetBounds()
{
	static double b[6];
	bool init = false;
	// Initialize to zero
	b[0] = b[1] = b[2] = b[3] = b[4] = b[5] = 0.0;

	if (this->StaticAxesActor)
	{
		double const* s = this->StaticAxesActor->GetBounds();
		if (s)
		{
			if (!init)
			{
				std::memcpy(b, s, sizeof(b));
				init = true;
			}
			else
			{
				b[0] = std::min(b[0], s[0]);
				b[1] = std::max(b[1], s[1]);
				b[2] = std::min(b[2], s[2]);
				b[3] = std::max(b[3], s[3]);
				b[4] = std::min(b[4], s[4]);
				b[5] = std::max(b[5], s[5]);
			}
		}
	}

	if (this->DynamicAxesActor)
	{
		double const* d = this->DynamicAxesActor->GetBounds();
		if (d)
		{
			if (!init)
			{
				std::memcpy(b, d, sizeof(b));
				init = true;
			}
			else
			{
				b[0] = std::min(b[0], d[0]);
				b[1] = std::max(b[1], d[1]);
				b[2] = std::min(b[2], d[2]);
				b[3] = std::max(b[3], d[3]);
				b[4] = std::min(b[4], d[4]);
				b[5] = std::max(b[5], d[5]);
			}
		}
	}

	// Fall back to zero bounds if nothing provided
	if (!init)
	{
		b[0] = b[2] = b[4] = 0.0;
		b[1] = b[3] = b[5] = 0.0;
	}

	return b;
}

// Release graphics resources
void vtkTransformRepresentation::ReleaseGraphicsResources(vtkWindow* w)
{
	if (this->StaticAxesActor)
	{
		this->StaticAxesActor->ReleaseGraphicsResources(w);
	}
	if (this->DynamicAxesActor)
	{
		this->DynamicAxesActor->ReleaseGraphicsResources(w);
	}
	if (this->OriginRepresentation)
	{
		this->OriginRepresentation->ReleaseGraphicsResources(w);
	}
	if (this->SelectionXRepresentation)
	{
		this->SelectionXRepresentation->ReleaseGraphicsResources(w);
	}
	if (this->SelectionYRepresentation)
	{
		this->SelectionYRepresentation->ReleaseGraphicsResources(w);
	}
	if (this->SelectionZRepresentation)
	{
		this->SelectionZRepresentation->ReleaseGraphicsResources(w);
	}
}

// Render opaque geometry
int vtkTransformRepresentation::RenderOpaqueGeometry(vtkViewport* viewport)
{
	int count = 0;
	if (this->StaticAxesActor)
	{
		count += this->StaticAxesActor->RenderOpaqueGeometry(viewport);
	}
	if (this->DynamicAxesActor)
	{
		count += this->DynamicAxesActor->RenderOpaqueGeometry(viewport);
	}
	if (this->OriginRepresentation)
	{
		count += this->OriginRepresentation->RenderOpaqueGeometry(viewport);
	}
	if (this->SelectionXRepresentation)
	{
		count += this->SelectionXRepresentation->RenderOpaqueGeometry(viewport);
	}
	if (this->SelectionYRepresentation)
	{
		count += this->SelectionYRepresentation->RenderOpaqueGeometry(viewport);
	}
	if (this->SelectionZRepresentation)
	{
		count += this->SelectionZRepresentation->RenderOpaqueGeometry(viewport);
	}
	return count;
}

// Render translucent polygonal geometry
int vtkTransformRepresentation::RenderTranslucentPolygonalGeometry(vtkViewport* viewport)
{
	int count = 0;
	if (this->StaticAxesActor)
	{
		count += this->StaticAxesActor->RenderTranslucentPolygonalGeometry(viewport);
	}
	if (this->DynamicAxesActor)
	{
		count += this->DynamicAxesActor->RenderTranslucentPolygonalGeometry(viewport);
	}
	if (this->OriginRepresentation)
	{
		count += this->OriginRepresentation->RenderTranslucentPolygonalGeometry(viewport);
	}
	if (this->SelectionXRepresentation)
	{
		count += this->SelectionXRepresentation->RenderTranslucentPolygonalGeometry(viewport);
	}
	if (this->SelectionYRepresentation)
	{
		count += this->SelectionYRepresentation->RenderTranslucentPolygonalGeometry(viewport);
	}
	if (this->SelectionZRepresentation)
	{
		count += this->SelectionZRepresentation->RenderTranslucentPolygonalGeometry(viewport);
	}
	return count;
}

// GetYawPitchRollDegrees: simple extractor assuming no significant scale/shear.
// Z-Y-X (yaw-pitch-roll) convention, degrees, handles gimbal-lock.
void vtkTransformRepresentation::GetYawPitchRollDegrees(double& yawDeg, double& pitchDeg, double& rollDeg)
{
	yawDeg = pitchDeg = rollDeg = 0.0;
	if (!this->Transform)
	{
		return;
	}

	vtkMatrix4x4* M = this->Transform->GetMatrix();
	if (!M)
	{
		return;
	}

	// rotation matrix elements (row,col)
	double r00 = M->GetElement(0, 0);
	double r01 = M->GetElement(0, 1);
	double r02 = M->GetElement(0, 2);
	double r10 = M->GetElement(1, 0);
	double r11 = M->GetElement(1, 1);
	double r12 = M->GetElement(1, 2);
	double r20 = M->GetElement(2, 0);
	double r21 = M->GetElement(2, 1);
	double r22 = M->GetElement(2, 2);

	auto clamp = [](double v) {
		if (v > 1.0) return 1.0;
		if (v < -1.0) return -1.0;
		return v;
		};

	// pitch = asin(-r20)
	double pitchRad = std::asin(clamp(-r20));
	double cosPitch = std::cos(pitchRad);

	const double EPS = 1e-6;
	double yawRad = 0.0;
	double rollRad = 0.0;

	if (std::fabs(cosPitch) > EPS)
	{
		// no gimbal lock
		yawRad = std::atan2(r10, r00);
		rollRad = std::atan2(r21, r22);
	}
	else
	{
		// Gimbal lock: choose roll = 0 and derive yaw
		yawRad = std::atan2(-r01, r11);
		rollRad = 0.0;
	}

	yawDeg = vtkMath::DegreesFromRadians(yawRad);
	pitchDeg = vtkMath::DegreesFromRadians(pitchRad);
	rollDeg = vtkMath::DegreesFromRadians(rollRad);
}

// Helper: apply yaw/pitch/roll (degrees) to this->Transform while preserving translation.
void vtkTransformRepresentation::SetYawPitchRollDegrees(double yawDeg, double pitchDeg, double rollDeg)
{
	if (!this->Transform)
	{
		return;
	}

	// preserve current translation
	double tx = 0.0, ty = 0.0, tz = 0.0;
	vtkMatrix4x4* M = this->Transform->GetMatrix();
	if (M)
	{
		tx = M->GetElement(0, 3);
		ty = M->GetElement(1, 3);
		tz = M->GetElement(2, 3);
	}

	// Build new transform: rotation (Z then Y then X) then translation
	this->Transform->Identity();
	this->Transform->PostMultiply(); // ensure operations apply in requested order
	this->Transform->RotateZ(yawDeg);
	this->Transform->RotateY(pitchDeg);
	this->Transform->RotateX(rollDeg);
	this->Transform->Translate(tx, ty, tz);
}

// Individual setters update only the requested angle while preserving the other two.
void vtkTransformRepresentation::SetYawDegrees(double yawDeg)
{
	double yaw = 0.0, pitch = 0.0, roll = 0.0;
	this->GetYawPitchRollDegrees(yaw, pitch, roll);
	this->SetYawPitchRollDegrees(yawDeg, pitch, roll);
}

void vtkTransformRepresentation::SetPitchDegrees(double pitchDeg)
{
	double yaw = 0.0, pitch = 0.0, roll = 0.0;
	this->GetYawPitchRollDegrees(yaw, pitch, roll);
	this->SetYawPitchRollDegrees(yaw, pitchDeg, roll);
}

void vtkTransformRepresentation::SetRollDegrees(double rollDeg)
{
	double yaw = 0.0, pitch = 0.0, roll = 0.0;
	this->GetYawPitchRollDegrees(yaw, pitch, roll);
	this->SetYawPitchRollDegrees(yaw, pitch, rollDeg);
}

void vtkTransformRepresentation::GetOriginAndYPR(double out[6])
{
	this->GetOriginWorldPosition(out);
	// yaw/pitch/roll
	double yaw = 0.0, pitch = 0.0, roll = 0.0;
	this->GetYawPitchRollDegrees(yaw, pitch, roll);
	out[3] = yaw;
	out[4] = pitch;
	out[5] = roll;
}
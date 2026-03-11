#include "vtkLandmarkActor.h"

#include <vtkActor2D.h>
#include <vtkGlyph2D.h>
#include <vtkGlyphSource2D.h>
#include <vtkObjectFactory.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper2D.h>
#include <vtkPropCollection.h>
#include <vtkProperty2D.h>
#include <vtkTransformCoordinateSystems.h>
#include <vtkViewport.h>
#include <vtkWindow.h>

vtkStandardNewMacro(vtkLandmarkActor);

//----------------------------------------------------------------------------
vtkLandmarkActor::vtkLandmarkActor()
	: m_highlight(false)
{
	BuildPipeline();
}

//----------------------------------------------------------------------------
vtkLandmarkActor::~vtkLandmarkActor()
{
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::BuildPipeline()
{
	// Create focal point (single point in world coordinates)
	m_focalPoint->SetNumberOfPoints(1);
	m_focalPoint->SetPoint(0, 0.0, 0.0, 0.0);

	m_focalData = vtkSmartPointer<vtkPolyData>::New();
	m_focalData->SetPoints(m_focalPoint);

	// Create default glyph (thick cross at 45 degrees, matching seed widget)
	CreateDefaultGlyph();

	// Transform world -> display coordinates
	m_transform->SetInputCoordinateSystemToWorld();
	m_transform->SetOutputCoordinateSystemToDisplay();
	m_transform->SetInputData(m_focalData);

	// Glyph the transformed point
	m_glypher = vtkSmartPointer<vtkGlyph2D>::New();
	m_glypher->SetInputConnection(m_transform->GetOutputPort());
	m_glypher->SetSourceData(m_glyphShape);
	m_glypher->SetVectorModeToVectorRotationOff();
	m_glypher->ScalingOn();
	m_glypher->SetScaleModeToDataScalingOff();
	m_glypher->SetScaleFactor(1.0);

	// Map to 2D
	m_mapper = vtkSmartPointer<vtkPolyDataMapper2D>::New();
	m_mapper->SetInputConnection(m_glypher->GetOutputPort());

	// Create property with default appearance
	m_property = vtkSmartPointer<vtkProperty2D>::New();
	m_property->SetColor(1.0, 0.0, 0.0); // Red by default
	m_property->SetLineWidth(1.5);
	m_property->SetOpacity(0.8);

	// Create actor
	m_actor = vtkSmartPointer<vtkActor2D>::New();
	m_actor->SetMapper(m_mapper);
	m_actor->SetProperty(m_property);
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::CreateDefaultGlyph()
{
	vtkNew<vtkGlyphSource2D> glyph;
	glyph->SetGlyphTypeToThickCross();
	glyph->SetRotationAngle(45.0);
	glyph->SetScale(20.0); // Match seed widget size
	glyph->FilledOff();
	glyph->Update();

	m_glyphShape = vtkSmartPointer<vtkPolyData>::New();
	m_glyphShape->DeepCopy(glyph->GetOutput());
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::SetWorldPosition(double x, double y, double z)
{
	double oldPos[3];
	m_focalPoint->GetPoint(0, oldPos);

	// Only update if position actually changed
	if (oldPos[0] != x || oldPos[1] != y || oldPos[2] != z) {
		m_focalPoint->SetPoint(0, x, y, z);
		m_focalPoint->Modified();
		m_focalData->Modified(); // Ensure polydata knows about the change
		this->Modified();
	}
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::SetWorldPosition(double pos[3])
{
	SetWorldPosition(pos[0], pos[1], pos[2]);
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::GetWorldPosition(double pos[3])
{
	m_focalPoint->GetPoint(0, pos);
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::SetGlyphShape(vtkPolyData* shape)
{
	if (!shape || shape == m_glyphShape) return;

	m_glyphShape = vtkSmartPointer<vtkPolyData>::New();
	m_glyphShape->DeepCopy(shape);
	m_glypher->SetSourceData(m_glyphShape);
	this->Modified();
}

//----------------------------------------------------------------------------
vtkPolyData* vtkLandmarkActor::GetGlyphShape()
{
	return m_glyphShape;
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::SetColor(double r, double g, double b)
{
	if (m_property) {
		m_property->SetColor(r, g, b);
		this->Modified();
	}
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::GetColor(double& r, double& g, double& b)
{
	if (m_property) {
		double* c = m_property->GetColor();
		r = c[0];
		g = c[1];
		b = c[2];
	}
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::SetLineWidth(float width)
{
	if (m_property) {
		m_property->SetLineWidth(width);
		this->Modified();
	}
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::SetHighlight(bool on)
{
	if (m_highlight == on) return;

	m_highlight = on;

	if (m_property) {
		if (m_highlight) {
			// Highlighted: brighter, thicker
			m_property->SetLineWidth(2.5);
			m_property->SetOpacity(1.0);
		}
		else {
			// Normal: standard appearance
			m_property->SetLineWidth(1.5);
			m_property->SetOpacity(0.8);
		}
		this->Modified();
	}
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::SetViewport(vtkViewport* viewport)
{
	m_transform->SetViewport(viewport);
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::GetActors2D(vtkPropCollection* pc)
{
	if (pc && this->GetVisibility() && m_actor) {
		pc->AddItem(m_actor);
	}
	this->Superclass::GetActors2D(pc);
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::ReleaseGraphicsResources(vtkWindow* win)
{
	if (m_actor) {
		m_actor->ReleaseGraphicsResources(win);
	}
}

//----------------------------------------------------------------------------
int vtkLandmarkActor::RenderOverlay(vtkViewport* viewport)
{
	if (!this->GetVisibility()) return 0;

	// Update transform (world->display)
	m_transform->Update();

	// Render the internal actor
	int rendered = 0;
	if (m_actor) {
		rendered = m_actor->RenderOverlay(viewport);
	}

	return rendered;
}

//----------------------------------------------------------------------------
void vtkLandmarkActor::PrintSelf(ostream& os, vtkIndent indent)
{
	this->Superclass::PrintSelf(os, indent);

	os << indent << "Highlight: " << (m_highlight ? "On" : "Off") << "\n";

	if (m_property) {
		os << indent << "Property:\n";
		m_property->PrintSelf(os, indent.GetNextIndent());
	}

	if (m_glyphShape) {
		os << indent << "Glyph Shape: " << m_glyphShape << "\n";
	}
}
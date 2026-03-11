#ifndef vtkLandmarkActor_h
#define vtkLandmarkActor_h

#include <vtkActor2D.h>
#include <vtkSmartPointer.h>
#include <vtkNew.h>

class vtkPoints;
class vtkPolyData;
class vtkGlyph2D;
class vtkPolyDataMapper2D;
class vtkProperty2D;
class vtkTransformCoordinateSystems;
class vtkViewport;

/// Lightweight 2D actor for displaying landmark shadows in SliceView instances.
/// Designed to work alongside vtkSeedWidget in LandmarkCoordinator.
/// Uses world coordinates and automatically handles world->display transformation.
class vtkLandmarkActor : public vtkActor2D
{
public:
	static vtkLandmarkActor* New();
	vtkTypeMacro(vtkLandmarkActor, vtkActor2D);
	void PrintSelf(ostream& os, vtkIndent indent) override;

	/// Set the landmark's world position (3D coordinates)
	void SetWorldPosition(double x, double y, double z);
	void SetWorldPosition(double pos[3]);
	void GetWorldPosition(double pos[3]);

	/// Set the glyph shape (typically a thick cross matching seed widget)
	void SetGlyphShape(vtkPolyData* shape);
	vtkPolyData* GetGlyphShape();

	/// Configure appearance (color, line width, etc.)
	void SetColor(double r, double g, double b);
	void GetColor(double& r, double& g, double& b);
	void SetLineWidth(float width);

	/// Highlight state (e.g., when hovered or selected)
	void SetHighlight(bool on);
	bool GetHighlight() const { return m_highlight; }

	/// Required vtkProp overrides
	void GetActors2D(vtkPropCollection* pc) override;
	void ReleaseGraphicsResources(vtkWindow* win) override;
	int RenderOverlay(vtkViewport* viewport) override;

	/// Must be called before rendering with the SliceView's renderer
	void SetViewport(vtkViewport* viewport);

protected:
	vtkLandmarkActor();
	~vtkLandmarkActor() override;

private:
	vtkLandmarkActor(const vtkLandmarkActor&) = delete;
	void operator=(const vtkLandmarkActor&) = delete;

	// Pipeline components
	vtkNew<vtkPoints> m_focalPoint;
	vtkSmartPointer<vtkPolyData> m_focalData;
	vtkSmartPointer<vtkPolyData> m_glyphShape;
	vtkNew<vtkTransformCoordinateSystems> m_transform;
	vtkSmartPointer<vtkGlyph2D> m_glypher;
	vtkSmartPointer<vtkPolyDataMapper2D> m_mapper;
	vtkSmartPointer<vtkActor2D> m_actor;
	vtkSmartPointer<vtkProperty2D> m_property;

	// State
	bool m_highlight;

	void BuildPipeline();
	void CreateDefaultGlyph();
};

#endif // vtkLandmarkActor_h
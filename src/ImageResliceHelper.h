#pragma once

#include <vtkSmartPointer.h>
#include <vtkTransform.h>

class vtkImageReslice;
class vtkTransform;
class vtkImageData;
class vtkAlgorithmOutput;
class vtkImageChangeInformation; // forward declare

//
// Lightweight, reusable helper that encapsulates a vtkImageReslice + vtkTransform.
// Not tied to any view/widget; views (SliceView / VolumeView) remain agnostic and
// can connect their mappers to reslice->GetOutputPort() when they want a resliced
// representation.  Identity transform + matched output grid => native appearance.
//
class ImageResliceHelper
{
public:
	ImageResliceHelper();
	~ImageResliceHelper();

	// Set input from a raw vtkImageData (convenience)
	void SetInputData(vtkImageData* img);

	// Set input from an algorithm output (preferred for pipeline connections)
	void SetInputConnection(vtkAlgorithmOutput* port);

	// Access the reslice transform to modify (translate/rotate about seed, etc.)
	// Caller may call transform->Identity()/Translate()/Rotate*() and then call Update().
	// Note: this returns the "user" transform. The helper composes this with
	// translations about the image center when applying to the internal reslice filter.
	vtkTransform* GetTransform();

	// Return physical center of current input image (in world coordinates).
	// Returns true on success, false if input not available.
	bool GetInputCenter(double center[3]);

	// Set integer downsample factor (1 == no downsample, 4 == 4x coarser)
	// Update() will recompute output spacing/extent accordingly.
	void SetDownsampleFactor(int factor);

	// Optional: explicitly set output spacing/extent/origin. If not set, Update()
	// will attempt to derive sensible defaults from the input and downsample factor.
	void SetOutputSpacing(const double sp[3]);
	void SetOutputOrigin(const double org[3]);
	void SetOutputExtent(const int extent[6]);

	// Reset any explicit output grid so Update() recomputes from input.
	void ResetOutputGridToInput();

	// Trigger pipeline update / recompute internal parameters.
	// Call after modifying transform / downsample factor.
	void Update();

	// Return the reslice filter's output port so callers can connect mappers:
	//    mapper->SetInputConnection(helper.GetOutputPort());
	vtkAlgorithmOutput* GetOutputPort();

	// Convenience: get the internal reslice (rarely needed).
	vtkImageReslice* GetReslice();
private:
	// recompute output spacing/extent based on input + downsample
	void computeOutputGridFromInput();

	vtkSmartPointer<vtkImageReslice> m_reslice;
	// Internal composed transform applied to the reslice filter (translate(center) * userTransform * translate(-center))
	vtkSmartPointer<vtkTransform>    m_resliceTransform;
	// User-editable transform returned by GetTransform(); callers modify this transform.
	vtkSmartPointer<vtkTransform>    m_userTransform;
	vtkSmartPointer<vtkImageChangeInformation> m_changeInfo; // inserted before reslice to adjust origin

	// explicit output grid overrides (optional)
	bool m_hasExplicitOutputGrid = false;
	double m_outSpacing[3];
	double m_outOrigin[3];
	int    m_outExtent[6];

	// downsample factor (integer >= 1)
	int m_downsample = 1;
};
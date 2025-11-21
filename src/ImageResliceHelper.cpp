#include "ImageResliceHelper.h"

#include <vtkImageReslice.h>
#include <vtkTransform.h>
#include <vtkSmartPointer.h>
#include <vtkAlgorithmOutput.h>
#include <vtkImageData.h>
#include <vtkMatrix4x4.h>
#include <vtkImageChangeInformation.h>

#include <algorithm>
#include <cmath>

ImageResliceHelper::ImageResliceHelper()
{
	m_reslice = vtkSmartPointer<vtkImageReslice>::New();
	m_resliceTransform = vtkSmartPointer<vtkTransform>::New();
	m_userTransform = vtkSmartPointer<vtkTransform>::New();
	m_changeInfo = vtkSmartPointer<vtkImageChangeInformation>::New();
	// wire: changeInfo -> reslice
	m_reslice->SetInputConnection(m_changeInfo->GetOutputPort());

	// reslice transform will be our composed transform; callers edit m_userTransform
	m_reslice->SetResliceTransform(m_resliceTransform);
	m_reslice->SetInterpolationModeToCubic();
	m_reslice->AutoCropOutputOn();
}

ImageResliceHelper::~ImageResliceHelper()
{
}

void ImageResliceHelper::SetInputData(vtkImageData* img)
{
	if (!img) return;
	// Store input via changeInfo so downstream reslice sees the same data
	m_changeInfo->SetInputData(img);
	// Do not override output origin here; computeOutputGridFromInput will set the reslice output origin appropriately
}

void ImageResliceHelper::SetInputConnection(vtkAlgorithmOutput* port)
{
	if (!port) return;
	m_changeInfo->SetInputConnection(port);
	// When using a pipeline input, we cannot compute center immediately here; the caller should call Update() to compute
}

vtkTransform* ImageResliceHelper::GetTransform()
{
	return m_userTransform.Get();
}

vtkImageReslice* ImageResliceHelper::GetReslice()
{
	return m_reslice.Get();
}

bool ImageResliceHelper::GetInputCenter(double center[3])
{
	vtkImageData* in = vtkImageData::SafeDownCast(m_changeInfo->GetOutput());
	if (!in) return false;
	double origin[3]; in->GetOrigin(origin);
	double spacing[3]; in->GetSpacing(spacing);
	int extent[6]; in->GetExtent(extent);
	double centerIndex[3];
	centerIndex[0] = 0.5 * (extent[0] + extent[1]);
	centerIndex[1] = 0.5 * (extent[2] + extent[3]);
	centerIndex[2] = 0.5 * (extent[4] + extent[5]);
	center[0] = origin[0] + centerIndex[0] * spacing[0];
	center[1] = origin[1] + centerIndex[1] * spacing[1];
	center[2] = origin[2] + centerIndex[2] * spacing[2];
	return true;
}

void ImageResliceHelper::SetDownsampleFactor(int factor)
{
	if (factor < 1) factor = 1;
	m_downsample = factor;
}

void ImageResliceHelper::SetOutputSpacing(const double sp[3])
{
	m_hasExplicitOutputGrid = true;
	m_outSpacing[0] = sp[0]; m_outSpacing[1] = sp[1]; m_outSpacing[2] = sp[2];
}

void ImageResliceHelper::SetOutputOrigin(const double org[3])
{
	m_hasExplicitOutputGrid = true;
	m_outOrigin[0] = org[0]; m_outOrigin[1] = org[1]; m_outOrigin[2] = org[2];
}

void ImageResliceHelper::SetOutputExtent(const int extent[6])
{
	m_hasExplicitOutputGrid = true;
	for (int i = 0; i < 6; ++i) m_outExtent[i] = extent[i];
}

void ImageResliceHelper::ResetOutputGridToInput()
{
	m_hasExplicitOutputGrid = false;
}

void ImageResliceHelper::computeOutputGridFromInput()
{
	// If explicit grid provided, honor it
	if (m_hasExplicitOutputGrid) {
		m_reslice->SetOutputSpacing(m_outSpacing);
		m_reslice->SetOutputOrigin(m_outOrigin);
		m_reslice->SetOutputExtent(m_outExtent);
		return;
	}

	// use changeInfo output as the input to reslice
	vtkImageData* in = vtkImageData::SafeDownCast(m_changeInfo->GetOutput());
	if (!in) return;

	double inSpacing[3]; in->GetSpacing(inSpacing);
	double inOrigin[3]; in->GetOrigin(inOrigin);
	int inExt[6]; in->GetExtent(inExt);

	// Compute reduced spacing based on integer downsample factor
	double outSpacing[3] = { inSpacing[0] * m_downsample, inSpacing[1] * m_downsample, inSpacing[2] * m_downsample };

	int outExt[6];
	double outOrigin[3];

	for (int i = 0; i < 3; ++i) {
		int inCount = inExt[2 * i + 1] - inExt[2 * i] + 1;
		if (inCount < 1) inCount = 1;
		// compute output count rounding up so we cover full physical extent
		int outCount = (inCount + m_downsample - 1) / m_downsample;
		if (outCount < 1) outCount = 1;

		// compute input min and max in physical coordinates
		double inMinPhys = inOrigin[i] + inExt[2 * i] * inSpacing[i];
		double inMaxPhys = inOrigin[i] + inExt[2 * i + 1] * inSpacing[i];
		double inCenterPhys = 0.5 * (inMinPhys + inMaxPhys);

		// compute origin so that the output grid is centered at inCenterPhys
		double halfExtentPhys = 0.5 * (outCount - 1) * outSpacing[i];
		outOrigin[i] = inCenterPhys - halfExtentPhys;

		outExt[2 * i] = 0;
		outExt[2 * i + 1] = outCount - 1;
	}

	m_reslice->SetOutputSpacing(outSpacing);
	m_reslice->SetOutputOrigin(outOrigin);
	m_reslice->SetOutputExtent(outExt);
}

void ImageResliceHelper::Update()
{
	// Ensure changeInfo is updated so its output is available to reslice
	m_changeInfo->Update();

	// Recompute output grid before updating reslice. Important: preserve any transform
	computeOutputGridFromInput();

	// Compose reslice transform: translate to center, apply user transform, translate back
	double center[3];
	if (GetInputCenter(center)) {
		m_resliceTransform->Identity();
		m_resliceTransform->Translate(center[0], center[1], center[2]);
		m_resliceTransform->Concatenate(m_userTransform);
		m_resliceTransform->Translate(-center[0], -center[1], -center[2]);
		m_reslice->SetResliceTransform(m_resliceTransform);
	}

	m_reslice->Update();
}

vtkAlgorithmOutput* ImageResliceHelper::GetOutputPort()
{
	return m_reslice->GetOutputPort();
}
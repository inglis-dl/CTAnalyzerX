#include "BoneMorphometry.h"

#include <itkImage.h>
#include <itkImageFileReader.h>
#include <itkBinaryThresholdImageFilter.h>
#include <itkConnectedComponentImageFilter.h>
#include <itkSignedMaurerDistanceMapImageFilter.h>
#include <itkBinaryThinningImageFilter3D.h>
#include <itkMedialThicknessImageFilter3D.h>
#include <itkLabelGeometryImageFilter.h>
#include <itkBinaryBallStructuringElement.h>
#include <itkBinaryDilateImageFilter.h>
#include <itkImageRegionIterator.h>
#include <itkLabelImageToShapeLabelMapFilter.h>
#include <itkImageToVTKImageFilter.h>

#include <vtkCellArray.h>
#include <vtkDiscreteFlyingEdges3D.h>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkImageImport.h>
#include <vtkMarchingCubes.h>
#include <vtkCleanPolyData.h>
#include <vtkTriangleFilter.h>
#include <vtkMassProperties.h>
#include <vtkCenterOfMass.h>
#include <vtkImplicitPolyDataDistance.h>
#include <itkVTKImageToImageFilter.h>

#include <cmath>
#include <iostream>
#include <set>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using ImageType = itk::Image<unsigned char, 3>;
using FloatImageType = itk::Image<float, 3>;

namespace BoneMorphometry {

	ImageType::Pointer VTKToITKImage(vtkImageData* image)
	{
		if (!image)
			return nullptr;

		using VtkToItk = itk::VTKImageToImageFilter<ImageType>;
		auto vtkToItk = VtkToItk::New();
		vtkToItk->SetInput(image);
		vtkToItk->Update();

		auto out = ImageType::New();
		out->Graft(vtkToItk->GetOutput());
		return out;
	}

	// ------------------------------------------------------------
	// ITK → VTK conversion
	// ------------------------------------------------------------
	vtkSmartPointer<vtkImageData> ITKToVTKImage(const std::string& filename)
	{
		auto reader = itk::ImageFileReader<ImageType>::New();
		reader->SetFileName(filename);

		using ItkToVtk = itk::ImageToVTKImageFilter<ImageType>;
		auto itkToVtk = ItkToVtk::New();
		itkToVtk->SetInput(reader->GetOutput());
		itkToVtk->Update();

		auto out = vtkSmartPointer<vtkImageData>::New();
		out->DeepCopy(itkToVtk->GetOutput());

		return out;
	}

	// ------------------------------------------------------------
	// Extract surface mesh
	// ------------------------------------------------------------
	vtkPolyData* ExtractSurface(vtkImageData* img)
	{
		auto dfe = vtkSmartPointer<vtkDiscreteFlyingEdges3D>::New();
		dfe->SetInputData(img);

		// Extract label 255 (bone)
		dfe->SetValue(0, 255);

		// Optional: ensure scalars are treated as discrete
		dfe->ComputeNormalsOn();   // Usually desirable
		dfe->ComputeGradientsOff();
		dfe->Update();

		auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
		clean->SetInputConnection(dfe->GetOutputPort());
		clean->Update();

		auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
		tri->SetInputConnection(clean->GetOutputPort());
		tri->Update();

		tri->GetOutput()->Register(nullptr);
		return tri->GetOutput();
	}

	// ------------------------------------------------------------
	// BV/TV
	// ------------------------------------------------------------
	double ComputeBVTV(const std::string& filename)
	{
		auto reader = itk::ImageFileReader<ImageType>::New();
		reader->SetFileName(filename);
		reader->Update();

		auto img = reader->GetOutput();
		auto spacing = img->GetSpacing();
		double voxelVol = spacing[0] * spacing[1] * spacing[2];

		itk::ImageRegionConstIterator<ImageType> it(img, img->GetLargestPossibleRegion());

		double boneVol = 0, totalVol = 0;
		for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
			totalVol += voxelVol;
			if (it.Get() > 0) boneVol += voxelVol;
		}
		return boneVol / totalVol;
	}

	// ------------------------------------------------------------
	// SMI
	// ------------------------------------------------------------
	double ComputeSMI(const std::string& filename)
	{
		vtkSmartPointer<vtkImageData> img = ITKToVTKImage(filename);
		vtkSmartPointer<vtkPolyData> surf = ExtractSurface(img);

		auto mass = vtkSmartPointer<vtkMassProperties>::New();
		mass->SetInputData(surf);
		mass->Update();

		double V = mass->GetVolume();
		double S = mass->GetSurfaceArea();

		// Dilate mask by 1 voxel
		auto reader = itk::ImageFileReader<ImageType>::New();
		reader->SetFileName(filename);
		reader->Update();

		using Struct = itk::BinaryBallStructuringElement<unsigned char, 3>;
		Struct se; se.SetRadius(1); se.CreateStructuringElement();

		auto dil = itk::BinaryDilateImageFilter<ImageType, ImageType, Struct>::New();
		dil->SetInput(reader->GetOutput());
		dil->SetKernel(se);
		dil->Update();

		// Convert dilated mask to VTK
		auto importer = vtkSmartPointer<vtkImageImport>::New();
		auto dimg = dil->GetOutput();
		auto region = dimg->GetLargestPossibleRegion();
		auto size = region.GetSize();
		auto spacing = dimg->GetSpacing();
		auto origin = dimg->GetOrigin();

		importer->SetDataSpacing(spacing[0], spacing[1], spacing[2]);
		importer->SetDataOrigin(origin[0], origin[1], origin[2]);
		importer->SetWholeExtent(0, size[0] - 1, 0, size[1] - 1, 0, size[2] - 1);
		importer->SetDataExtentToWholeExtent();
		importer->SetDataScalarTypeToUnsignedChar();
		importer->SetNumberOfScalarComponents(1);
		importer->SetImportVoidPointer(dimg->GetBufferPointer());
		importer->Update();

		auto surfD = ExtractSurface(importer->GetOutput());
		auto massD = vtkSmartPointer<vtkMassProperties>::New();
		massD->SetInputData(surfD);
		massD->Update();

		double Vd = massD->GetVolume();
		double Sd = massD->GetSurfaceArea();

		return 6.0 * ((Vd - V) * S) / (V * (Sd - S));
	}

	// ------------------------------------------------------------
	// Voxel MAT (MedialThicknessImageFilter3D)
	// ------------------------------------------------------------
	MATMetrics ComputeVoxelMAT(const std::string& filename)
	{
		auto reader = itk::ImageFileReader<ImageType>::New();
		reader->SetFileName(filename);
		reader->Update();

		using MT = itk::MedialThicknessImageFilter3D<ImageType, FloatImageType>;
		auto mt = MT::New();
		mt->SetInput(reader->GetOutput());
		mt->Update();

		MATMetrics M;
		auto output = mt->GetOutput();
		itk::ImageRegionConstIterator<FloatImageType> mit(output, output->GetLargestPossibleRegion());
		double sum = 0.0;
		size_t count = 0;
		double maxRadius = 0.0;
		for (mit.GoToBegin(); !mit.IsAtEnd(); ++mit) {
			float v = mit.Get();
			if (v > 0) {
				sum += v;
				++count;
				if (v > maxRadius) maxRadius = v;
			}
		}
		M.meanRadius = (count > 0) ? (sum / count) / 2.0 : 0.0;
		M.maxRadius = maxRadius / 2.0;
		return M;
	}

	// ------------------------------------------------------------
	// Surface-based cortical thickness (VTK implicit distance)
	// ------------------------------------------------------------
	double ComputeSurfaceThickness(vtkPolyData* surface)
	{
		auto imp = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
		imp->SetInput(surface);

		double sum = 0;
		int count = surface->GetNumberOfPoints();

		for (int i = 0; i < count; i++) {
			double p[3];
			surface->GetPoint(i, p);
			double d = std::abs(imp->EvaluateFunction(p));
			sum += d;
		}
		return sum / count;
	}

	// ------------------------------------------------------------
	// Euler characteristic from triangulated surface
	// ------------------------------------------------------------
	int ComputeEulerCharacteristicFromSurface(vtkPolyData* surface)
	{
		if (!surface)
			return 0;

		vtkIdType V = surface->GetNumberOfPoints();
		vtkIdType F = surface->GetNumberOfPolys();

		std::set<std::pair<vtkIdType, vtkIdType>> uniqueEdges;

		vtkCellArray* polys = surface->GetPolys();
		vtkIdType npts = 0;
		const vtkIdType* pts = nullptr;

		polys->InitTraversal();
		while (polys->GetNextCell(npts, pts))
		{
			if (npts < 3)
				continue;

			for (vtkIdType i = 0; i < npts; ++i)
			{
				vtkIdType a = pts[i];
				vtkIdType b = pts[(i + 1) % npts];
				if (a > b)
					std::swap(a, b);

				uniqueEdges.emplace(a, b);
			}
		}

		vtkIdType E = static_cast<vtkIdType>(uniqueEdges.size());
		return static_cast<int>(V - E + F);
	}

	// ------------------------------------------------------------
	// Euler characteristic via LabelGeometryImageFilter
	// ------------------------------------------------------------
	int ComputeEulerCharacteristicITK(const std::string& filename,
		unsigned int labelValue)
	{
		auto reader = itk::ImageFileReader<ImageType>::New();
		reader->SetFileName(filename);
		reader->Update();

		// Build a binary mask for the requested label.
		auto bin = itk::BinaryThresholdImageFilter<ImageType, ImageType>::New();
		bin->SetInput(reader->GetOutput());
		bin->SetLowerThreshold(static_cast<unsigned char>(labelValue));
		bin->SetUpperThreshold(static_cast<unsigned char>(labelValue));
		bin->SetInsideValue(1);
		bin->SetOutsideValue(0);
		bin->Update();

		// Convert binary mask to VTK.
		auto mimg = bin->GetOutput();
		auto region = mimg->GetLargestPossibleRegion();
		auto size = region.GetSize();
		auto spacing = mimg->GetSpacing();
		auto origin = mimg->GetOrigin();

		auto importer = vtkSmartPointer<vtkImageImport>::New();
		importer->SetDataSpacing(spacing[0], spacing[1], spacing[2]);
		importer->SetDataOrigin(origin[0], origin[1], origin[2]);
		importer->SetWholeExtent(0, size[0] - 1, 0, size[1] - 1, 0, size[2] - 1);
		importer->SetDataExtentToWholeExtent();
		importer->SetDataScalarTypeToUnsignedChar();
		importer->SetNumberOfScalarComponents(1);
		importer->SetImportVoidPointer(mimg->GetBufferPointer());
		importer->Update();

		auto surf = ExtractSurface(importer->GetOutput());
		return ComputeEulerCharacteristicFromSurface(surf);
	}

	// Thickness via MedialThicknessImageFilter3D
	double ComputeMedialThicknessITK(const std::string& filename,
		unsigned int labelValue)
	{
		auto reader = itk::ImageFileReader<ImageType>::New();
		reader->SetFileName(filename);
		reader->Update();

		using MT = itk::MedialThicknessImageFilter3D<ImageType, FloatImageType>;
		auto mt = MT::New();
		mt->SetInput(reader->GetOutput());
		mt->Update();

		// Compute the maximum thickness manually, since GetMaxThickness does not exist
		auto output = mt->GetOutput();
		itk::ImageRegionConstIterator<FloatImageType> it(output, output->GetLargestPossibleRegion());
		double maxThickness = 0.0;
		for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
			float v = it.Get();
			if (v > maxThickness) maxThickness = v;
		}
		// Thickness is 2x the radius
		return maxThickness;
	}

	// ------------------------------------------------------------
	// Void extraction + morphometry
	// ------------------------------------------------------------
	std::vector<VoidMetrics> ExtractAndMeasureVoids(const std::string& filename)
	{
		std::vector<VoidMetrics> out;

		auto reader = itk::ImageFileReader<ImageType>::New();
		reader->SetFileName(filename);
		reader->Update();

		auto img = reader->GetOutput();
		auto region = img->GetLargestPossibleRegion();
		auto size = region.GetSize();

		// Invert mask
		auto inv = itk::BinaryThresholdImageFilter<ImageType, ImageType>::New();
		inv->SetInput(img);
		inv->SetLowerThreshold(0);
		inv->SetUpperThreshold(0);
		inv->SetInsideValue(1);
		inv->SetOutsideValue(0);
		inv->Update();

		// Connected components
		auto cc = itk::ConnectedComponentImageFilter<ImageType, ImageType>::New();
		cc->SetInput(inv->GetOutput());
		cc->Update();

		auto labels = cc->GetOutput();

		// Identify border-touching labels
		std::set<unsigned int> border;
		itk::ImageRegionConstIterator<ImageType> it(labels, region);

		for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
			auto idx = it.GetIndex();
			bool isBorder =
				idx[0] == 0 || idx[0] == size[0] - 1 ||
				idx[1] == 0 || idx[1] == size[1] - 1 ||
				idx[2] == 0 || idx[2] == size[2] - 1;

			if (isBorder && it.Get() > 0)
				border.insert(it.Get());
		}

		// Measure each void
		unsigned int maxLabel = 0;
		for (it.GoToBegin(); !it.IsAtEnd(); ++it)
			if (it.Get() > maxLabel) maxLabel = it.Get();

		for (unsigned int L = 1; L <= maxLabel; L++) {
			if (border.count(L)) continue;

			// Build void mask
			auto voidMask = ImageType::New();
			voidMask->SetRegions(region);
			voidMask->Allocate();
			voidMask->FillBuffer(0);

			itk::ImageRegionIterator<ImageType> oit(voidMask, region);
			for (it.GoToBegin(), oit.GoToBegin(); !it.IsAtEnd(); ++it, ++oit)
				if (it.Get() == L) oit.Set(1);

			// Convert to VTK
			auto importer = vtkSmartPointer<vtkImageImport>::New();
			auto spacing = voidMask->GetSpacing();
			auto origin = voidMask->GetOrigin();

			importer->SetDataSpacing(spacing[0], spacing[1], spacing[2]);
			importer->SetDataOrigin(origin[0], origin[1], origin[2]);
			importer->SetWholeExtent(0, size[0] - 1, 0, size[1] - 1, 0, size[2] - 1);
			importer->SetDataExtentToWholeExtent();
			importer->SetDataScalarTypeToUnsignedChar();
			importer->SetNumberOfScalarComponents(1);
			importer->SetImportVoidPointer(voidMask->GetBufferPointer());
			importer->Update();

			auto surf = ExtractSurface(importer->GetOutput());

			auto mass = vtkSmartPointer<vtkMassProperties>::New();
			mass->SetInputData(surf);
			mass->Update();

			VoidMetrics M;
			M.volume = mass->GetVolume();
			M.surfaceArea = mass->GetSurfaceArea();
			M.equivalentDiameter = std::pow(6.0 * M.volume / M_PI, 1.0 / 3.0);
			M.sphericity = mass->GetNormalizedShapeIndex();

			auto com = vtkSmartPointer<vtkCenterOfMass>::New();
			com->SetInputData(surf);
			com->SetUseScalarsAsWeights(false);
			com->Update();
			com->GetCenter(M.centroid.data());

			// Thickness via MedialThicknessImageFilter3D
			using MT = itk::MedialThicknessImageFilter3D<ImageType, FloatImageType>;
			auto mt = MT::New();
			mt->SetInput(voidMask);
			mt->Update();
			auto output = mt->GetOutput();
			itk::ImageRegionConstIterator<FloatImageType> mit(output, output->GetLargestPossibleRegion());
			double maxThickness = 0.0;
			for (mit.GoToBegin(); !mit.IsAtEnd(); ++mit) {
				float v = mit.Get();
				if (v > maxThickness) maxThickness = v;
			}
			M.thickness = maxThickness;
			M.eulerCharacteristic = ComputeEulerCharacteristicFromSurface(surf);

			out.push_back(M);
		}

		return out;
	}

	// ------------------------------------------------------------
	// High-level API
	// ------------------------------------------------------------
	bool ComputeBoneMetrics(const std::string& filename, BoneMetrics& out)
	{
		out.bvtv = ComputeBVTV(filename);
		out.smi = ComputeSMI(filename);

		auto mat = ComputeVoxelMAT(filename);
		out.meanTrabecularThickness = 2.0 * mat.meanRadius;
		out.maxTrabecularThickness = 2.0 * mat.maxRadius;

		vtkSmartPointer<vtkImageData> img = ITKToVTKImage(filename);
		vtkSmartPointer<vtkPolyData> surf = ExtractSurface(img);
		out.meanCorticalThickness = ComputeSurfaceThickness(surf);

		return true;
	}

	bool ComputeVoidMetrics(const std::string& filename, std::vector<VoidMetrics>& out)
	{
		out = ExtractAndMeasureVoids(filename);
		return true;
	}

} // namespace BoneMorphometry
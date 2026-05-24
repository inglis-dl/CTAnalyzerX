#include "BoneMorphometry.h"

#include <itkBinaryBallStructuringElement.h>
#include <itkBinaryDilateImageFilter.h>
#include <itkBinaryThinningImageFilter3D.h>
#include <itkBinaryThresholdImageFilter.h>
#include <itkBoneMorphometryFeaturesFilter.h>
#include <itkConnectedComponentImageFilter.h>
#include <itkImage.h>
#include <itkImageFileReader.h>
#include <itkImageFileWriter.h>
#include <itkImageRegionIterator.h>
#include <itkImageToVTKImageFilter.h>
#include <itkInvertIntensityImageFilter.h>
#include <itkLabelImageToShapeLabelMapFilter.h>
#include <itkLabelGeometryImageFilter.h>
#include <itkLabelShapeKeepNObjectsImageFilter.h>
#include <itkMedialThicknessImageFilter3D.h>
#include <itkShapeLabelMapFilter.h>
#include <itkSignedMaurerDistanceMapImageFilter.h>
#include <itkVTKImageToImageFilter.h>


// Add these includes at the top:
#include <vtkPolyDataConnectivityFilter.h>

#include <vtkAppendPolyData.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCenterOfMass.h>
#include <vtkCleanPolyData.h>
#include <vtkContourFilter.h>
#include <vtkDijkstraGraphGeodesicPath.h>
#include <vtkDiscreteFlyingEdges3D.h>
#include <vtkDoubleArray.h>
#include <vtkIdList.h>
#include <vtkImageCast.h>
#include <vtkImageData.h>
#include <vtkImageEuclideanDistance.h>
#include <vtkImageGaussianSmooth.h>
#include <vtkImageGradient.h>
#include <vtkImageImport.h>
#include <vtkImageMagnitude.h>
#include <vtkImageMathematics.h>
#include <vtkImageThreshold.h>
#include <vtkInformation.h>
#include <vtkIntArray.h>
#include <vtkLine.h>
#include <vtkMarchingCubes.h>
#include <vtkMassProperties.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkMultiBlockDataSet.h>
#include <vtkPointLocator.h>
#include <vtkPolyLine.h>
#include <vtkPointData.h>
#include <vtkSmartPointer.h>
#include <vtkStripper.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <vtkXMLMultiBlockDataWriter.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkXMLPolyDataWriter.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace BoneMorphometry {

	// ------------------------------------------------------------
	// Load image from file
	// ------------------------------------------------------------
	ImageType::Pointer LoadImage(const std::string& filename)
	{
		try
		{
			auto reader = itk::ImageFileReader<ImageType>::New();
			reader->SetFileName(filename);
			reader->Update();
			
			auto output = ImageType::New();
			output->Graft(reader->GetOutput());
			return output;
		}
		catch (const itk::ExceptionObject& ex)
		{
			std::cerr << "Error loading image: " << ex.what() << std::endl;
			return nullptr;
		}
	}

	// ------------------------------------------------------------
	// VTK -> ITK conversion
	// ------------------------------------------------------------
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

	FloatImageType::Pointer VTKToITKFloatImage(vtkImageData* image)
	{
		if (!image)
			return nullptr;

		using VtkToItk = itk::VTKImageToImageFilter<FloatImageType>;
		auto vtkToItk = VtkToItk::New();
		vtkToItk->SetInput(image);
		vtkToItk->Update();

		auto out = FloatImageType::New();
		out->Graft(vtkToItk->GetOutput());
		return out;
	}


	// ------------------------------------------------------------
	// ITK -> VTK conversion (image-based)
	// ------------------------------------------------------------
	vtkSmartPointer<vtkImageData> ITKToVTKImage(ImageType::Pointer itkImage)
	{
		if (!itkImage)
			return nullptr;

		using ItkToVtk = itk::ImageToVTKImageFilter<ImageType>;
		auto itkToVtk = ItkToVtk::New();
		itkToVtk->SetInput(itkImage);
		itkToVtk->Update();

		auto out = vtkSmartPointer<vtkImageData>::New();
		out->DeepCopy(itkToVtk->GetOutput());

		return out;
	}

	// ------------------------------------------------------------
	// Extract surface mesh
	// ------------------------------------------------------------
	vtkSmartPointer<vtkPolyData> ExtractSurface(vtkImageData* image, double threshold)
	{
		if (!image)
			return nullptr;

		auto dfe = vtkSmartPointer<vtkDiscreteFlyingEdges3D>::New();
		dfe->SetInputData(image);
		dfe->SetValue(0, threshold);
		dfe->ComputeNormalsOn();
		dfe->ComputeGradientsOff();
		dfe->Update();

		auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
		clean->SetInputConnection(dfe->GetOutputPort());
		clean->Update();

		auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
		tri->SetInputConnection(clean->GetOutputPort());
		tri->Update();

		auto output = vtkSmartPointer<vtkPolyData>::New();
		output->DeepCopy(tri->GetOutput());
		return output;
	}

	// Hybrid approach combining ITK and custom methods
	bool ComputeBoneMetrics(ImageType::Pointer itkImage, BoneMetrics& out)
	{
		if (!itkImage)
			return false;

		// Use ITK filter for BVTV and trabecular metrics
		using FilterType = itk::BoneMorphometryFeaturesFilter<ImageType>;
		auto boneFilter = FilterType::New();
		boneFilter->SetInput(itkImage);
		boneFilter->SetThreshold(1);
		boneFilter->Update();

		out.VoxelBVTV = boneFilter->GetBVTV();
		out.VoxelTrabecularThickness = boneFilter->GetTbTh();
		out.VoxelBSBV = boneFilter->GetBSBV();
		out.VoxelTrabecularNumber = boneFilter->GetTbN();
		out.VoxelTrabecularSpacing = boneFilter->GetTbSp();

		vtkSmartPointer<vtkImageData> vtkImage = ITKToVTKImage(itkImage);
		auto range = vtkImage->GetScalarRange();
		const double t = range[1];

		vtkSmartPointer<vtkPolyData> surf = ExtractSurface(vtkImage, t);

		if (surf && surf->GetNumberOfPoints() > 0)
		{
			auto mass = vtkSmartPointer<vtkMassProperties>::New();
			mass->SetInputData(surf);
			mass->Update();

			double V = mass->GetVolume();
			double S = mass->GetSurfaceArea();
			out.MeshVolume = V;
			out.MeshSurfaceArea = S;
			if (V != 0)
				out.MeshBSBV = S / V;

			out.MeshSphericity = mass->GetNormalizedShapeIndex();
			out.MeshEulerNumber = ComputeEulerNumberFromSurface(surf);
		}

		auto spacing = itkImage->GetSpacing();
		double voxelVol = spacing[0] * spacing[1] * spacing[2];

		itk::ImageRegionConstIterator<ImageType> it(itkImage, itkImage->GetLargestPossibleRegion());

		double boneVol = 0;
		for (it.GoToBegin(); !it.IsAtEnd(); ++it) {
			if (it.Get() > 0) boneVol += voxelVol;
		}

		out.VoxelVolume = boneVol;

		// SMI requires custom implementation (not in ITK filter)
		out.StructureModelIndex = ComputeSMI(itkImage);

		return true;
	}

	// ------------------------------------------------------------
	// SMI (image-based)
	// ------------------------------------------------------------
	double ComputeSMI(ImageType::Pointer itkImage)
	{
		if (!itkImage)
			return 0.0;

		vtkSmartPointer<vtkImageData> vtkImage = ITKToVTKImage(itkImage);
		auto range = vtkImage->GetScalarRange();
		const double t = range[1];// 0.5 * (range[1] - range[0]);

		vtkSmartPointer<vtkPolyData> surf = ExtractSurface(vtkImage, t);

		if (!surf || surf->GetNumberOfPoints() == 0)
			return 0.0;

		auto mass = vtkSmartPointer<vtkMassProperties>::New();
		mass->SetInputData(surf);
		mass->Update();

		double V = mass->GetVolume();
		double S = mass->GetSurfaceArea();

		if (V <= 0.0 || S <= 0.0)
			return 0.0;

		// Dilate mask by 1 voxel
		using Struct = itk::BinaryBallStructuringElement<unsigned char, 3>;
		Struct se; se.SetRadius(1); se.CreateStructuringElement();

		auto dil = itk::BinaryDilateImageFilter<ImageType, ImageType, Struct>::New();
		dil->SetInput(itkImage);
		dil->SetKernel(se);
		dil->Update();

		vtkSmartPointer<vtkImageData> vtkImgDilated = ITKToVTKImage(dil->GetOutput());
		vtkSmartPointer<vtkPolyData> surfD = ExtractSurface(vtkImgDilated, t);

		if (!surfD || surfD->GetNumberOfPoints() == 0)
			return 0.0;

		auto massD = vtkSmartPointer<vtkMassProperties>::New();
		massD->SetInputData(surfD);
		massD->Update();

		double Vd = massD->GetVolume();
		double Sd = massD->GetSurfaceArea();

		double denominator = V * (Sd - S);
		if (std::abs(denominator) < 1e-10)
			return 0.0;

		return 6.0 * ((Vd - V) * S) / denominator;
	}

	// ------------------------------------------------------------
	// Euler characteristic from triangulated surface
	// ------------------------------------------------------------
	int ComputeEulerNumberFromSurface(vtkPolyData* surface)
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
	// Void extraction + morphometry (image-based)
	// Unified void extraction using ITK LabelMap + VTK surface analysis
	// Combines the best of both approaches
	// ------------------------------------------------------------
	std::vector<VoidMetrics> ExtractAndMeasureVoids(ImageType::Pointer vtkImage, double& totalVolume, double& totalSurfaceArea)
	{
		std::vector<VoidMetrics> out;
		totalVolume = 0;
		totalSurfaceArea = 0;

		if (!vtkImage)
			return out;

		auto region = vtkImage->GetLargestPossibleRegion();
		auto spacing = vtkImage->GetSpacing();

		// Step 1: Invert image (ITK native, cleaner than manual threshold)
		auto inv = itk::InvertIntensityImageFilter<ImageType>::New();
		inv->SetInput(vtkImage);
		inv->SetMaximum(255);
		inv->Update();

		// Step 2: Connected components with shape analysis
		using LabelType = unsigned short;
		using ShapeLabelObjectType = itk::ShapeLabelObject<LabelType, 3>;
		using LabelMapType = itk::LabelMap<ShapeLabelObjectType>;
		using LabelImageType = itk::Image<LabelType, 3>;

		auto connected = itk::ConnectedComponentImageFilter<ImageType, LabelImageType>::New();
		connected->SetFullyConnected(true); // 26-connectivity
		connected->SetInput(inv->GetOutput());
		connected->Update();

		// Step 3: Convert to shape label map for rich geometric properties
		using I2LType = itk::LabelImageToShapeLabelMapFilter<LabelImageType, LabelMapType>;
		auto i2l = I2LType::New();
		i2l->SetInput(connected->GetOutput());
		i2l->SetComputePerimeter(false); // Not meaningful for 3D
		i2l->Update();

		LabelMapType* labelMap = i2l->GetOutput();

		// Step 4: Remove border-touching labels (exterior background)
		int removedCount = 0;
		for (int i = labelMap->GetNumberOfLabelObjects() - 1; i >= 0; --i)
		{
			auto* obj = labelMap->GetNthLabelObject(i);

			if (obj->GetNumberOfPixelsOnBorder() > 0)
			{
				labelMap->RemoveLabel(obj->GetLabel());
				++removedCount;
			}
		}

		auto voidMask = ImageType::New();
		voidMask->SetRegions(region);
		voidMask->SetSpacing(spacing);
		voidMask->SetOrigin(vtkImage->GetOrigin());
		voidMask->SetDirection(vtkImage->GetDirection());
		voidMask->Allocate();

		// Step 5: Process each internal void with full VTK + ITK analysis
		for (unsigned int n = 0; n < labelMap->GetNumberOfLabelObjects(); ++n)
		{
			ShapeLabelObjectType* labelObject = labelMap->GetNthLabelObject(n);
			LabelType currentLabel = labelObject->GetLabel();

			VoidMetrics M;

			// ============================================================
			// Part A: ITK-based metrics (fast, already computed)
			// ============================================================

			// Basic geometric properties from ITK
			M.VoxelVolume = labelObject->GetPhysicalSize(); // Already in physical units (mm³)

			totalVolume += M.VoxelVolume;

			// Skip tiny voids (likely noise)
			if (labelObject->GetNumberOfPixels() == 1)
				continue;

			// Equivalent diameter from ITK
			double equivalentRadius = labelObject->GetEquivalentSphericalRadius();
			M.VoxelEquivalentDiameter = 2.0 * equivalentRadius;

			// ITK shape descriptors
			M.VoxelElongation = labelObject->GetElongation();
			M.VoxelRoundness = labelObject->GetRoundness();
			M.VoxelFlatness = labelObject->GetFlatness();

			// ============================================================
			// Part B: Create binary mask for this void (for VTK + advanced ITK)
			// ============================================================

			voidMask->FillBuffer(0);

			// Extract this label's pixels into binary mask
			itk::ImageRegionConstIterator<LabelImageType> labelIt(connected->GetOutput(), region);
			itk::ImageRegionIterator<ImageType> maskIt(voidMask, region);

			for (labelIt.GoToBegin(), maskIt.GoToBegin(); !labelIt.IsAtEnd(); ++labelIt, ++maskIt)
			{
				if (labelIt.Get() == currentLabel)
					maskIt.Set(1);
			}

			// ============================================================
			// Part C: VTK-based surface analysis
			// ============================================================

			vtkSmartPointer<vtkImageData> vtkVoidMask = ITKToVTKImage(voidMask);
			if (!vtkVoidMask)
			{
				std::cerr << "  Error: Failed to convert to VTK\n";
				continue;
			}

			vtkSmartPointer<vtkPolyData> surf = ExtractSurface(vtkVoidMask, 1);

			if (!surf || surf->GetNumberOfPoints() == 0)
			{
				std::cerr << "  Warning: No surface extracted\n";
			}
			else
			{
				// VTK mass properties (more accurate for surface area)
				auto mass = vtkSmartPointer<vtkMassProperties>::New();
				mass->SetInputData(surf);
				mass->Update();

				M.MeshSurfaceArea = mass->GetSurfaceArea();
				totalSurfaceArea += M.MeshSurfaceArea;

				// VTK's normalized shape index (sphericity measure)
				M.MeshSphericity = mass->GetNormalizedShapeIndex();
			}

			out.push_back(M);
		}

		return out;
	}

	// ------------------------------------------------------------
// Write all voids as a single multi-block VTK file
// ------------------------------------------------------------
// ------------------------------------------------------------
// Write all voids as a single combined VTK PolyData surface file
// Uses vtkAppendPolyData to merge all void surfaces
// ------------------------------------------------------------
	bool WriteSurfaces(ImageType::Pointer itkImage, const std::string& outputBoneFilename,
		const std::string& outputVoidsFilename)
	{
		if (!itkImage)
			return false;

		try
		{
			auto region = itkImage->GetLargestPossibleRegion();

			// ------------------------------------------------------------
			// Part 1: Write largest exterior bone surface
			// ------------------------------------------------------------
			vtkSmartPointer<vtkImageData> vtkBoneImage = ITKToVTKImage(itkImage);
			if (!vtkBoneImage)
			{
				std::cerr << "Error: Failed to convert input image to VTK\n";
				return false;
			}

			auto boneRange = vtkBoneImage->GetScalarRange();
			double boneThreshold = boneRange[1];

			vtkSmartPointer<vtkPolyData> allBoneSurfaces = ExtractSurface(vtkBoneImage, boneThreshold);
			if (!allBoneSurfaces || allBoneSurfaces->GetNumberOfPoints() == 0)
			{
				std::cerr << "Error: No bone surface extracted from input image\n";
				return false;
			}

			auto largestExterior = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
			largestExterior->SetInputData(allBoneSurfaces);
			largestExterior->SetExtractionModeToLargestRegion();
			largestExterior->Update();

			auto largestExteriorSurface = vtkSmartPointer<vtkPolyData>::New();
			largestExteriorSurface->DeepCopy(largestExterior->GetOutput());

			auto exteriorWriter = vtkSmartPointer<vtkXMLPolyDataWriter>::New();
			exteriorWriter->SetFileName(outputBoneFilename.c_str());
			exteriorWriter->SetInputData(largestExteriorSurface);
			exteriorWriter->SetDataModeToBinary();

			if (!exteriorWriter->Write())
			{
				std::cerr << "Error: Failed to write exterior bone surface: " << outputBoneFilename << "\n";
				return false;
			}

			std::cout << "Wrote largest exterior bone surface to " << outputBoneFilename << "\n";

			// ------------------------------------------------------------
			// Part 2: Extract and write combined void surfaces (existing logic)
			// ------------------------------------------------------------

			// Step 1: Invert to get non-bone regions
			auto inv = itk::InvertIntensityImageFilter<ImageType>::New();
			inv->SetInput(itkImage);
			inv->SetMaximum(boneRange[1]);
			inv->Update();

			// Step 2: Connected components
			using LabelType = unsigned short;
			using ShapeLabelObjectType = itk::ShapeLabelObject<LabelType, 3>;
			using LabelMapType = itk::LabelMap<ShapeLabelObjectType>;
			using LabelImageType = itk::Image<LabelType, 3>;

			auto connected = itk::ConnectedComponentImageFilter<ImageType, LabelImageType>::New();
			connected->SetFullyConnected(false);
			connected->SetInput(inv->GetOutput());
			connected->Update();

			// Step 3: Get label map
			using I2LType = itk::LabelImageToShapeLabelMapFilter<LabelImageType, LabelMapType>;
			auto i2l = I2LType::New();
			i2l->SetInput(connected->GetOutput());
			i2l->Update();

			LabelMapType* labelMap = i2l->GetOutput();

			// Step 4: Remove border-touching labels
			for (int i = labelMap->GetNumberOfLabelObjects() - 1; i >= 0; --i)
			{
				auto* obj = labelMap->GetNthLabelObject(i);
				if (obj->GetNumberOfPixelsOnBorder() > 0)
				{
					labelMap->RemoveLabel(obj->GetLabel());
				}
			}

			auto voidMask = ImageType::New();
			voidMask->SetRegions(region);
			voidMask->SetSpacing(itkImage->GetSpacing());
			voidMask->SetOrigin(itkImage->GetOrigin());
			voidMask->SetDirection(itkImage->GetDirection());
			voidMask->Allocate();

			// Step 5: Create appender to combine all surfaces
			auto appender = vtkSmartPointer<vtkAppendPolyData>::New();

			int voidCount = 0;
			for (unsigned int n = 0; n < labelMap->GetNumberOfLabelObjects(); ++n)
			{
				ShapeLabelObjectType* labelObject = labelMap->GetNthLabelObject(n);
				LabelType currentLabel = labelObject->GetLabel();

				if (labelObject->GetNumberOfPixels() == 1)
					continue;

				// Create binary mask for this void
				voidMask->FillBuffer(0);

				itk::ImageRegionConstIterator<LabelImageType> labelIt(connected->GetOutput(), region);
				itk::ImageRegionIterator<ImageType> maskIt(voidMask, region);

				for (labelIt.GoToBegin(), maskIt.GoToBegin(); !labelIt.IsAtEnd(); ++labelIt, ++maskIt)
				{
					if (labelIt.Get() == currentLabel)
						maskIt.Set(1);
				}

				// Extract surface
				vtkSmartPointer<vtkImageData> vtkVoidMask = ITKToVTKImage(voidMask);
				if (!vtkVoidMask)
				{
					std::cerr << "    Warning: Failed to convert void " << (voidCount + 1) << " to VTK\n";
					continue;
				}

				auto range = vtkVoidMask->GetScalarRange();
				const double t = range[1];

				vtkSmartPointer<vtkPolyData> surf = ExtractSurface(vtkVoidMask, t);
				if (!surf || surf->GetNumberOfPoints() == 0)
				{
					std::cerr << "    Warning: No surface extracted for void " << (voidCount + 1) << "\n";
					continue;
				}

				appender->AddInputData(surf);
				++voidCount;
			}

			auto combinedVoidSurface = vtkSmartPointer<vtkPolyData>::New();
			if (voidCount > 0)
			{
				appender->Update();
				combinedVoidSurface->DeepCopy(appender->GetOutput());
			}
			else
			{
				std::cout << "Debug: No internal voids found; writing empty void surface file\n";
			}

			auto voidWriter = vtkSmartPointer<vtkXMLPolyDataWriter>::New();
			voidWriter->SetFileName(outputVoidsFilename.c_str());
			voidWriter->SetInputData(combinedVoidSurface);
			voidWriter->SetDataModeToBinary();

			if (!voidWriter->Write())
			{
				std::cerr << "Error: Failed to write void surfaces: " << outputVoidsFilename << "\n";
				return false;
			}

			std::cout << "Successfully wrote combined void surfaces to " << outputVoidsFilename << "\n";
			return true;
		}
		catch (const std::exception& ex)
		{
			std::cerr << "Error writing combined void surfaces: " << ex.what() << std::endl;
			return false;
		}
	}

	bool WriteVoidsLabeledImage(ImageType::Pointer img,
							 const std::string& outputFilename, bool uniqueLabels)
	{
		if (!img)
			return false;

		try
		{
			auto region = img->GetLargestPossibleRegion();

			// Step 1: Invert to get non-bone regions
			auto inv = itk::InvertIntensityImageFilter<ImageType>::New();
			inv->SetInput(img);
			inv->SetMaximum(255);
			inv->Update();

			// Step 2: Connected components
			using LabelType = unsigned short;
			using LabelImageType = itk::Image<LabelType, 3>;

			auto connected = itk::ConnectedComponentImageFilter<ImageType, LabelImageType>::New();
			connected->SetFullyConnected(false);
			connected->SetInput(inv->GetOutput());
			connected->Update();

			// Step 3: Remove border-touching labels
			using ShapeLabelObjectType = itk::ShapeLabelObject<LabelType, 3>;
			using LabelMapType = itk::LabelMap<ShapeLabelObjectType>;
			using I2LType = itk::LabelImageToShapeLabelMapFilter<LabelImageType, LabelMapType>;

			auto i2l = I2LType::New();
			i2l->SetInput(connected->GetOutput());
			i2l->Update();

			LabelMapType* labelMap = i2l->GetOutput();

			for (int i = labelMap->GetNumberOfLabelObjects() - 1; i >= 0; --i)
			{
				auto* obj = labelMap->GetNthLabelObject(i);
				if (obj->GetNumberOfPixelsOnBorder() > 0)
				{
					labelMap->RemoveLabel(obj->GetLabel());
				}
			}

			// Step 4: Convert label map back to image
			using L2IType = itk::LabelMapToLabelImageFilter<LabelMapType, LabelImageType>;
			auto l2i = L2IType::New();
			l2i->SetInput(labelMap);
			l2i->Update();

			// Step 5: Cast to unsigned char for visualization (scale labels)
			auto labelImage = l2i->GetOutput();
			auto outputImage = ImageType::New();
			outputImage->SetRegions(region);
			outputImage->SetSpacing(img->GetSpacing());
			outputImage->SetOrigin(img->GetOrigin());
			outputImage->SetDirection(img->GetDirection());
			outputImage->Allocate();
			outputImage->FillBuffer(0);

			itk::ImageRegionConstIterator<LabelImageType> labelIt(labelImage, region);
			itk::ImageRegionIterator<ImageType> outIt(outputImage, region);

			// Scale labels to visible range (each void gets a distinct value)
			unsigned char labelIncrement = 255 / std::max(1u, static_cast<unsigned int>(labelMap->GetNumberOfLabelObjects()));

			for (labelIt.GoToBegin(), outIt.GoToBegin(); !labelIt.IsAtEnd(); ++labelIt, ++outIt)
			{
				LabelType label = labelIt.Get();
				if (label > 0)
				{
					// Scale label to 0-255 range for visualization
					unsigned char scaledLabel = static_cast<unsigned char>(std::min(255, static_cast<int>(label * labelIncrement)));
					outIt.Set(uniqueLabels? scaledLabel : 255);
				}
			}

			// Step 6: Write to file
			auto writer = itk::ImageFileWriter<ImageType>::New();
			writer->SetFileName(outputFilename);
			writer->SetInput(outputImage);
			writer->Update();

			std::cout << "Debug: Wrote " << labelMap->GetNumberOfLabelObjects()
				<< " labeled voids to " << outputFilename << "\n";

			return true;
		}
		catch (const itk::ExceptionObject& ex)
		{
			std::cerr << "Error writing labeled void image: " << ex.what() << std::endl;
			return false;
		}
	}

	// ------------------------------------------------------------
	// Compute Euler number using voxel-based 3D connectivity
	// Based on: Odgaard & Gundersen (1993) and Lee et al. (1994)
	// ------------------------------------------------------------
	int ComputeEulerNumber(ImageType::Pointer binaryImage)
	{
		if (!binaryImage)
			return 0;

		auto region = binaryImage->GetLargestPossibleRegion();
		auto size = region.GetSize();

		// Euler characteristic for 3D binary images using the Euler-Poincaré formula
		// For a 3D binary image, we count voxel configurations in 2×2×2 cubes
		// χ = C₁ - C₂ + C₃ - C₄ + C₅ - C₆ + C₇ - C₈
		// Where Cᵢ represents different vertex configurations

		// Simplified approach: Count contributions from each 2×2×2 cube
		// Using lookup table for 256 possible configurations

		long long eulerContribution = 0;

		// Iterate through all possible 2×2×2 cubes in the volume
		for (unsigned int z = 0; z < size[2] - 1; ++z)
		{
			for (unsigned int y = 0; y < size[1] - 1; ++y)
			{
				for (unsigned int x = 0; x < size[0] - 1; ++x)
				{
					// Read the 8 corners of the 2×2×2 cube
					ImageType::IndexType idx;
					unsigned char cube[8];

					idx[0] = x;     idx[1] = y;     idx[2] = z;     cube[0] = (binaryImage->GetPixel(idx) > 0) ? 1 : 0;
					idx[0] = x + 1; idx[1] = y;     idx[2] = z;     cube[1] = (binaryImage->GetPixel(idx) > 0) ? 1 : 0;
					idx[0] = x;     idx[1] = y + 1; idx[2] = z;     cube[2] = (binaryImage->GetPixel(idx) > 0) ? 1 : 0;
					idx[0] = x + 1; idx[1] = y + 1; idx[2] = z;     cube[3] = (binaryImage->GetPixel(idx) > 0) ? 1 : 0;
					idx[0] = x;     idx[1] = y;     idx[2] = z + 1; cube[4] = (binaryImage->GetPixel(idx) > 0) ? 1 : 0;
					idx[0] = x + 1; idx[1] = y;     idx[2] = z + 1; cube[5] = (binaryImage->GetPixel(idx) > 0) ? 1 : 0;
					idx[0] = x;     idx[1] = y + 1; idx[2] = z + 1; cube[6] = (binaryImage->GetPixel(idx) > 0) ? 1 : 0;
					idx[0] = x + 1; idx[1] = y + 1; idx[2] = z + 1; cube[7] = (binaryImage->GetPixel(idx) > 0) ? 1 : 0;

					// Convert to configuration index (0-255)
					int config = cube[0] | (cube[1] << 1) | (cube[2] << 2) | (cube[3] << 3) |
						(cube[4] << 4) | (cube[5] << 5) | (cube[6] << 6) | (cube[7] << 7);

					// Add Euler contribution from this cube configuration
					eulerContribution += GetEulerLUT(config);
				}
			}
		}

		return static_cast<int>(eulerContribution);
	}

	// ------------------------------------------------------------
	// Lookup table for Euler contributions from 2×2×2 cube configurations
	// Based on Toriwaki & Yonekura (2002) and Gray (1971)
	// ------------------------------------------------------------
	int GetEulerLUT(int config)
	{
		// Full 256-entry lookup table
		// Each entry represents the Euler contribution for that voxel configuration
		static const int eulerLUT[256] = {
			 0,  1,  1,  0,  1,  0, -2, -1,  1, -2,  0, -1,  0, -1, -1,  0,  // 0-15
			 1,  0, -2, -1, -2, -1, -1, -2, -6, -3, -3, -2, -3, -2,  0, -1,  // 16-31
			 1, -2,  0, -1, -2, -1, -3, -2, -6, -3, -3, -2, -3,  0, -2, -1,  // 32-47
			 0, -1, -1,  0, -1, -2, -2, -1, -3, -2, -2, -1, -2, -1, -1,  0,  // 48-63
			 1, -2, -6, -3, -2, -1, -3, -2,  0, -3, -3,  0, -1, -2, -2, -1,  // 64-79
			-2, -1, -3, -2, -3, -2,  0, -1, -3,  0,  0,  1, -2, -1,  1,  0,  // 80-95
			-6, -3, -3,  0, -3,  0,  0,  1, -3,  0,  0,  1,  0,  1,  1,  0,  // 96-111
			-3, -2, -2, -1,  0,  1,  1,  0,  0,  1,  1,  0,  1,  0,  0,  0,  // 112-127
			 1, -6, -2, -3, -2, -3, -3,  0,  0, -3, -1, -2, -1, -2, -2, -1,  // 128-143
			-2, -3, -3, -2, -3,  0, -2, -1, -3,  0,  0,  1,  0,  1,  1,  0,  // 144-159
			 0, -3, -1, -2, -3, -2, -2, -1, -3,  0,  0,  1, -2, -1,  1,  0,  // 160-175
			-1, -2, -2, -1, -2,  1, -1,  0,  0,  1,  1,  0,  1,  0,  0,  0,  // 176-191
			 0, -3, -3,  0, -1, -2, -2, -1, -1, -2, -2, -1, -2,  1, -1,  0,  // 192-207
			-1, -2,  0,  1, -2,  1,  1,  0, -2,  1, -1,  0, -1,  0,  0,  0,  // 208-223
			-1, -2, -2, -1, -2, -1,  1,  0, -2, -1,  1,  0,  1,  0,  0,  0,  // 224-239
			-1, -1, -1, -1, -1,  0,  0,  0, -1,  0,  0,  0,  0,  0,  0,  0   // 240-255
		};

		return eulerLUT[config];
	}

	TopologyMetrics ComputeTopology(ImageType::Pointer img)
	{
		TopologyMetrics metrics;

		if (!img)
			return metrics;

		// Step 1: Count all connected components
		auto ccFilter = itk::ConnectedComponentImageFilter<ImageType, ImageType>::New();
		ccFilter->SetInput(img);
		ccFilter->SetFullyConnected(true);
		ccFilter->Update();

		metrics.NumComponents = ccFilter->GetObjectCount();

		// Step 2: Extract only the largest connected component
		using LabelShapeFilterType = itk::LabelShapeKeepNObjectsImageFilter<ImageType>;
		auto labelShapeFilter = LabelShapeFilterType::New();
		labelShapeFilter->SetInput(ccFilter->GetOutput());
		labelShapeFilter->SetBackgroundValue(0);
		labelShapeFilter->SetNumberOfObjects(1); // Keep only the largest
		labelShapeFilter->SetAttribute(LabelShapeFilterType::LabelObjectType::NUMBER_OF_PIXELS);
		labelShapeFilter->Update();

		// Step 3: Create binary mask of largest component
		auto largestComponentMask = itk::BinaryThresholdImageFilter<ImageType, ImageType>::New();
		largestComponentMask->SetInput(labelShapeFilter->GetOutput());
		largestComponentMask->SetLowerThreshold(1);
		largestComponentMask->SetUpperThreshold(255);
		largestComponentMask->SetInsideValue(1);
		largestComponentMask->SetOutsideValue(0);
		largestComponentMask->Update();

		auto largestComponent = largestComponentMask->GetOutput();

		// Step 4: Compute Euler number on largest component only
		metrics.EulerNumber = ComputeEulerNumber(largestComponent);

		// Step 5: Count internal cavities within largest component
		auto inverter = itk::BinaryThresholdImageFilter<ImageType, ImageType>::New();
		inverter->SetInput(largestComponent);
		inverter->SetLowerThreshold(0);
		inverter->SetUpperThreshold(0);
		inverter->SetInsideValue(1);
		inverter->SetOutsideValue(0);
		inverter->Update();

		auto ccFilterInv = itk::ConnectedComponentImageFilter<ImageType, ImageType>::New();
		ccFilterInv->SetInput(inverter->GetOutput());
		ccFilterInv->SetFullyConnected(true);
		ccFilterInv->Update();

		// Subtract 1 for exterior background
		metrics.NumCavities = std::max(0, static_cast<int>(ccFilterInv->GetObjectCount()) - 1);

		// Step 6: Compute genus using Euler-Poincaré formula
		// For a single 3D object: χ = 2 - 2g - 2h
		// Where g = genus (handles), h = number of holes/cavities
		// Rearranged: g = (2 - 2h - χ) / 2 = 1 - h - χ/2

		// Standard formula: For a single connected component with cavities
		// χ = 2(1 - g) - 2h, where h = number of internal cavities
		// Solving for genus: g = 1 - h - χ/2
		metrics.Genus = (2 - 2 * metrics.NumCavities - metrics.EulerNumber) / 2;

		return metrics;
	}

	// ------------------------------------------------------------
	// High-level API (image-based)
	// ------------------------------------------------------------

	bool ComputeVoidMetrics(ImageType::Pointer itkImage, std::vector<VoidMetrics>& out,
		double& totalVolume, double& totalSurfaceArea)
	{
		if (!itkImage)
			return false;

		out = ExtractAndMeasureVoids(itkImage, totalVolume, totalSurfaceArea);
		return true;
	}

} // namespace BoneMorphometry

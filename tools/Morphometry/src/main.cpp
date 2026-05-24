#include "BoneMorphometry.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>

#include <itkImageFileWriter.h>
#include <itkBinaryThinningImageFilter3D.h>
#include <itkBinaryThresholdImageFilter.h>

#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include <vtkXMLPolyDataWriter.h>

#include <iostream>
#include <iomanip>
#include <cmath>

template<typename T>
void PrintValue(std::ostream& os, T value)
{
	auto oldFlags = os.flags();
	auto oldPrecision = os.precision();

	if (std::fabs(value) < 0.1 && value != 0.0)
	{
		os << std::scientific
			<< std::setprecision(6)
			<< value;
	}
	else
	{
		os << value;
	}

	os.flags(oldFlags);
	os.precision(oldPrecision);
}


int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "Usage: BoneMorphometry <bone_mask.nii>" << std::endl;
		return EXIT_FAILURE;
	}

	std::string filename = argv[1];

	std::cout << "----------------------------------------\n";
	std::cout << " Bone Morphometry Engine (ITK + VTK)\n";
	std::cout << "----------------------------------------\n";
	std::cout << "Input file: " << filename << "\n\n";

	auto image = BoneMorphometry::LoadImage(filename);
	if (!image)
	{
		std::cerr << "Error: Failed to load image file.\n";
		return EXIT_FAILURE;
	}

	std::filesystem::path p(filename);
	std::filesystem::path boneSurfPath = p.parent_path() / "bone_surface.vtp";
	std::filesystem::path voidsSurfPath = p.parent_path() / "void_surface.vtp";

	BoneMetrics bone;
	if (!BoneMorphometry::ComputeBoneMetrics(image, bone))
	{
		std::cerr << "Error: Failed to compute bone metrics.\n";
		return EXIT_FAILURE;
	}

	std::cout << "=== Solid Bone Morphometry ===\n";
	std::cout << std::fixed << std::setprecision(6);

	std::cout << "BV/TV: " << bone.VoxelBVTV << " %\n";
	std::cout << "BS/BV: " << bone.VoxelBSBV << " mm^-1\n";
	std::cout << "SMI: " << bone.StructureModelIndex << "\n";
	std::cout << "Trabecular Thickness (model based): " << bone.VoxelTrabecularThickness << " mm\n";
	std::cout << "Trabecular Number (model based): " << bone.VoxelTrabecularNumber << " mm\n";
	std::cout << "Trabecular Spacing (model based): " << bone.VoxelTrabecularSpacing << " mm\n";
	std::cout << "Voxel Volume: " << bone.VoxelVolume << "mm^3\n";
	std::cout << "Volume (surface based): " << bone.MeshVolume << "mm^3\n";
	std::cout << "Surface Area: " << bone.MeshSurfaceArea << "mm^2\n";
	std::cout << "BS/BV (surface based): " << bone.MeshBSBV << "mm^-1\n";
	std::cout << "Euler Number (surface based): " << bone.MeshEulerNumber << " \n";

	TopologyMetrics topo = BoneMorphometry::ComputeTopology(image);

	std::cout << "Bone topology - Euler: " << topo.EulerNumber
		<< ", Cavities: " << topo.NumCavities
		<< ", Genus: " << topo.Genus << "\n";


	std::vector<VoidMetrics> voids;
	double totalVoidVolume = 0;
	double totalVoidSurfaceArea = 0;
	if (!BoneMorphometry::ComputeVoidMetrics(image, voids, totalVoidVolume, totalVoidSurfaceArea))
	{
		std::cerr << "Error: Failed to compute void metrics.\n";
		return EXIT_FAILURE;
	}

	std::cout << "=== Internal Void Morphometry ===\n";
	std::cout << "Number of internal voids: ";
	PrintValue(std::cout, voids.size());
	std::cout << "\n";
	std::cout << "Total void volume: ";
	PrintValue(std::cout, totalVoidVolume);
	std::cout << " mm^3\n";
	std::cout << "Total void surface area: ";
	PrintValue(std::cout, totalVoidSurfaceArea);
	std::cout << " mm^2\n\n";

	/*
	int idx = 1;
	for (const auto& v : voids)
	{
		std::cout << "Void " << idx++ << ":\n";
		std::cout << " Volume: " << v.VoxelVolume << " mm^3\n";
		std::cout << " Surface Area: " << v.MeshSurfaceArea << " mm^2\n";
		std::cout << " Equivalent Diameter: " << v.VoxelEquivalentDiameter << " mm\n";
		std::cout << " Sphericity: " << v.MeshSphericity << "\n";
		std::cout << " Elongation: " << v.VoxelElongation << "\n";
		std::cout << " Roundness: " << v.VoxelRoundness << "\n";
		std::cout << " Flatness: " << v.VoxelFlatness << "\n";
	}
	*/
	int idx = 1;
	for (const auto& v : voids)
	{
		std::cout << "Void " << idx++ << ":\n";

		std::cout << " Volume: ";
		PrintValue(std::cout, v.VoxelVolume);
		std::cout << " mm^3\n";

		std::cout << " Surface Area: ";
		PrintValue(std::cout, v.MeshSurfaceArea);
		std::cout << " mm^2\n";

		std::cout << " Equivalent Diameter: ";
		PrintValue(std::cout, v.VoxelEquivalentDiameter);
		std::cout << " mm\n";

		std::cout << " Sphericity: ";
		PrintValue(std::cout, v.MeshSphericity);
		std::cout << "\n";

		std::cout << " Elongation: ";
		PrintValue(std::cout, v.VoxelElongation);
		std::cout << "\n";

		std::cout << " Roundness: ";
		PrintValue(std::cout, v.VoxelRoundness);
		std::cout << "\n";

		std::cout << " Flatness: ";
		PrintValue(std::cout, v.VoxelFlatness);
		std::cout << "\n";
	}

	std::cout << "Morphometry analysis complete.\n";
	
	BoneMorphometry::WriteSurfaces(image, boneSurfPath.string(), voidsSurfPath.string());

	return EXIT_SUCCESS;
}
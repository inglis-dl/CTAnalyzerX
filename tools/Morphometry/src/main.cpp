#include "BoneMorphometry.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

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

	// ------------------------------------------------------------
	// Compute solid bone morphometry
	// ------------------------------------------------------------
	auto image = BoneMorphometry::LoadImage(filename);
	BoneMetrics bone;
	std::vector<VoidMetrics> voids;
	BoneMorphometry::ComputeBoneMetrics(image, bone);

	std::cout << "=== Solid Bone Morphometry ===\n";
	std::cout << std::fixed << std::setprecision(6);

	std::cout << "BV/TV: " << bone.bvtv << "\n";
	std::cout << "SMI: " << bone.smi << "\n";
	std::cout << "Mean Trabecular Thickness: " << bone.meanTrabecularThickness << " mm\n";
	std::cout << "Mean Cortical Thickness: " << bone.meanCorticalThickness << " mm\n\n";

	// ------------------------------------------------------------
	// Compute void morphometry
	// ------------------------------------------------------------
	BoneMorphometry::ComputeVoidMetrics(image, voids);

	std::cout << "=== Internal Void Morphometry ===\n";
	std::cout << "Number of internal voids: " << voids.size() << "\n\n";

	int idx = 1;
	for (const auto& v : voids)
	{
		std::cout << "Void " << idx++ << ":\n";
		std::cout << " Volume: " << v.volume << " mm^3\n";
		std::cout << " Surface Area: " << v.surfaceArea << " mm^2\n";
		std::cout << " Equivalent Diameter:" << v.equivalentDiameter << " mm\n";
		std::cout << " Sphericity: " << v.sphericity << "\n";
		std::cout << " Thickness: " << v.thickness << " mm\n";
		std::cout << " Euler Characteristic: " << v.eulerCharacteristic << "\n";
		std::cout << " Centroid: ("
			<< v.centroid[0] << ", "
			<< v.centroid[1] << ", "
			<< v.centroid[2] << ")\n\n";
	}

	std::cout << "Morphometry analysis complete.\n";
	return EXIT_SUCCESS;
}
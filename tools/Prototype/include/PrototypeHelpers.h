#pragma once

// ---------------------------------------------------------------------------
// PrototypeHelpers.h
// ---------------------------------------------------------------------------

#include "ProcessHelpers.h"

#include <vtkSmartPointer.h>

class vtkActor;
class vtkColorTransferFunction;
class vtkImageData;
class vtkScalarBarActor;

namespace PrototypeHelpers
{
	// -----------------------------------------------------------------------
	// Bone island segmentation - VTK morphological pipeline
	// -----------------------------------------------------------------------

	// Alternative segmentation pipeline that pre-processes the input with:
	//   1. vtkImageGaussianSmooth			suppress noise before morphology.
	//   2. vtkImageContinuousErode3D		shrink thin connections between regions.
	//   3. vtkImageContinuousDilate3D		restore island cores after erosion.
	//   4. vtkImageThresholdConnectivity	seed-based connected-threshold fill.
	// The resulting label image uses one integer label per accepted seed and
	// the returned BoneIsland vector is coloured by island voxel-count scale,
	// identical to segmentBoneIslands.
	// smoothStdDev   : Gaussian standard deviation in voxel units (default 1.0).
	// morphKernelSize: half-width of the erode/dilate structuring element
	//                    (default 1  3x3x3 kernel).
	std::vector<ProcessHelpers::BoneIsland> segmentBoneIslandsAlternate(
		vtkImageData* reslicedImage,
		double                                   threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outLabelImage,
		double                                   smoothStdDev = 1.0,
		int                                      morphKernelSize = 1,
		const std::function<void(int)>& progressCb = nullptr);

#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	// -----------------------------------------------------------------------
	// Bone island segmentation - ITK ImageGridCutFilter (graph cut)
	// -----------------------------------------------------------------------
	//
	// Uses GraphCut::FilterType which resolves to ImageGridCutFilter (GridCut,
	// multi-threaded) when GRIDCUT_LIBRARY_AVAILABLE is defined, or falls back
	// to ImageGraphCut3DKolmogorovFilter (single-threaded) otherwise.
	//
	// Foreground seed image: voxel paths from the bone centroid out to each of
	// the 5 selected landmark surface points (Lpos excluded - adjacent bone side).
	// Background seed image: threshold-gated outward rays from the same 5
	// landmark tips, stopping on re-entry into bone-density tissue.
	//
	// sigma  : Gaussian width for the boundary term exp(-delta^2 / 2*sigma^2).
	//            For 16-bit CT bone data try 50-200; smaller = sharper edges.
	// minIslandVoxels : connected components smaller than this are discarded.
	void buildGraphCutSeedImages(
		vtkImageData* reslicedImage,
		const std::array<std::array<std::array<double, 3>, 2>, 3>& landmarkPoints,
		const double                             eigenvectors[3][3],
		vtkSmartPointer<vtkImageData>& outForegroundSeeds,
		vtkSmartPointer<vtkImageData>& outBackgroundSeeds,
		double                                   threshold);

	std::vector<BoneIsland> segmentBoneIslandsGraphCut(
		vtkImageData* reslicedImage,
		double                                   threshold,
		const std::vector<std::array<double, 3>>& foregroundSeedsWorld,
		const std::vector<std::array<double, 3>>& backgroundSeedsWorld,
		vtkSmartPointer<vtkImageData>& outLabelImage,
		double                                   sigma = 100.0,
		vtkIdType                                minIslandVoxels = 50,
		const std::function<void(int)>& progressCb = nullptr);

	// Converts a binary seed image (1 = seed, 0 = background) produced by
	// buildGraphCutSeedImages into a vtkPolyData of point vertices suitable
	// for rendering as a point cloud overlay.
	// Each above-zero voxel centre is emitted as one vertex.
	// Colour the returned actor with r,g,b to distinguish FG from BG seeds.
	vtkSmartPointer<vtkActor> makeSeedImageActor(
		vtkImageData* seedImage,
		double        r, double g, double b,
		double        pointSize = 3.0);
#endif // CTAXPROTOTYPE_ENABLE_GRAPH_CUT

	// -----------------------------------------------------------------------
	// VTK actor / prop builders
	// -----------------------------------------------------------------------

	vtkSmartPointer<vtkActor> makeLineActor(
		const double p0[3], const double p1[3],
		double r, double g, double b, double lineWidth = 2.0);

	vtkSmartPointer<vtkActor> makeSphereActor(
		const double centre[3], double radius,
		double r, double g, double b);

	vtkSmartPointer<vtkActor> makeRingActor(
		const double centre[3], const double normal[3], double radius,
		double r, double g, double b, double lineWidth = 2.0);

	vtkSmartPointer<vtkActor> makeIslandSurfaceActor(
		vtkImageData* labelImage,
		int           islandLabel,
		double        r, double g, double b,
		double        opacity = 0.6);

	// Builds a cool-to-warm color transfer function that maps scalar values in
	// [minVal, maxVal] to colours.  Used to colour island surfaces by voxel
	// count and to drive the scalar bar annotation.
	// When minVal == maxVal a single mid-range colour is used.
	vtkSmartPointer<vtkColorTransferFunction> makeIslandColorTF(
		double minVal, double maxVal);

	// Builds a vertical scalar bar actor annotated with voxel-count labels.
	// colorTF   : the same transfer function used to colour the surfaces.
	// minVoxels : lower bound displayed on the bar (smallest island).
	// maxVoxels : upper bound displayed on the bar (largest island).
	// The bar is positioned in the upper-right corner of the viewport at
	// normalised coordinates and sized so it does not obstruct the volume.
	vtkSmartPointer<vtkScalarBarActor> makeIslandScalarBar(
		vtkColorTransferFunction* colorTF,
		vtkIdType                 minVoxels,
		vtkIdType                 maxVoxels);

} // namespace PrototypeHelpers
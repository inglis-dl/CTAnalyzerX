#pragma once

// ---------------------------------------------------------------------------
// PrototypeHelpers.h
// ---------------------------------------------------------------------------

#include <vtkSmartPointer.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <vector>

class vtkActor;
class vtkColorTransferFunction;
class vtkImageData;
class vtkScalarBarActor;

namespace PrototypeHelpers
{
	// -----------------------------------------------------------------------
	// JSON / sidecar I/O
	// -----------------------------------------------------------------------

	QJsonObject readJsonObjectFileOrThrow(const QString& path);
	QString     cropPathFromSidecarOrThrow(const QJsonObject& obj);
	double      thresholdFromSidecar(const QJsonObject& obj);

	// -----------------------------------------------------------------------
	// Image statistics
	// -----------------------------------------------------------------------

	double computeScalarStdDev(vtkImageData* image);
	QJsonObject computeScalarThresholdStats(vtkImageData* image, double threshold);

	// -----------------------------------------------------------------------
	// PCA result
	// -----------------------------------------------------------------------

	struct PcaResult
	{
		double centroid[3];
		double axes[3][3];
		double eigenvalues[3];
		double circumRadius;
		bool   valid = false;
	};

	// -----------------------------------------------------------------------
	// PCA
	// -----------------------------------------------------------------------

	bool computePca(vtkImageData* image, double threshold,
					PcaResult& result,
					const std::function<void(int)>& progressCb = nullptr);

	// -----------------------------------------------------------------------
	// Ray-AABB intersection (slab method)
	// -----------------------------------------------------------------------

	bool rayAabbIntersect(const double rayOrigin[3], const double rayDir[3],
						  const double bbMin[3], const double bbMax[3],
						  double& tEntry, double& tExit);

	bool rayAabbExit(const double rayOrigin[3], const double rayDir[3],
					 const double bbMin[3], const double bbMax[3],
					 double& tExit);

	// -----------------------------------------------------------------------
	// Surface search
	// -----------------------------------------------------------------------

	void findSurfacePointFromBoundary(vtkImageData* image,
									  const double centroid[3],
									  const double axisDir[3],
									  double threshold,
									  double outWorld[3]);

	// -----------------------------------------------------------------------
	// Bone island segmentation
	// -----------------------------------------------------------------------

	struct BoneIsland
	{
		int         label;        // unique integer label (1-based)
		vtkIdType   voxelCount;   // number of voxels in the island
		double      seedWorld[3]; // world-space seed point
		int         seedVoxel[3]; // nearest voxel index of seedWorld
		QJsonObject json;         // serialised summary
	};

	std::vector<BoneIsland> segmentBoneIslands(
		vtkImageData* reslicedImage,
		double                                   threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outLabelImage,
		const std::function<void(int)>& progressCb = nullptr);

	// Parallel version of segmentBoneIslands.
	// Identical signature and output contract; uses QtConcurrent for the
	// binary mask and per-seed BFS, with a union-find merge pass to collapse
	// seeds that landed on the same physical bone.
	std::vector<BoneIsland> segmentBoneIslandsParallel(
		vtkImageData* reslicedImage,
		double                                     threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outLabelImage,
		const std::function<void(int)>& progressCb = nullptr);

	// Walk each seed inward toward the PCA centroid along its eigen-axis direction
	// until the first voxel that satisfies scalar >= threshold is found.
	// Seeds that already satisfy the threshold at their original position are
	// returned unchanged.  Seeds for which no qualifying voxel is found before
	// reaching the centroid are also returned unchanged as a safe fallback.
	//
	// seedsWorld layout (mirrors the 6-element vector built in onRegions):
	//   s = 0,1  ->  axis 0  (positive tip, negative tip)
	//   s = 2,3  ->  axis 1  (positive tip, negative tip)
	//   s = 4,5  ->  axis 2  (positive tip, negative tip)
	std::vector<std::array<double, 3>> computeInwardAdjustedSeeds(
		vtkImageData* image,
		double                                     threshold,
		const std::vector<std::array<double, 3>>& originalSeedsWorld,
		const PcaResult& pca);

	// -----------------------------------------------------------------------
	// Orphan island identification
	//
	// Performs an unseeded 26-connected BFS over all above-threshold voxels
	// to label every connected foreground component.  Components that contain
	// at least one seed world point are marked as seeded; all others are
	// written as 1 into outOrphanMask (0 elsewhere).
	//
	// The result is cached in PrototypeMainWindow::m_orphanMaskImage after
	// onInitialize() step 3 and consumed by onClean() to include orphan
	// regions in the removal mask without a second threshold pass.
	// -----------------------------------------------------------------------
	void identifyOrphanIslands(
		vtkImageData* reslicedImage,
		double                                     threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outOrphanMask,
		const std::function<void(int)>& progressCb = nullptr);

	// -----------------------------------------------------------------------
	// Bone island segmentation - VTK morphological pipeline
	// -----------------------------------------------------------------------

	// Alternative segmentation pipeline that pre-processes the input with:
	//   1. vtkImageGaussianSmooth     - suppress noise before morphology.
	//   2. vtkImageContinuousErode3D  - shrink thin connections between regions.
	//   3. vtkImageContinuousDilate3D - restore island cores after erosion.
	//   4. vtkImageThresholdConnectivity - seed-based connected-threshold fill.
	// The resulting label image uses one integer label per accepted seed and
	// the returned BoneIsland vector is coloured by island voxel-count scale,
	// identical to segmentBoneIslands.
	// `smoothStdDev`   : Gaussian standard deviation in voxel units (default 1.0).
	// `morphKernelSize`: half-width of the erode/dilate structuring element
	//                    (default 1  3x3x3 kernel).
	std::vector<BoneIsland> segmentBoneIslandsAlternate(
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
	// `sigma`  : Gaussian width for the boundary term exp(-delta^2 / 2*sigma^2).
	//            For 16-bit CT bone data try 50-200; smaller = sharper edges.
	// `minIslandVoxels` : connected components smaller than this are discarded.
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
	// `colorTF`   : the same transfer function used to colour the surfaces.
	// `minVoxels` : lower bound displayed on the bar (smallest island).
	// `maxVoxels` : upper bound displayed on the bar (largest island).
	// The bar is positioned in the upper-right corner of the viewport at
	// normalised coordinates and sized so it does not obstruct the volume.
	vtkSmartPointer<vtkScalarBarActor> makeIslandScalarBar(
		vtkColorTransferFunction* colorTF,
		vtkIdType                 minVoxels,
		vtkIdType                 maxVoxels);

	// -----------------------------------------------------------------------
	// Region statistics helpers  (used by onRegions iterative loop)
	// -----------------------------------------------------------------------

	// Mean and standard deviation of reslicedImage intensities at all voxels
	// where labelImage scalar > 0.  Returns {0, 0} when no labelled voxels exist.
	struct RegionStats { double mean = 0.0; double stdDev = 0.0; };

	RegionStats computeRegionStats(vtkImageData* reslicedImage,
								   vtkImageData* labelImage);

	// Total volume (mm³) of all above-zero voxels in labelImage.
	double computeRegionVolumeMm3(vtkImageData* labelImage);

} // namespace PrototypeHelpers
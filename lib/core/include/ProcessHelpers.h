#pragma once

// ---------------------------------------------------------------------------
// ProcessHelpers.h
// ---------------------------------------------------------------------------

#include <vtkSmartPointer.h>
#include <vtkVector.h>
#include <vtkVectorOperators.h>

#include <QJsonObject>
#include <QString>

#include <functional>
#include <vector>
#include <array>

class vtkImageData;

namespace ProcessHelpers
{
	using Vec3 = vtkVector3d;

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
	QJsonObject computeScalarThresholdStats(vtkImageData* image, const double& threshold);

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

	bool computePca(vtkImageData* image, const double& threshold,
					PcaResult& result,
					const std::function<void(int)>& progressCb = nullptr);

	// -----------------------------------------------------------------------
	// PCA axis canonicalisation
	//
	// Enforces a canonical orientation on a PcaResult before it is used to
	// build a vtkImageReslice ResliceAxes matrix:
	//
	//   1. axes[0] is signed so +X in the resliced output points from the
	//      centroid toward the farthest foreground surface point along that
	//      axis (the bone tip).  In SliceView XY the tip appears on the
	//      right; in VolumeView the bone long-axis runs left-to-right.
	//
	//   2. axes[2] is recomputed as cross(axes[0], axes[1]) to guarantee a
	//      right-handed output coordinate frame after any sign flip.
	//      Without this a flipped axes[0] produces a mirrored output image.
	//
	// This is a no-op when pca.valid is false or image is nullptr.
	//
	// For segmented bone masks on export the furthest vertical bone voxel along the e1 eigen
	// vector is below the e0 axis so apply flip = true during onExport.
	//
	// -----------------------------------------------------------------------
	void orientPcaAxesForCanonicalReslice(vtkImageData* image,
										  const double& threshold,
										  PcaResult& pca, const bool& flip = false);

	// -----------------------------------------------------------------------
	// Ray-AABB intersection (slab method)
	// -----------------------------------------------------------------------

	bool rayAabbIntersect(const double rayOrigin[3], const double rayDir[3],
						  const double bbMin[3], const double bbMax[3],
						  double& tEntry, double& tExit);

	bool rayAabbIntersect(const Vec3& rayOrigin, const Vec3& rayDir,
						  const Vec3& bbMin, const Vec3& bbMax,
						  double& tEntry, double& tExit);

	bool rayAabbExit(const double rayOrigin[3], const double rayDir[3],
					 const double bbMin[3], const double bbMax[3],
					 double& tExit);

	bool rayAabbExit(const Vec3& rayOrigin, const Vec3& rayDir,
					 const Vec3& bbMin, const Vec3& bbMax,
					 double& tExit);

	vtkIdType flatten(const int& ix, const int& iy, const int& iz, const int dims[3]);

	bool IntersectLineWithBox(const vtkVector3d& c, const vtkVector3d& e,
		vtkImageData* image, vtkVector3d& p0, vtkVector3d& p1);

	// -----------------------------------------------------------------------
	// Surface search
	// -----------------------------------------------------------------------

	void findSurfacePointFromBoundary(vtkImageData* image,
									  const double centroid[3],
									  const double axisDir[3],
									  double threshold,
									  double outWorld[3]);

	void findSurfacePointFromBoundary(vtkImageData* image,
									  const Vec3& centroid,
									  const Vec3& axisDir,
									  double threshold,
									  Vec3& outWorld);

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
	std::vector<BoneIsland> segmentBoneIslandsParallel(
		vtkImageData* reslicedImage,
		double                                     threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outLabelImage,
		const std::function<void(int)>& progressCb = nullptr);

	// Walk each seed inward toward the PCA centroid along its eigen-axis direction
	std::vector<std::array<double, 3>> computeInwardAdjustedSeeds(
		vtkImageData* image,
		double                                     threshold,
		const std::vector<std::array<double, 3>>& originalSeedsWorld,
		const PcaResult& pca);

	// -----------------------------------------------------------------------
	// Orphan island identification
	// -----------------------------------------------------------------------
	void identifyOrphanIslands(
		vtkImageData* reslicedImage,
		double                                     threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outOrphanMask,
		const std::function<void(int)>& progressCb = nullptr);

	// -----------------------------------------------------------------------
	// Region statistics helpers  (used by onRegions iterative loop)
	// -----------------------------------------------------------------------

	struct RegionStats { double mean = 0.0; double stdDev = 0.0; };

	RegionStats computeRegionStats(vtkImageData* reslicedImage,
								   vtkImageData* labelImage);

	// Total volume (mm^3) of all above-zero voxels in labelImage.
	double computeRegionVolumeMm3(vtkImageData* labelImage);

} // namespace ProcessHelpers

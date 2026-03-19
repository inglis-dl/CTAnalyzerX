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
	// Ray–AABB intersection (slab method)
	// -----------------------------------------------------------------------

	bool rayAabbIntersect(const double rayOrigin[3], const double rayDir[3],
	                      const double bbMin[3],    const double bbMax[3],
	                      double& tEntry, double& tExit);

	bool rayAabbExit(const double rayOrigin[3], const double rayDir[3],
	                 const double bbMin[3],    const double bbMax[3],
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
		vtkImageData*                            reslicedImage,
		double                                   threshold,
		const std::vector<std::array<double,3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>&           outLabelImage,
		const std::function<void(int)>&          progressCb = nullptr);

	// -----------------------------------------------------------------------
	// Bone island segmentation — VTK morphological pipeline
	// -----------------------------------------------------------------------

	// Alternative segmentation pipeline that pre-processes the input with:
	//   1. vtkImageGaussianSmooth    – suppress noise before morphology.
	//   2. vtkImageContinuousErode3D – shrink thin connections between regions.
	//   3. vtkImageContinuousDilate3D– restore island cores after erosion.
	//   4. vtkImageThresholdConnectivity – seed-based connected-threshold fill.
	// The resulting label image uses one integer label per accepted seed and
	// the returned BoneIsland vector is coloured by island voxel-count scale,
	// identical to segmentBoneIslands.
	// `smoothStdDev`   : Gaussian standard deviation in voxel units (default 1.0).
	// `morphKernelSize`: half-width of the erode/dilate structuring element
	//                    (default 1 ? 3×3×3 kernel).
	std::vector<BoneIsland> segmentBoneIslandsAlternate(
		vtkImageData*                            reslicedImage,
		double                                   threshold,
		const std::vector<std::array<double,3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>&           outLabelImage,
		double                                   smoothStdDev    = 1.0,
		int                                      morphKernelSize = 1,
		const std::function<void(int)>&          progressCb      = nullptr);

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

} // namespace PrototypeHelpers
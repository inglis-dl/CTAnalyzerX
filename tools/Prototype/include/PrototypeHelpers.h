#pragma once

// ---------------------------------------------------------------------------
// PrototypeHelpers.h
//
// Free-function helpers used by PrototypeMainWindow.  Factored into their own
// translation unit so the window class source stays focused on UI/event logic.
//
// All functions are declared in the PrototypeHelpers namespace.
// ---------------------------------------------------------------------------

#include "PrototypeMainWindow.h" // for PrototypeMainWindow::PcaResult

#include <vtkSmartPointer.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <vector>

class vtkActor;
class vtkImageData;

namespace PrototypeHelpers
{
	// -----------------------------------------------------------------------
	// JSON / sidecar I/O
	// -----------------------------------------------------------------------

	// Reads and parses a JSON file at `path`.  Throws std::runtime_error on any
	// I/O or parse failure, or when the top-level value is not a JSON object.
	QJsonObject readJsonObjectFileOrThrow(const QString& path);

	// Extracts crop.outputPath from a sidecar JSON object.
	// Throws std::runtime_error when the key is absent or empty.
	QString cropPathFromSidecarOrThrow(const QJsonObject& obj);

	// Returns the threshold scalar value stored in obj["threshold"]["value"], or
	// std::numeric_limits<double>::quiet_NaN() when the key is absent.
	double thresholdFromSidecar(const QJsonObject& obj);

	// -----------------------------------------------------------------------
	// Image statistics
	// -----------------------------------------------------------------------

	// Computes the population standard deviation of all scalar values in `image`.
	// Returns 1.0 when the image is null or contains no points.
	double computeScalarStdDev(vtkImageData* image);

	// -----------------------------------------------------------------------
	// PCA
	// -----------------------------------------------------------------------

	// Computes PCA of the above-threshold voxels of `image` and writes the result
	// into `result`.
	//
	// progressCb(percent) is invoked at key milestones in [0, 100] so the caller
	// can update a progress bar.  Pass nullptr to suppress all callbacks.
	//
	// Returns false and emits a qWarning when fewer than 3 above-threshold voxels
	// are found or when mandatory VTK data is missing.
	bool computePca(vtkImageData* image, double threshold,
	                PrototypeMainWindow::PcaResult& result,
	                const std::function<void(int)>& progressCb = nullptr);

	// -----------------------------------------------------------------------
	// Ray–AABB intersection (slab method)
	// -----------------------------------------------------------------------

	// Full slab intersection: returns both the entry and exit parametric distances
	// for the ray (rayOrigin + t*rayDir) against [bbMin, bbMax].
	//
	// When rayOrigin is INSIDE the box, tEntry <= 0 and tExit > 0.
	// Returns false when the ray misses the box or exits behind the origin.
	bool rayAabbIntersect(const double rayOrigin[3], const double rayDir[3],
	                      const double bbMin[3],    const double bbMax[3],
	                      double& tEntry, double& tExit);

	// Exit-only slab intersection: returns the parametric distance tExit at which
	// the ray exits [bbMin, bbMax].
	//
	// Works correctly when rayOrigin is INSIDE the box (the common case when the
	// PCA centroid lies within the image volume).
	// Returns false only when the ray is parallel to a slab AND the origin is
	// outside that slab.
	bool rayAabbExit(const double rayOrigin[3], const double rayDir[3],
	                 const double bbMin[3],    const double bbMax[3],
	                 double& tExit);

	// -----------------------------------------------------------------------
	// Surface search
	// -----------------------------------------------------------------------

	// Finds the first above-threshold surface voxel along one eigen axis by:
	//   1. Casting a ray from `centroid` in `axisDir` (a unit eigenvector).
	//   2. Finding where that ray exits the image AABB — this is the walk start.
	//   3. Walking back from the exit point toward `centroid` (–axisDir), one
	//      voxel step at a time.
	//   4. Returning the first voxel whose scalar >= threshold.
	//
	// Falls back to `centroid` when the ray misses the box or no above-threshold
	// voxel is found along the ray.
	void findSurfacePointFromBoundary(vtkImageData* image,
	                                  const double centroid[3],
	                                  const double axisDir[3],
	                                  double threshold,
	                                  double outWorld[3]);

	// -----------------------------------------------------------------------
	// Bone island segmentation
	// -----------------------------------------------------------------------

	// Result of one seeded region-growing run on a single seed point.
	struct BoneIsland
	{
		int         label;        // unique integer label assigned to this island (1-based)
		vtkIdType   voxelCount;   // number of voxels in the island
		double      seedWorld[3]; // the world-space seed point that grew this island
		int         seedVoxel[3]; // nearest voxel index of seedWorld in the resliced image
		QJsonObject json;         // serialised summary (label, voxelCount, seedWorld, boundingBox)
	};

	// Thresholds `reslicedImage` at `threshold`, then runs an independent
	// 26-connected BFS flood-fill from each world-space seed in `seedsWorld`.
	//
	// Each seed that falls on an above-threshold voxel grows its own labelled
	// island.  Seeds that land on a below-threshold voxel or outside the image
	// extent are skipped (a qWarning is emitted for each).  If two seeds happen
	// to flood-fill into the same connected component the second seed will find
	// all reachable voxels already claimed and produce an island with voxelCount
	// == 0 (also warned and excluded from the output).
	//
	// progressCb(percent) is called at key milestones in [0, 100].
	//
	// Returns one BoneIsland per seed that successfully grew a non-empty region.
	// The label image (unsigned char, one scalar per voxel, 0 = background) is
	// written into `outLabelImage` so the caller can surface-extract each island.
	// `outLabelImage` is allocated and sized to match `reslicedImage` by this
	// function; pass a default-constructed vtkSmartPointer.
	std::vector<BoneIsland> segmentBoneIslands(
		vtkImageData*                          reslicedImage,
		double                                 threshold,
		const std::vector<std::array<double,3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>&         outLabelImage,
		const std::function<void(int)>&        progressCb = nullptr);

	// -----------------------------------------------------------------------
	// VTK actor builders
	// -----------------------------------------------------------------------

	// Returns a line actor connecting p0 and p1 in world space.
	vtkSmartPointer<vtkActor> makeLineActor(
		const double p0[3], const double p1[3],
		double r, double g, double b, double lineWidth = 2.0);

	// Returns a sphere glyph actor centred at `centre` with the given radius.
	vtkSmartPointer<vtkActor> makeSphereActor(
		const double centre[3], double radius,
		double r, double g, double b);

	// Returns a ring (closed polygon outline) actor.
	// normal    : unit vector perpendicular to the ring plane
	// radius    : ring radius
	// lineWidth : rendered line width in pixels
	vtkSmartPointer<vtkActor> makeRingActor(
		const double centre[3], const double normal[3], double radius,
		double r, double g, double b, double lineWidth = 2.0);

	// Returns a translucent surface actor for one bone island.
	// `labelImage`  : unsigned-char label volume (produced by segmentBoneIslands)
	// `islandLabel` : the integer label value to iso-surface
	// r, g, b       : surface colour
	// opacity       : surface opacity in [0, 1]
	vtkSmartPointer<vtkActor> makeIslandSurfaceActor(
		vtkImageData* labelImage,
		int           islandLabel,
		double        r, double g, double b,
		double        opacity = 0.6);

} // namespace PrototypeHelpers
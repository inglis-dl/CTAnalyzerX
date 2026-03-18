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

#include <QJsonObject>
#include <QString>

#include <functional>

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

} // namespace PrototypeHelpers
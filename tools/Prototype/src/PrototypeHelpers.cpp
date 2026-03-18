#include "PrototypeHelpers.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkLine.h>
#include <vtkMath.h>
#include <vtkPoints.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRegularPolygonSource.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace PrototypeHelpers
{
	// -----------------------------------------------------------------------
	// JSON / sidecar I/O
	// -----------------------------------------------------------------------

	QJsonObject readJsonObjectFileOrThrow(const QString& path)
	{
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly))
			throw std::runtime_error(("Failed to open JSON file: " + path).toStdString());

		const QByteArray data = f.readAll();
		f.close();

		const QJsonDocument doc = QJsonDocument::fromJson(data);
		if (!doc.isObject())
			throw std::runtime_error(("Invalid JSON (expected object): " + path).toStdString());

		return doc.object();
	}

	QString cropPathFromSidecarOrThrow(const QJsonObject& obj)
	{
		const QString cropPath = obj.value(QStringLiteral("crop"))
			.toObject()
			.value(QStringLiteral("outputPath"))
			.toString();

		if (cropPath.isEmpty())
			throw std::runtime_error("Sidecar missing crop.outputPath (crop not completed?).");

		return cropPath;
	}

	double thresholdFromSidecar(const QJsonObject& obj)
	{
		const QJsonValue v = obj.value(QStringLiteral("threshold"))
			.toObject()
			.value(QStringLiteral("value"));

		if (v.isDouble())
		{
			qDebug("Threshold: %f", v.toDouble());
			return v.toDouble();
		}

		qDebug("Threshold: (not present)");
		return std::numeric_limits<double>::quiet_NaN();
	}

	// -----------------------------------------------------------------------
	// Image statistics
	// -----------------------------------------------------------------------

	double computeScalarStdDev(vtkImageData* image)
	{
		if (!image)
			return 1.0;

		const vtkIdType nPoints = image->GetNumberOfPoints();
		if (nPoints <= 0)
			return 1.0;

		double sum   = 0.0;
		double sumSq = 0.0;

		for (vtkIdType i = 0; i < nPoints; ++i)
		{
			const double v = image->GetPointData()->GetScalars()->GetTuple1(i);
			sum   += v;
			sumSq += v * v;
		}

		const double mean     = sum / static_cast<double>(nPoints);
		const double variance = (sumSq / static_cast<double>(nPoints)) - (mean * mean);
		return std::sqrt(std::max(variance, 0.0));
	}

	// -----------------------------------------------------------------------
	// PCA
	// -----------------------------------------------------------------------

	bool computePca(vtkImageData* image, double threshold,
	                PrototypeMainWindow::PcaResult& result,
	                const std::function<void(int)>& progressCb)
	{
		if (!image)
			return false;

		const double* spacing = image->GetSpacing();
		const double* origin  = image->GetOrigin();
		const int*    dims    = image->GetDimensions();
		vtkDataArray* scalars = image->GetPointData()->GetScalars();

		if (!scalars)
			return false;

		// Report 0 % at the start of the first pass.
		if (progressCb) progressCb(0);

		// --- Pass 1: centroid and count ---
		double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
		vtkIdType count = 0;

		for (int k = 0; k < dims[2]; ++k)
		{
			for (int j = 0; j < dims[1]; ++j)
			{
				for (int i = 0; i < dims[0]; ++i)
				{
					const vtkIdType idx = static_cast<vtkIdType>(k) * dims[1] * dims[0]
					                    + static_cast<vtkIdType>(j) * dims[0]
					                    + i;
					if (scalars->GetTuple1(idx) < threshold)
						continue;

					const double wx = origin[0] + i * spacing[0];
					const double wy = origin[1] + j * spacing[1];
					const double wz = origin[2] + k * spacing[2];

					sumX += wx;
					sumY += wy;
					sumZ += wz;
					++count;
				}
			}
		}

		// Pass 1 complete: 40 %
		if (progressCb) progressCb(40);

		if (count < 3)
		{
			qWarning("computePca: fewer than 3 above-threshold voxels (%lld); PCA skipped.",
			         static_cast<long long>(count));
			return false;
		}

		const double n = static_cast<double>(count);
		result.centroid[0] = sumX / n;
		result.centroid[1] = sumY / n;
		result.centroid[2] = sumZ / n;

		// --- Pass 2: 3×3 covariance (upper-triangle, population formula) ---
		double c00 = 0.0, c01 = 0.0, c02 = 0.0;
		double c11 = 0.0, c12 = 0.0;
		double c22 = 0.0;

		// Track bounding box of the binary voxels (world space) for circumsphere radius
		double bbMin[3] = { std::numeric_limits<double>::max(),
		                    std::numeric_limits<double>::max(),
		                    std::numeric_limits<double>::max() };
		double bbMax[3] = { std::numeric_limits<double>::lowest(),
		                    std::numeric_limits<double>::lowest(),
		                    std::numeric_limits<double>::lowest() };

		for (int k = 0; k < dims[2]; ++k)
		{
			for (int j = 0; j < dims[1]; ++j)
			{
				for (int i = 0; i < dims[0]; ++i)
				{
					const vtkIdType idx = static_cast<vtkIdType>(k) * dims[1] * dims[0]
					                    + static_cast<vtkIdType>(j) * dims[0]
					                    + i;
					if (scalars->GetTuple1(idx) < threshold)
						continue;

					const double wx = origin[0] + i * spacing[0] - result.centroid[0];
					const double wy = origin[1] + j * spacing[1] - result.centroid[1];
					const double wz = origin[2] + k * spacing[2] - result.centroid[2];

					c00 += wx * wx;  c01 += wx * wy;  c02 += wx * wz;
					c11 += wy * wy;  c12 += wy * wz;
					c22 += wz * wz;

					// bounding box
					const double awx = wx + result.centroid[0];
					const double awy = wy + result.centroid[1];
					const double awz = wz + result.centroid[2];
					bbMin[0] = std::min(bbMin[0], awx); bbMax[0] = std::max(bbMax[0], awx);
					bbMin[1] = std::min(bbMin[1], awy); bbMax[1] = std::max(bbMax[1], awy);
					bbMin[2] = std::min(bbMin[2], awz); bbMax[2] = std::max(bbMax[2], awz);
				}
			}
		}

		// Pass 2 complete: 80 %
		if (progressCb) progressCb(80);

		c00 /= n; c01 /= n; c02 /= n;
		c11 /= n; c12 /= n;
		c22 /= n;

		// --- Eigen-decomposition via vtkMath::Jacobi ---
		// vtkMath::Jacobi expects a double** (array of row pointers)
		double row0[3] = { c00, c01, c02 };
		double row1[3] = { c01, c11, c12 };
		double row2[3] = { c02, c12, c22 };
		double* cov[3] = { row0, row1, row2 };

		double evecData[3][3]; // columns = eigenvectors after Jacobi
		double* evecs[3] = { evecData[0], evecData[1], evecData[2] };
		double  evals[3];

		vtkMath::Jacobi(cov, evals, evecs);
		// vtkMath::Jacobi returns eigenvalues in DESCENDING order

		// Transpose: Jacobi stores eigenvectors as columns of evecs[][] matrix
		// evecs[col][row], so axis i = (evecs[0][i], evecs[1][i], evecs[2][i])
		for (int i = 0; i < 3; ++i)
		{
			result.axes[i][0] = evecs[0][i];
			result.axes[i][1] = evecs[1][i];
			result.axes[i][2] = evecs[2][i];
			vtkMath::Normalize(result.axes[i]);
			result.eigenvalues[i] = evals[i];
		}

		// Circumsphere radius: half-diagonal of the binary bounding box,
		// centred on the centroid, guaranteed to lie outside the bounding box.
		const double dx = bbMax[0] - bbMin[0];
		const double dy = bbMax[1] - bbMin[1];
		const double dz = bbMax[2] - bbMin[2];
		result.circumRadius = 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz);

		qDebug("PCA centroid: (%.2f, %.2f, %.2f)", result.centroid[0], result.centroid[1], result.centroid[2]);
		qDebug("PCA eigenvalues: %.4f  %.4f  %.4f", evals[0], evals[1], evals[2]);
		qDebug("PCA circumsphere radius: %.2f", result.circumRadius);

		// Eigen-decomposition complete: 100 %
		if (progressCb) progressCb(100);

		result.valid = true;
		return true;
	}

	// -----------------------------------------------------------------------
	// Ray–AABB intersection (slab method)
	// -----------------------------------------------------------------------

	bool rayAabbIntersect(const double rayOrigin[3], const double rayDir[3],
	                      const double bbMin[3],    const double bbMax[3],
	                      double& tEntry, double& tExit)
	{
		double tMin = -std::numeric_limits<double>::max();
		double tMax =  std::numeric_limits<double>::max();

		for (int a = 0; a < 3; ++a)
		{
			const double d = rayDir[a];
			if (std::abs(d) < 1e-12)
			{
				// Ray is parallel to this slab pair; miss if origin is outside
				if (rayOrigin[a] < bbMin[a] || rayOrigin[a] > bbMax[a])
					return false;
			}
			else
			{
				const double invD = 1.0 / d;
				double t0 = (bbMin[a] - rayOrigin[a]) * invD;
				double t1 = (bbMax[a] - rayOrigin[a]) * invD;
				if (t0 > t1) std::swap(t0, t1);
				tMin = std::max(tMin, t0);
				tMax = std::min(tMax, t1);
				if (tMin > tMax)
					return false;
			}
		}

		tEntry = tMin; // may be <= 0 when origin is inside the box
		tExit  = tMax; // distance to the far face along rayDir
		return tExit > 0.0; // ray must actually reach the box
	}

	bool rayAabbExit(const double rayOrigin[3], const double rayDir[3],
	                 const double bbMin[3],    const double bbMax[3],
	                 double& tExit)
	{
		// tFar accumulates the minimum of all far-slab intersections.
		// Initialise to +inf so the first real slab clamps it down.
		double tFar = std::numeric_limits<double>::max();

		for (int a = 0; a < 3; ++a)
		{
			const double d = rayDir[a];

			if (std::abs(d) < 1e-12)
			{
				// Ray is parallel to this slab pair.
				// If the origin is outside the slab the ray never hits the box.
				if (rayOrigin[a] < bbMin[a] || rayOrigin[a] > bbMax[a])
					return false;
				// Otherwise the ray travels within the slab forever — no constraint
				// on tFar from this axis.
			}
			else
			{
				// Compute t for each of the two planes bounding this axis
				const double invD = 1.0 / d;
				const double tA   = (bbMin[a] - rayOrigin[a]) * invD;
				const double tB   = (bbMax[a] - rayOrigin[a]) * invD;

				// tFarAxis is the t of the plane the ray exits through.
				// tNearAxis is the plane it enters through (may be negative when
				// the origin is already past that plane, i.e. inside the box).
				const double tFarAxis = std::max(tA, tB);

				// The overall exit t is the minimum across all axes:
				// the ray exits the box as soon as it leaves any slab.
				tFar = std::min(tFar, tFarAxis);
			}
		}

		// tFar must be positive: the exit point is ahead of the ray origin.
		if (tFar <= 0.0)
			return false;

		tExit = tFar;
		return true;
	}

	// -----------------------------------------------------------------------
	// Surface search
	// -----------------------------------------------------------------------

	void findSurfacePointFromBoundary(vtkImageData* image,
	                                  const double centroid[3],
	                                  const double axisDir[3],
	                                  double threshold,
	                                  double outWorld[3])
	{
		const double* spacing = image->GetSpacing();
		const double* origin  = image->GetOrigin();
		const int*    dims    = image->GetDimensions();
		vtkDataArray* scalars = image->GetPointData()->GetScalars();

		// World-space AABB of the image volume
		const double bbMin[3] = {
			origin[0],
			origin[1],
			origin[2]
		};
		const double bbMax[3] = {
			origin[0] + (dims[0] - 1) * spacing[0],
			origin[1] + (dims[1] - 1) * spacing[1],
			origin[2] + (dims[2] - 1) * spacing[2]
		};

		// Step 1 & 2: find the exit distance from centroid to the far AABB face
		double tExit = 0.0;
		if (!rayAabbExit(centroid, axisDir, bbMin, bbMax, tExit))
		{
			// Centroid is outside the image entirely (degenerate case)
			qWarning("findSurfacePointFromBoundary: ray does not intersect image AABB; "
			         "returning centroid as fallback.");
			outWorld[0] = centroid[0];
			outWorld[1] = centroid[1];
			outWorld[2] = centroid[2];
			return;
		}

		// Step 2 result: the point on the far AABB face in the axisDir direction
		const double startWorld[3] = {
			centroid[0] + tExit * axisDir[0],
			centroid[1] + tExit * axisDir[1],
			centroid[2] + tExit * axisDir[2]
		};

		qDebug("findSurfacePointFromBoundary: tExit=%.3f  start=(%.2f, %.2f, %.2f)",
		       tExit, startWorld[0], startWorld[1], startWorld[2]);

		// Step 3: walk inward along -axisDir (back toward centroid)
		// Step size = smallest voxel spacing so no voxel is ever skipped
		const double step = std::min({ spacing[0], spacing[1], spacing[2] });

		// Maximum distance to walk: tExit (bbox face ? centroid) + one extra step
		// so the centroid voxel itself is always sampled.
		const double maxDist = tExit + step;

		double cur[3] = { startWorld[0], startWorld[1], startWorld[2] };
		double walked = 0.0;

		while (walked <= maxDist)
		{
			// World ? nearest voxel index
			const int ix = static_cast<int>((cur[0] - origin[0]) / spacing[0] + 0.5);
			const int iy = static_cast<int>((cur[1] - origin[1]) / spacing[1] + 0.5);
			const int iz = static_cast<int>((cur[2] - origin[2]) / spacing[2] + 0.5);

			// Only sample voxels that lie within the image extent
			if (ix >= 0 && ix < dims[0] &&
			    iy >= 0 && iy < dims[1] &&
			    iz >= 0 && iz < dims[2])
			{
				const vtkIdType idx = static_cast<vtkIdType>(iz) * dims[1] * dims[0]
				                    + static_cast<vtkIdType>(iy) * dims[0]
				                    + ix;

				// Step 4: first above-threshold voxel is the surface entry point
				if (scalars->GetTuple1(idx) >= threshold)
				{
					outWorld[0] = cur[0];
					outWorld[1] = cur[1];
					outWorld[2] = cur[2];
					return;
				}
			}

			// Advance one step back toward the centroid
			cur[0] -= axisDir[0] * step;
			cur[1] -= axisDir[1] * step;
			cur[2] -= axisDir[2] * step;
			walked += step;
		}

		// No above-threshold voxel found; centroid is the safe fallback
		qWarning("findSurfacePointFromBoundary: no above-threshold voxel found; "
		         "returning centroid as fallback.");
		outWorld[0] = centroid[0];
		outWorld[1] = centroid[1];
		outWorld[2] = centroid[2];
	}

	// -----------------------------------------------------------------------
	// VTK actor builders
	// -----------------------------------------------------------------------

	vtkSmartPointer<vtkActor> makeLineActor(
		const double p0[3], const double p1[3],
		double r, double g, double b, double lineWidth)
	{
		auto pts   = vtkSmartPointer<vtkPoints>::New();
		auto line  = vtkSmartPointer<vtkLine>::New();
		auto cells = vtkSmartPointer<vtkCellArray>::New();
		auto pd    = vtkSmartPointer<vtkPolyData>::New();

		pts->InsertNextPoint(p0);
		pts->InsertNextPoint(p1);
		line->GetPointIds()->SetId(0, 0);
		line->GetPointIds()->SetId(1, 1);
		cells->InsertNextCell(line);

		pd->SetPoints(pts);
		pd->SetLines(cells);

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputData(pd);

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetColor(r, g, b);
		actor->GetProperty()->SetLineWidth(static_cast<float>(lineWidth));
		actor->GetProperty()->SetLighting(false);

		return actor;
	}

	vtkSmartPointer<vtkActor> makeSphereActor(
		const double centre[3], double radius,
		double r, double g, double b)
	{
		auto sphere = vtkSmartPointer<vtkSphereSource>::New();
		sphere->SetCenter(centre);
		sphere->SetRadius(radius);
		sphere->SetPhiResolution(16);
		sphere->SetThetaResolution(16);

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputConnection(sphere->GetOutputPort());

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetColor(r, g, b);

		return actor;
	}

	vtkSmartPointer<vtkActor> makeRingActor(
		const double centre[3], const double normal[3], double radius,
		double r, double g, double b, double lineWidth)
	{
		auto ring = vtkSmartPointer<vtkRegularPolygonSource>::New();
		ring->SetNumberOfSides(64);
		ring->SetRadius(radius);
		ring->SetCenter(centre);
		ring->SetNormal(normal);
		ring->GeneratePolygonOff(); // outline (closed polyline) only, no filled face

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputConnection(ring->GetOutputPort());

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetColor(r, g, b);
		actor->GetProperty()->SetLineWidth(static_cast<float>(lineWidth));
		actor->GetProperty()->SetLighting(false);

		return actor;
	}

} // namespace PrototypeHelpers
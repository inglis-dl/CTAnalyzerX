#include "PrototypeHelpers.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkColorTransferFunction.h>
#include <vtkDataArray.h>
#include <vtkDiscreteFlyingEdges3D.h>
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
#include <vtkScalarBarActor.h>
#include <vtkTextProperty.h>
#include <vtkThreshold.h>
#include <vtkUnsignedCharArray.h>
#include <vtkImageContinuousDilate3D.h>
#include <vtkImageContinuousErode3D.h>
#include <vtkImageGaussianSmooth.h>
#include <vtkImageThresholdConnectivity.h>

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>
#include <stdexcept>
#include <vector>

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

		double sum = 0.0;
		double sumSq = 0.0;

		for (vtkIdType i = 0; i < nPoints; ++i)
		{
			const double v = image->GetPointData()->GetScalars()->GetTuple1(i);
			sum += v;
			sumSq += v * v;
		}

		const double mean = sum / static_cast<double>(nPoints);
		const double variance = (sumSq / static_cast<double>(nPoints)) - (mean * mean);
		return std::sqrt(std::max(variance, 0.0));
	}

	QJsonObject computeScalarThresholdStats(vtkImageData* image, double threshold)
	{
		QJsonObject result;
		if (!image)
			return result;

		const vtkIdType nPoints = image->GetNumberOfPoints();
		if (nPoints <= 0)
			return result;

		double range[2];
		image->GetScalarRange(range);
		result.insert("min", range[0]);
		result.insert("max", range[1]);

		double sum = 0.0;
		double sumSq = 0.0;
		double sumFg = 0.0;
		double sumFgSq = 0.0;
		double sumBg = 0.0;
		double sumBgSq = 0.0;
		double countFg = 0.0;
		double countBg = 0.0;
		for (vtkIdType i = 0; i < nPoints; ++i)
		{
			const double v = image->GetPointData()->GetScalars()->GetTuple1(i);
			if (v >= threshold)
			{
				sumFg += v;
				sumFgSq += v * v;
				countFg += 1.0;
			}
			else if (v < threshold) {
				sumBg += v;
				sumBgSq += v * v;
				countBg += 1.0;
			}
			sum += v;
			sumSq += v * v;
		}

		const double count = static_cast<double>(nPoints);
		const double mean = sum / count;
		const double variance = (sumSq / count) - (mean * mean);
		const double meanFg = (countFg > 0) ? (sumFg / countFg) : 0.0;
		const double varianceFg = (countFg > 0) ? ((sumFgSq / countFg) - (meanFg * meanFg)) : 0.0;
		const double meanBg = (countBg > 0) ? (sumBg / countBg) : 0.0;
		const double varianceBg = (countBg > 0) ? ((sumBgSq / countBg) - (meanBg * meanBg)) : 0.0;

		result.insert("mean", mean);
		result.insert("stdDev", std::sqrt(std::max(variance, 0.0)));
		result.insert("meanFg", meanFg);
		result.insert("stdDevFg", std::sqrt(std::max(varianceFg, 0.0)));
		result.insert("meanBg", meanBg);
		result.insert("stdDevBg", std::sqrt(std::max(varianceBg, 0.0)));

		return result;
	}


	// -----------------------------------------------------------------------
	// PCA
	// -----------------------------------------------------------------------

	bool computePca(vtkImageData* image, double threshold,
					PcaResult& result,
					const std::function<void(int)>& progressCb)
	{
		if (!image)
			return false;

		const double* spacing = image->GetSpacing();
		const double* origin = image->GetOrigin();
		const int* dims = image->GetDimensions();
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

		double row0[3] = { c00, c01, c02 };
		double row1[3] = { c01, c11, c12 };
		double row2[3] = { c02, c12, c22 };
		double* cov[3] = { row0, row1, row2 };

		double evecData[3][3];
		double* evecs[3] = { evecData[0], evecData[1], evecData[2] };
		double  evals[3];

		vtkMath::Jacobi(cov, evals, evecs);

		// Copy eigenvectors from Jacobi output columns into result.axes rows,
		// then normalise and store eigenvalues.
		// vtkMath::Jacobi stores eigenvectors as columns of the evecs matrix,
		// so evecs[row][col] ? result.axes[col][row].
		for (int i = 0; i < 3; ++i)
		{
			result.axes[i][0] = evecs[0][i];
			result.axes[i][1] = evecs[1][i];
			result.axes[i][2] = evecs[2][i];
			vtkMath::Normalize(result.axes[i]);
			result.eigenvalues[i] = evals[i];
		}

		// Fix eigenvector sign convention so orientation is stable across runs.
		// vtkMath::Jacobi returns eigenvectors with arbitrary sign (both +v and
		// -v are valid eigenvectors).  Flip each axis so its largest-magnitude
		// component is positive, giving a consistent "positive dominant" direction
		// that prevents the resliced volume from flipping between calls.
		for (int i = 0; i < 3; ++i)
		{
			// Find the component with the largest absolute value
			int dominantComponent = 0;
			double maxAbs = 0.0;
			for (int d = 0; d < 3; ++d)
			{
				const double a = std::abs(result.axes[i][d]);
				if (a > maxAbs) { maxAbs = a; dominantComponent = d; }
			}

			// Flip the entire axis if its dominant component points negative
			if (result.axes[i][dominantComponent] < 0.0)
			{
				result.axes[i][0] = -result.axes[i][0];
				result.axes[i][1] = -result.axes[i][1];
				result.axes[i][2] = -result.axes[i][2];
			}
		}

		const double dx = bbMax[0] - bbMin[0];
		const double dy = bbMax[1] - bbMin[1];
		const double dz = bbMax[2] - bbMin[2];
		result.circumRadius = 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz);

		qDebug("PCA centroid: (%.2f, %.2f, %.2f)", result.centroid[0], result.centroid[1], result.centroid[2]);
		qDebug("PCA eigenvalues: %.4f  %.4f  %.4f", evals[0], evals[1], evals[2]);
		qDebug("PCA circumsphere radius: %.2f", result.circumRadius);

		if (progressCb) progressCb(100);

		result.valid = true;
		return true;
	}

	// -----------------------------------------------------------------------
	// Ray–AABB intersection (slab method)
	// -----------------------------------------------------------------------

	bool rayAabbIntersect(const double rayOrigin[3], const double rayDir[3],
						  const double bbMin[3], const double bbMax[3],
						  double& tEntry, double& tExit)
	{
		double tMin = -std::numeric_limits<double>::max();
		double tMax = std::numeric_limits<double>::max();

		for (int a = 0; a < 3; ++a)
		{
			const double d = rayDir[a];
			if (std::abs(d) < 1e-12)
			{
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

		tEntry = tMin;
		tExit = tMax;
		return tExit > 0.0;
	}

	bool rayAabbExit(const double rayOrigin[3], const double rayDir[3],
					 const double bbMin[3], const double bbMax[3],
					 double& tExit)
	{
		double tFar = std::numeric_limits<double>::max();

		for (int a = 0; a < 3; ++a)
		{
			const double d = rayDir[a];

			if (std::abs(d) < 1e-12)
			{
				if (rayOrigin[a] < bbMin[a] || rayOrigin[a] > bbMax[a])
					return false;
			}
			else
			{
				const double invD = 1.0 / d;
				const double tA = (bbMin[a] - rayOrigin[a]) * invD;
				const double tB = (bbMax[a] - rayOrigin[a]) * invD;
				const double tFarAxis = std::max(tA, tB);
				tFar = std::min(tFar, tFarAxis);
			}
		}

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
		const double* origin = image->GetOrigin();
		const int* dims = image->GetDimensions();
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
	// Bone island segmentation — VTK-native seeded BFS region growing
	// -----------------------------------------------------------------------

	std::vector<BoneIsland> segmentBoneIslands(
		vtkImageData* reslicedImage,
		double                                 threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outLabelImage,
		const std::function<void(int)>& progressCb)
	{
		if (!reslicedImage || seedsWorld.empty())
			return {};

		const double* origin = reslicedImage->GetOrigin();
		const double* spacing = reslicedImage->GetSpacing();
		const int* dims = reslicedImage->GetDimensions();
		vtkDataArray* scalars = reslicedImage->GetPointData()->GetScalars();

		if (!scalars)
		{
			qWarning("segmentBoneIslands: resliced image has no scalar data.");
			return {};
		}

		if (progressCb) progressCb(0);

		const vtkIdType nx = dims[0];
		const vtkIdType ny = dims[1];
		const vtkIdType nz = dims[2];
		const vtkIdType totalVoxels = nx * ny * nz;

		// ------------------------------------------------------------------
		// Build a flat binary mask: 1 = above threshold, 0 = background.
		// Using a separate array avoids modifying the original scalar data.
		// ------------------------------------------------------------------
		std::vector<unsigned char> binary(static_cast<std::size_t>(totalVoxels), 0u);

		for (vtkIdType i = 0; i < totalVoxels; ++i)
		{
			if (scalars->GetTuple1(i) >= threshold)
				binary[static_cast<std::size_t>(i)] = 1u;
		}

		if (progressCb) progressCb(10);

		// ------------------------------------------------------------------
		// Label map: 0 = unvisited / background, label > 0 = island id.
		// Sized identical to the input volume.
		// ------------------------------------------------------------------
		std::vector<unsigned char> labelMap(static_cast<std::size_t>(totalVoxels), 0u);

		// Helper: flat voxel index from 3-D coordinates
		auto flatIdx = [&](vtkIdType x, vtkIdType y, vtkIdType z) -> vtkIdType
			{
				return z * ny * nx + y * nx + x;
			};

		// Helper: world ? nearest voxel index via VTK physical?continuous-index transform.
		// Returns false when the resulting index falls outside the image extent.
		auto worldToVoxel = [&](const double w[3], int out[3]) -> bool
			{
				double cont[3] = { 0.0, 0.0, 0.0 };
				reslicedImage->TransformPhysicalPointToContinuousIndex(w, cont);
				out[0] = static_cast<int>(std::lround(cont[0]));
				out[1] = static_cast<int>(std::lround(cont[1]));
				out[2] = static_cast<int>(std::lround(cont[2]));
				return (out[0] >= 0 && out[0] < dims[0] &&
						out[1] >= 0 && out[1] < dims[1] &&
						out[2] >= 0 && out[2] < dims[2]);
			};

		// 26-connected neighbourhood offsets (all combinations of ±1 per axis)
		const int offsets[26][3] = {
			{-1,-1,-1},{-1,-1, 0},{-1,-1, 1},
			{-1, 0,-1},{-1, 0, 0},{-1, 0, 1},
			{-1, 1,-1},{-1, 1, 0},{-1, 1, 1},
			{ 0,-1,-1},{ 0,-1, 0},{ 0,-1, 1},
			{ 0, 0,-1},           { 0, 0, 1},
			{ 0, 1,-1},{ 0, 1, 0},{ 0, 1, 1},
			{ 1,-1,-1},{ 1,-1, 0},{ 1,-1, 1},
			{ 1, 0,-1},{ 1, 0, 0},{ 1, 0, 1},
			{ 1, 1,-1},{ 1, 1, 0},{ 1, 1, 1},
		};

		// ------------------------------------------------------------------
		// BFS flood-fill from each seed, one island per seed
		// ------------------------------------------------------------------
		std::vector<BoneIsland> islands;
		islands.reserve(seedsWorld.size());

		const int nSeeds = static_cast<int>(seedsWorld.size());

		for (int s = 0; s < nSeeds; ++s)
		{
			const auto& sw = seedsWorld[static_cast<std::size_t>(s)];
			const double seedW[3] = { sw[0], sw[1], sw[2] };

			int seedVox[3];
			if (!worldToVoxel(seedW, seedVox))
			{
				qWarning("segmentBoneIslands: seed %d (%.2f, %.2f, %.2f) is outside the image extent; skipped.",
						 s, seedW[0], seedW[1], seedW[2]);
				continue;
			}

			const vtkIdType seedFlat = flatIdx(seedVox[0], seedVox[1], seedVox[2]);

			// Skip if the seed voxel is below threshold
			if (binary[static_cast<std::size_t>(seedFlat)] == 0u)
			{
				qWarning("segmentBoneIslands: seed %d (%.2f, %.2f, %.2f) ? voxel (%d,%d,%d) "
						 "is below threshold; skipped.",
						 s, seedW[0], seedW[1], seedW[2],
						 seedVox[0], seedVox[1], seedVox[2]);
				continue;
			}

			// Skip if already claimed by a previous seed
			if (labelMap[static_cast<std::size_t>(seedFlat)] != 0u)
			{
				qWarning("segmentBoneIslands: seed %d voxel (%d,%d,%d) was already labelled %u; "
						 "island would be empty — skipped.",
						 s, seedVox[0], seedVox[1], seedVox[2],
						 static_cast<unsigned>(labelMap[static_cast<std::size_t>(seedFlat)]));
				continue;
			}

			const unsigned char islandLabel = static_cast<unsigned char>(islands.size() + 1u);

			// BFS
			std::queue<std::array<vtkIdType, 3>> bfsQueue;
			bfsQueue.push({ seedVox[0], seedVox[1], seedVox[2] });
			labelMap[static_cast<std::size_t>(seedFlat)] = islandLabel;
			vtkIdType voxelCount = 0;

			// Track the axis-aligned bounding box of this island (voxel indices)
			int bbVoxMin[3] = { seedVox[0], seedVox[1], seedVox[2] };
			int bbVoxMax[3] = { seedVox[0], seedVox[1], seedVox[2] };

			while (!bfsQueue.empty())
			{
				const auto cur = bfsQueue.front();
				bfsQueue.pop();

				const vtkIdType cx = cur[0];
				const vtkIdType cy = cur[1];
				const vtkIdType cz = cur[2];
				++voxelCount;

				bbVoxMin[0] = std::min(bbVoxMin[0], static_cast<int>(cx));
				bbVoxMin[1] = std::min(bbVoxMin[1], static_cast<int>(cy));
				bbVoxMin[2] = std::min(bbVoxMin[2], static_cast<int>(cz));
				bbVoxMax[0] = std::max(bbVoxMax[0], static_cast<int>(cx));
				bbVoxMax[1] = std::max(bbVoxMax[1], static_cast<int>(cy));
				bbVoxMax[2] = std::max(bbVoxMax[2], static_cast<int>(cz));

				for (const auto& off : offsets)
				{
					const vtkIdType nx_ = cx + off[0];
					const vtkIdType ny_ = cy + off[1];
					const vtkIdType nz_ = cz + off[2];

					if (nx_ < 0 || nx_ >= nx ||
						ny_ < 0 || ny_ >= ny ||
						nz_ < 0 || nz_ >= nz)
						continue;

					const vtkIdType nFlat = flatIdx(nx_, ny_, nz_);
					const auto nFlatSz = static_cast<std::size_t>(nFlat);

					if (binary[nFlatSz] == 0u || labelMap[nFlatSz] != 0u)
						continue;

					labelMap[nFlatSz] = islandLabel;
					bfsQueue.push({ nx_, ny_, nz_ });
				}
			}

			qDebug("segmentBoneIslands: seed %d ? island label %u, %lld voxels, "
				   "BB voxel [%d,%d,%d]–[%d,%d,%d]",
				   s, static_cast<unsigned>(islandLabel),
				   static_cast<long long>(voxelCount),
				   bbVoxMin[0], bbVoxMin[1], bbVoxMin[2],
				   bbVoxMax[0], bbVoxMax[1], bbVoxMax[2]);

			// Build world-space bounding box for the JSON summary
			// Use VTK's index?physical transform so direction cosines are respected.
			double bbIdxMin[3] = {
				static_cast<double>(bbVoxMin[0]),
				static_cast<double>(bbVoxMin[1]),
				static_cast<double>(bbVoxMin[2])
			};
			double bbIdxMax[3] = {
				static_cast<double>(bbVoxMax[0]),
				static_cast<double>(bbVoxMax[1]),
				static_cast<double>(bbVoxMax[2])
			};
			double bbWorldMin[3] = { 0.0, 0.0, 0.0 };
			double bbWorldMax[3] = { 0.0, 0.0, 0.0 };
			reslicedImage->TransformContinuousIndexToPhysicalPoint(bbIdxMin, bbWorldMin);
			reslicedImage->TransformContinuousIndexToPhysicalPoint(bbIdxMax, bbWorldMax);

			// Serialise island summary to JSON
			auto packVec3 = [](const double v[3]) -> QJsonArray
				{
					return QJsonArray{ v[0], v[1], v[2] };
				};

			QJsonObject islandJson;
			islandJson[QStringLiteral("label")] = static_cast<int>(islandLabel);
			islandJson[QStringLiteral("voxelCount")] = static_cast<qint64>(voxelCount);
			islandJson[QStringLiteral("seedWorld")] = packVec3(seedW);
			islandJson[QStringLiteral("bbMin")] = packVec3(bbWorldMin);
			islandJson[QStringLiteral("bbMax")] = packVec3(bbWorldMax);

			BoneIsland island;
			island.label = static_cast<int>(islandLabel);
			island.voxelCount = voxelCount;
			island.seedWorld[0] = seedW[0];
			island.seedWorld[1] = seedW[1];
			island.seedWorld[2] = seedW[2];
			island.seedVoxel[0] = seedVox[0];
			island.seedVoxel[1] = seedVox[1];
			island.seedVoxel[2] = seedVox[2];
			island.json = islandJson;

			islands.push_back(island);

			// Update progress: spread seeds across [10, 90]
			if (progressCb)
			{
				const int pct = 10 + static_cast<int>(
					80.0 * (s + 1) / static_cast<double>(nSeeds));
				progressCb(pct);
			}
		}

		// ------------------------------------------------------------------
		// Pack the label map into a new vtkImageData (unsigned char scalars)
		// with the same geometry as the resliced input.
		// ------------------------------------------------------------------
		outLabelImage = vtkSmartPointer<vtkImageData>::New();
		outLabelImage->SetDimensions(dims);
		outLabelImage->SetSpacing(spacing);
		outLabelImage->SetOrigin(origin);
		outLabelImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

		unsigned char* outPtr = static_cast<unsigned char*>(
			outLabelImage->GetScalarPointer());

		for (std::size_t i = 0; i < static_cast<std::size_t>(totalVoxels); ++i)
			outPtr[i] = labelMap[i];

		if (progressCb) progressCb(100);

		return islands;
	}

	// -----------------------------------------------------------------------
	// VTK actor builders
	// -----------------------------------------------------------------------

	vtkSmartPointer<vtkActor> makeLineActor(
		const double p0[3], const double p1[3],
		double r, double g, double b, double lineWidth)
	{
		auto pts = vtkSmartPointer<vtkPoints>::New();
		auto line = vtkSmartPointer<vtkLine>::New();
		auto cells = vtkSmartPointer<vtkCellArray>::New();
		auto pd = vtkSmartPointer<vtkPolyData>::New();

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
		ring->GeneratePolygonOff();

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputConnection(ring->GetOutputPort());

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetColor(r, g, b);
		actor->GetProperty()->SetLineWidth(static_cast<float>(lineWidth));
		actor->GetProperty()->SetLighting(false);

		return actor;
	}

	vtkSmartPointer<vtkActor> makeIslandSurfaceActor(
		vtkImageData* labelImage,
		int           islandLabel,
		double        r, double g, double b,
		double        opacity)
	{
		// vtkDiscreteFlyingEdges3D iso-surfaces exactly on an integer label value.
		// It is available in VTK 8+ and is faster than vtkMarchingCubes for
		// discrete label volumes.
		auto flyingEdges = vtkSmartPointer<vtkDiscreteFlyingEdges3D>::New();
		flyingEdges->SetInputData(labelImage);
		flyingEdges->SetValue(0, static_cast<double>(islandLabel));
		flyingEdges->Update();

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputConnection(flyingEdges->GetOutputPort());
		mapper->ScalarVisibilityOff(); // use actor colour, not scalar colour map

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetColor(r, g, b);
		actor->GetProperty()->SetOpacity(opacity);
		// Back-face culling keeps the inside of partially-visible islands clean
		actor->GetProperty()->BackfaceCullingOn();

		return actor;
	}

	// -----------------------------------------------------------------------
	// Colour transfer function / scalar bar actor for island labels
	// -----------------------------------------------------------------------

	vtkSmartPointer<vtkColorTransferFunction> makeIslandColorTF(
		double minVal, double maxVal)
	{
		auto tf = vtkSmartPointer<vtkColorTransferFunction>::New();
		tf->SetColorSpaceToRGB();
		tf->AllowDuplicateScalarsOn();

		// Guard against a degenerate range (only one island or all equal sizes).
		if (maxVal <= minVal)
			maxVal = minVal + 1.0;

		const double mid = 0.5 * (minVal + maxVal);

		// High-luminance pastel palette — bright enough to contrast against the
		// VolumeView black renderer background (default VTK rendererColour).
		// min  ? vivid yellow  (smallest island)
		// mid  ? vivid cyan    (mid-range island)
		// max  ? vivid magenta (largest island)
		// All three anchors have luminance > 0.7, so surfaces remain clearly
		// visible even at the default opacity of 0.55.
		tf->AddRGBPoint(minVal, 1.00, 0.95, 0.20); // vivid yellow
		tf->AddRGBPoint(mid, 0.20, 0.95, 0.95); // vivid cyan
		tf->AddRGBPoint(maxVal, 1.00, 0.30, 0.95); // vivid magenta
		tf->Build();

		return tf;
	}

	vtkSmartPointer<vtkScalarBarActor> makeIslandScalarBar(
		vtkColorTransferFunction* colorTF,
		vtkIdType                 minVoxels,
		vtkIdType                 maxVoxels)
	{
		auto bar = vtkSmartPointer<vtkScalarBarActor>::New();

		bar->SetLookupTable(colorTF);
		bar->SetTitle("Voxels");
		bar->SetNumberOfLabels(5);

		// Position: upper-right corner, narrow strip so it does not obstruct the volume.
		// Normalised viewport coordinates [x0, y0] width x height.
		bar->SetPosition(0.88, 0.55);
		bar->SetWidth(0.10);
		bar->SetHeight(0.40);

		// Orientation
		bar->SetOrientationToVertical();

		// Label format: integer voxel counts
		bar->SetLabelFormat("%.0f");

		// Title text styling
		vtkTextProperty* titleProp = bar->GetTitleTextProperty();
		titleProp->SetFontSize(11);
		titleProp->SetBold(1);
		titleProp->SetItalic(0);
		titleProp->SetColor(1.0, 1.0, 1.0);

		// Label text styling
		vtkTextProperty* labelProp = bar->GetLabelTextProperty();
		labelProp->SetFontSize(9);
		labelProp->SetBold(0);
		labelProp->SetItalic(0);
		labelProp->SetColor(1.0, 1.0, 1.0);

		// Range annotation: these appear as text ticks at the bar ends.
		char loLabel[32];
		char hiLabel[32];
		std::snprintf(loLabel, sizeof(loLabel), "%lld", static_cast<long long>(minVoxels));
		std::snprintf(hiLabel, sizeof(hiLabel), "%lld", static_cast<long long>(maxVoxels));
		bar->GetLookupTable()->SetAnnotation(
			static_cast<double>(minVoxels), loLabel);
		bar->GetLookupTable()->SetAnnotation(
			static_cast<double>(maxVoxels), hiLabel);

		// Draw a thin border frame around the bar for visual clarity on bright backgrounds.
		bar->DrawFrameOn();

		return bar;
	}

	// -----------------------------------------------------------------------
	// Bone island segmentation — VTK morphological pipeline (alternate)
	// -----------------------------------------------------------------------

	std::vector<BoneIsland> segmentBoneIslandsAlternate(
		vtkImageData* reslicedImage,
		double                                   threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outLabelImage,
		double                                   smoothStdDev,
		int                                      morphKernelSize,
		const std::function<void(int)>& progressCb)
	{
		if (!reslicedImage || seedsWorld.empty())
			return {};

		if (!reslicedImage->GetPointData()->GetScalars())
		{
			qWarning("segmentBoneIslandsAlternate: resliced image has no scalar data.");
			return {};
		}

		if (progressCb) progressCb(0);

		// ------------------------------------------------------------------
		// Step 1: Gaussian smooth — reduce noise so the morphological
		//         operators work on clean foreground/background boundaries.
		// ------------------------------------------------------------------
		auto smoother = vtkSmartPointer<vtkImageGaussianSmooth>::New();
		smoother->SetInputData(reslicedImage);
		smoother->SetStandardDeviation(smoothStdDev);
		// Radius factor 2 keeps the kernel tight (±2? covers ~95 % of the Gaussian).
		smoother->SetRadiusFactor(2.0);
		smoother->Update();

		qDebug("segmentBoneIslandsAlternate: Gaussian smooth done (stdDev=%.2f).", smoothStdDev);
		if (progressCb) progressCb(10);

		// ------------------------------------------------------------------
		// Step 2: Erosion — disconnect thin bridges between closely touching
		//         bone islands by shrinking all foreground regions inward.
		// ------------------------------------------------------------------
		auto erode = vtkSmartPointer<vtkImageContinuousErode3D>::New();
		erode->SetInputConnection(smoother->GetOutputPort());
		erode->SetKernelSize(
			2 * morphKernelSize + 1,
			2 * morphKernelSize + 1,
			2 * morphKernelSize + 1);
		erode->Update();

		qDebug("segmentBoneIslandsAlternate: erosion done (kernel=%d).",
			   2 * morphKernelSize + 1);
		if (progressCb) progressCb(25);

		// ------------------------------------------------------------------
		// Step 3: Dilation — restore the eroded island cores back toward
		//         their original size without re-bridging the gaps.
		// ------------------------------------------------------------------
		auto dilate = vtkSmartPointer<vtkImageContinuousDilate3D>::New();
		dilate->SetInputConnection(erode->GetOutputPort());
		dilate->SetKernelSize(
			2 * morphKernelSize + 1,
			2 * morphKernelSize + 1,
			2 * morphKernelSize + 1);
		dilate->Update();

		qDebug("segmentBoneIslandsAlternate: dilation done (kernel=%d).",
			   2 * morphKernelSize + 1);
		if (progressCb) progressCb(40);

		// ------------------------------------------------------------------
		// Step 4: Seeded connected-threshold fill via
		//         vtkImageThresholdConnectivity.
		// One pass per seed: each accepted seed produces one island label.
		// We run the filter independently for each seed and accumulate a
		// hand-built label map so the output format matches segmentBoneIslands.
		// ------------------------------------------------------------------
		vtkImageData* morphOutput = dilate->GetOutput();

		const double* origin = reslicedImage->GetOrigin();   // keep original geometry
		const double* spacing = reslicedImage->GetSpacing();
		const int* dims = reslicedImage->GetDimensions();

		const vtkIdType nx = dims[0];
		const vtkIdType ny = dims[1];
		const vtkIdType nz = dims[2];
		const vtkIdType totalVoxels = nx * ny * nz;

		// Label map built incrementally; 0 = background / unclaimed.
		std::vector<unsigned char> labelMap(static_cast<std::size_t>(totalVoxels), 0u);

		// Helper: flat index
		auto flatIdx = [&](vtkIdType x, vtkIdType y, vtkIdType z) -> vtkIdType
			{
				return z * ny * nx + y * nx + x;
			};

		// Helper: world ? nearest voxel (returns false when out of extent)
		auto worldToVoxel = [&](const double w[3], int out[3]) -> bool
			{
				double cont[3] = { 0.0, 0.0, 0.0 };
				reslicedImage->TransformPhysicalPointToContinuousIndex(w, cont);
				out[0] = static_cast<int>(std::lround(cont[0]));
				out[1] = static_cast<int>(std::lround(cont[1]));
				out[2] = static_cast<int>(std::lround(cont[2]));
				return (out[0] >= 0 && out[0] < dims[0] &&
						out[1] >= 0 && out[1] < dims[1] &&
						out[2] >= 0 && out[2] < dims[2]);
			};

		std::vector<BoneIsland> islands;
		islands.reserve(seedsWorld.size());

		const int nSeeds = static_cast<int>(seedsWorld.size());

		for (int s = 0; s < nSeeds; ++s)
		{
			const auto& sw = seedsWorld[static_cast<std::size_t>(s)];
			const double seedW[3] = { sw[0], sw[1], sw[2] };

			int seedVox[3];
			if (!worldToVoxel(seedW, seedVox))
			{
				qWarning("segmentBoneIslandsAlternate: seed %d (%.2f, %.2f, %.2f) "
						 "is outside the image extent; skipped.",
						 s, seedW[0], seedW[1], seedW[2]);
				continue;
			}

			// Skip if a previous seed already claimed this voxel.
			if (labelMap[static_cast<std::size_t>(flatIdx(seedVox[0], seedVox[1], seedVox[2]))] != 0u)
			{
				qWarning("segmentBoneIslandsAlternate: seed %d voxel (%d,%d,%d) "
						 "already labelled; skipped.",
						 s, seedVox[0], seedVox[1], seedVox[2]);
				continue;
			}

			// ----------------------------------------------------------
			// Run vtkImageThresholdConnectivity on the morphology output.
			// The filter flood-fills connected voxels whose scalar value
			// lies in [threshold, +inf) starting from the supplied seed.
			// Background (out-of-range) voxels are set to 0; in-range
			// connected voxels are set to 1 (ReplaceIn = 1, InValue = 1).
			// ----------------------------------------------------------
			auto conn = vtkSmartPointer<vtkImageThresholdConnectivity>::New();
			conn->SetInputData(morphOutput);
			conn->ThresholdByUpper(threshold);
			conn->SetInValue(1.0);
			conn->SetOutValue(0.0);
			conn->ReplaceInOn();
			conn->ReplaceOutOn();
			// VTK 8+: AddSeed is not available; use AddSeed(int*) instead.
			int seed[3] = { seedVox[0], seedVox[1], seedVox[2] };
			// conn->AddSeed(seed);
			// SetSeedPoints takes world-space (physical) coordinates; the filter
			// maps them to the nearest voxel internally — no index conversion needed.
			auto seedPoints = vtkSmartPointer<vtkPoints>::New();
			seedPoints->InsertNextPoint(seedW[0], seedW[1], seedW[2]);
			conn->SetSeedPoints(seedPoints);
			conn->Update();

			vtkImageData* connOut = conn->GetOutput();
			vtkDataArray* connScalars = connOut->GetPointData()->GetScalars();

			if (!connScalars)
			{
				qWarning("segmentBoneIslandsAlternate: seed %d — connectivity filter "
						 "produced no scalars; skipped.", s);
				continue;
			}

			// Check that the seed voxel was actually reached (above threshold).
			const vtkIdType seedFlat = flatIdx(seedVox[0], seedVox[1], seedVox[2]);
			if (connScalars->GetTuple1(seedFlat) < 0.5)
			{
				qWarning("segmentBoneIslandsAlternate: seed %d (%.2f, %.2f, %.2f) "
						 "? voxel (%d,%d,%d) is below threshold after morphology; skipped.",
						 s, seedW[0], seedW[1], seedW[2],
						 seedVox[0], seedVox[1], seedVox[2]);
				continue;
			}

			const unsigned char islandLabel = static_cast<unsigned char>(islands.size() + 1u);

			// Merge the binary connectivity mask into our label map,
			// skipping voxels already claimed by a previous island.
			vtkIdType voxelCount = 0;

			int bbVoxMin[3] = { seedVox[0], seedVox[1], seedVox[2] };
			int bbVoxMax[3] = { seedVox[0], seedVox[1], seedVox[2] };

			for (int k = 0; k < dims[2]; ++k)
			{
				for (int j = 0; j < dims[1]; ++j)
				{
					for (int i = 0; i < dims[0]; ++i)
					{
						const vtkIdType idx = flatIdx(i, j, k);
						const auto      idxSz = static_cast<std::size_t>(idx);

						// Only accept voxels inside this seed's connected region
						// that have not been taken by an earlier seed.
						if (connScalars->GetTuple1(idx) < 0.5)
							continue;
						if (labelMap[idxSz] != 0u)
							continue;

						labelMap[idxSz] = islandLabel;
						++voxelCount;

						bbVoxMin[0] = std::min(bbVoxMin[0], i);
						bbVoxMin[1] = std::min(bbVoxMin[1], j);
						bbVoxMin[2] = std::min(bbVoxMin[2], k);
						bbVoxMax[0] = std::max(bbVoxMax[0], i);
						bbVoxMax[1] = std::max(bbVoxMax[1], j);
						bbVoxMax[2] = std::max(bbVoxMax[2], k);
					}
				}
			}

			qDebug("segmentBoneIslandsAlternate: seed %d ? island label %u, %lld voxels, "
				   "BB voxel [%d,%d,%d]–[%d,%d,%d]",
				   s, static_cast<unsigned>(islandLabel),
				   static_cast<long long>(voxelCount),
				   bbVoxMin[0], bbVoxMin[1], bbVoxMin[2],
				   bbVoxMax[0], bbVoxMax[1], bbVoxMax[2]);

			// Build world-space bounding box for the JSON summary.
			// Use VTK's index?physical transform so direction cosines are respected.
			double bbIdxMin[3] = {
				static_cast<double>(bbVoxMin[0]),
				static_cast<double>(bbVoxMin[1]),
				static_cast<double>(bbVoxMin[2])
			};
			double bbIdxMax[3] = {
				static_cast<double>(bbVoxMax[0]),
				static_cast<double>(bbVoxMax[1]),
				static_cast<double>(bbVoxMax[2])
			};
			double bbWorldMin[3] = { 0.0, 0.0, 0.0 };
			double bbWorldMax[3] = { 0.0, 0.0, 0.0 };
			reslicedImage->TransformContinuousIndexToPhysicalPoint(bbIdxMin, bbWorldMin);
			reslicedImage->TransformContinuousIndexToPhysicalPoint(bbIdxMax, bbWorldMax);

			auto packVec3 = [](const double v[3]) -> QJsonArray
				{
					return QJsonArray{ v[0], v[1], v[2] };
				};

			QJsonObject islandJson;
			islandJson[QStringLiteral("label")] = static_cast<int>(islandLabel);
			islandJson[QStringLiteral("voxelCount")] = static_cast<qint64>(voxelCount);
			islandJson[QStringLiteral("seedWorld")] = packVec3(seedW);
			islandJson[QStringLiteral("bbMin")] = packVec3(bbWorldMin);
			islandJson[QStringLiteral("bbMax")] = packVec3(bbWorldMax);

			BoneIsland island;
			island.label = static_cast<int>(islandLabel);
			island.voxelCount = voxelCount;
			island.seedWorld[0] = seedW[0];
			island.seedWorld[1] = seedW[1];
			island.seedWorld[2] = seedW[2];
			island.seedVoxel[0] = seedVox[0];
			island.seedVoxel[1] = seedVox[1];
			island.seedVoxel[2] = seedVox[2];
			island.json = islandJson;

			islands.push_back(island);

			// Progress: spread seeds across [40, 90]
			if (progressCb)
			{
				const int pct = 40 + static_cast<int>(
					50.0 * (s + 1) / static_cast<double>(nSeeds));
				progressCb(pct);
			}
		}

		// ------------------------------------------------------------------
		// Pack the label map into a vtkImageData with the original geometry.
		// ------------------------------------------------------------------
		outLabelImage = vtkSmartPointer<vtkImageData>::New();
		outLabelImage->SetDimensions(dims);
		outLabelImage->SetSpacing(spacing);
		outLabelImage->SetOrigin(origin);
		outLabelImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

		unsigned char* outPtr = static_cast<unsigned char*>(
			outLabelImage->GetScalarPointer());

		for (std::size_t i = 0; i < static_cast<std::size_t>(totalVoxels); ++i)
			outPtr[i] = labelMap[i];

		if (progressCb) progressCb(100);

		return islands;
	}

} // namespace PrototypeHelpers
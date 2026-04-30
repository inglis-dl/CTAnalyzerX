#include "PrototypeHelpers.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkColorTransferFunction.h>
#include <vtkDataArray.h>
#include <vtkDiscreteFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkImageShiftScale.h>
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

// ITK headers required by segmentBoneIslandsGraphCut
#include <itkImage.h>
#include <itkVTKImageToImageFilter.h>
#include <itkImageToVTKImageFilter.h>
#include <itkBinaryThresholdImageFilter.h>
#include <itkConnectedComponentImageFilter.h>
#include <itkRelabelComponentImageFilter.h>

#include "GraphCut.h" // selects GridCut or Kolmogorov backend

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>
#include <stdexcept>
#include <vector>
#include <atomic>

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

		// --- Pass 2: 3x3 covariance (upper-triangle, population formula) ---
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
	// Ray-AABB intersection (slab method)
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
	// Bone island segmentation - VTK-native seeded BFS region growing
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

		// 26-connected neighbourhood offsets (all combinations of +/-1 per axis)
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
						 "island would be empty - skipped.",
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
				   "BB voxel [%d,%d,%d]-[%d,%d,%d]",
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
// Bone island segmentation — parallel (QtConcurrent)
//
// Identical output contract to segmentBoneIslands().  Three parallel
// passes replace the three serial O(N) loops, and the per-seed BFS tasks
// run concurrently.  A union-find merge pass after the BFS collapses any
// seeds that land on the same physical bone into a single BoneIsland,
// preserving the same island count and label semantics as the serial version.
//
// Pass breakdown
// ──────────────
//  1. Binary mask        QtConcurrent::blockingMap   O(N) fully data-parallel
//  2. Per-seed BFS       QtConcurrent::run           one task per seed,
//                                                    QAtomicInteger CAS ownership
//  3. Union-find merge   serial O(N) adjacency scan  collapses same-bone seeds
//  4. Output pack        QtConcurrent::blockingMap   O(N) fully data-parallel
// -----------------------------------------------------------------------
	std::vector<BoneIsland> segmentBoneIslandsParallel(
		vtkImageData* reslicedImage,
		double                                     threshold,
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
			qWarning("segmentBoneIslandsParallel: resliced image has no scalar data.");
			return {};
		}

		if (progressCb) progressCb(0);

		const vtkIdType NX = dims[0];
		const vtkIdType NY = dims[1];
		const vtkIdType NZ = dims[2];
		const vtkIdType totalVoxels = NX * NY * NZ;
		const int       nVox = static_cast<int>(totalVoxels);

		// Flat-index helper — captured by value into every lambda so that
		// NX/NY are not referenced through a dangling stack frame once tasks
		// are running on worker threads.
		const auto flatIdx =
			[NX, NY](vtkIdType x, vtkIdType y, vtkIdType z) -> vtkIdType
			{
				return z * NY * NX + y * NX + x;
			};

		// 26-connected neighbourhood offsets
		constexpr int offsets[26][3] = {
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

		// ── 1. Parallel binary mask ───────────────────────────────────────
		// GetTuple1() is a pure indexed read on VTK's contiguous scalar buffer
		// and is safe to call concurrently from multiple threads.
		std::vector<quint8> binary(static_cast<std::size_t>(nVox), 0u);

		{
			QVector<int> idx(nVox);
			std::iota(idx.begin(), idx.end(), 0);

			QtConcurrent::blockingMap(idx,
				[&](int i)
				{
					if (scalars->GetTuple1(i) >= threshold)
						binary[static_cast<std::size_t>(i)] = 1u;
				});
		}

		if (progressCb) progressCb(10);

		// ── 2. Seed setup — must run on calling thread ────────────────────
		// TransformPhysicalPointToContinuousIndex is not thread-safe; resolve
		// all seed voxel coordinates here before any async tasks are launched.
		struct SeedSetup
		{
			int    voxel[3] = { 0, 0, 0 };
			double world[3] = { 0.0, 0.0, 0.0 };
			quint8 label = 0u;   // 1-based unique label per valid seed
			bool   valid = false;
		};

		const int nSeeds = static_cast<int>(seedsWorld.size());
		std::vector<SeedSetup> setups(static_cast<std::size_t>(nSeeds));

		quint8 nextLabel = 1u;
		for (int s = 0; s < nSeeds; ++s)
		{
			SeedSetup& ss = setups[static_cast<std::size_t>(s)];
			ss.world[0] = seedsWorld[static_cast<std::size_t>(s)][0];
			ss.world[1] = seedsWorld[static_cast<std::size_t>(s)][1];
			ss.world[2] = seedsWorld[static_cast<std::size_t>(s)][2];

			double cont[3] = {};
			reslicedImage->TransformPhysicalPointToContinuousIndex(ss.world, cont);
			ss.voxel[0] = static_cast<int>(std::lround(cont[0]));
			ss.voxel[1] = static_cast<int>(std::lround(cont[1]));
			ss.voxel[2] = static_cast<int>(std::lround(cont[2]));

			if (ss.voxel[0] < 0 || ss.voxel[0] >= dims[0] ||
				ss.voxel[1] < 0 || ss.voxel[1] >= dims[1] ||
				ss.voxel[2] < 0 || ss.voxel[2] >= dims[2])
			{
				qWarning("segmentBoneIslandsParallel: seed %d is outside image extent; skipped.", s);
				continue;
			}

			const vtkIdType sf = flatIdx(ss.voxel[0], ss.voxel[1], ss.voxel[2]);
			if (binary[static_cast<std::size_t>(sf)] == 0u)
			{
				qWarning("segmentBoneIslandsParallel: seed %d voxel (%d,%d,%d) "
					"is below threshold; skipped.",
					s, ss.voxel[0], ss.voxel[1], ss.voxel[2]);
				continue;
			}

			ss.label = nextLabel++;
			ss.valid = true;
		}

		// ── 3. Atomic label map ───────────────────────────────────────────
		// QAtomicInteger<quint8> default-constructs to 0.
		// testAndSetOrdered(0, label) is the Qt equivalent of
		// compare_exchange_strong: returns true and writes label only when the
		// current value is 0 (voxel unclaimed).
		auto labelMap =
			std::make_unique<QAtomicInteger<quint8>[]>(
				static_cast<std::size_t>(nVox));

		for (int i = 0; i < nVox; ++i)
			labelMap[i].storeRelaxed(0u);

		// ── 4. Concurrent per-seed BFS ────────────────────────────────────
		// Each task grows from its own seed voxel.  Voxel ownership is
		// decided by CAS: the first thread to claim a voxel enqueues it into
		// its own BFS queue; all others skip it.  Seeds on the same physical
		// bone will each win their own starting voxel and grow until they
		// collide — the union-find pass (step 5) re-unites them.
		struct BfsResult
		{
			int       seedIndex = -1;
			quint8    label = 0u;
			vtkIdType voxelCount = 0;
			int       bbMin[3] = {};
			int       bbMax[3] = {};
			bool      valid = false;
		};

		// Raw pointer captured by value — the unique_ptr owner outlives all
		// futures because .result() is called before this function returns.
		QAtomicInteger<quint8>* lmRaw = labelMap.get();
		const quint8* binRaw = binary.data();

		QVector<QFuture<BfsResult>> futures;
		futures.reserve(nSeeds);

		for (int s = 0; s < nSeeds; ++s)
		{
			// Copy by value: the lambda must not hold a reference into setups
			// since this loop may continue modifying it while tasks run.
			const SeedSetup ss = setups[static_cast<std::size_t>(s)];

			if (!ss.valid)
			{
				futures.append(QtConcurrent::run(
					[s]() -> BfsResult { return BfsResult{ s, 0u, 0, {}, {}, false }; }));
				continue;
			}

			futures.append(QtConcurrent::run(
				[s, ss, NX, NY, NZ, lmRaw, binRaw, flatIdx, &offsets]() -> BfsResult
				{
					BfsResult result;
					result.seedIndex = s;
					result.label = ss.label;
					result.bbMin[0] = ss.voxel[0];
					result.bbMin[1] = ss.voxel[1];
					result.bbMin[2] = ss.voxel[2];
					result.bbMax[0] = ss.voxel[0];
					result.bbMax[1] = ss.voxel[1];
					result.bbMax[2] = ss.voxel[2];

					const vtkIdType seedFlat =
						flatIdx(ss.voxel[0], ss.voxel[1], ss.voxel[2]);

					// Claim seed voxel; abort if another task already owns it.
					if (!lmRaw[seedFlat].testAndSetOrdered(0u, ss.label))
						return result;  // valid remains false

					std::queue<std::array<vtkIdType, 3>> bfsQueue;
					bfsQueue.push({ ss.voxel[0], ss.voxel[1], ss.voxel[2] });
					result.valid = true;

					while (!bfsQueue.empty())
					{
						const auto cur = bfsQueue.front();
						bfsQueue.pop();

						const vtkIdType cx = cur[0];
						const vtkIdType cy = cur[1];
						const vtkIdType cz = cur[2];

						++result.voxelCount;

						result.bbMin[0] = std::min(result.bbMin[0], static_cast<int>(cx));
						result.bbMin[1] = std::min(result.bbMin[1], static_cast<int>(cy));
						result.bbMin[2] = std::min(result.bbMin[2], static_cast<int>(cz));
						result.bbMax[0] = std::max(result.bbMax[0], static_cast<int>(cx));
						result.bbMax[1] = std::max(result.bbMax[1], static_cast<int>(cy));
						result.bbMax[2] = std::max(result.bbMax[2], static_cast<int>(cz));

						for (const auto& off : offsets)
						{
							const vtkIdType nx_ = cx + off[0];
							const vtkIdType ny_ = cy + off[1];
							const vtkIdType nz_ = cz + off[2];

							if (nx_ < 0 || nx_ >= NX ||
								ny_ < 0 || ny_ >= NY ||
								nz_ < 0 || nz_ >= NZ)
								continue;

							const vtkIdType   nFlat = flatIdx(nx_, ny_, nz_);
							const std::size_t nFlatSz = static_cast<std::size_t>(nFlat);

							if (binRaw[nFlatSz] == 0u)
								continue;

							// CAS: enqueue only if this task wins ownership.
							if (lmRaw[nFlat].testAndSetOrdered(0u, ss.label))
								bfsQueue.push({ nx_, ny_, nz_ });
						}
					}

					return result;
				}));
		}

		// Collect all BFS results — .result() blocks until each task is done.
		std::vector<BfsResult> bfsResults;
		bfsResults.reserve(static_cast<std::size_t>(nSeeds));

		for (int s = 0; s < nSeeds; ++s)
		{
			bfsResults.push_back(futures[s].result());

			if (progressCb)
			{
				const int pct = 10 + static_cast<int>(
					50.0 * (s + 1) / static_cast<double>(nSeeds));
				progressCb(pct);
			}
		}

		if (progressCb) progressCb(60);

		// ── 5. Union-find merge ───────────────────────────────────────────
		// Seeds that landed on the same physical bone each grew a fragment with
		// a distinct label.  The fragments share 26-connected boundaries in the
		// label map.  A single O(N) adjacency scan detects all such boundaries;
		// union-find with path compression merges the sets so every fragment of
		// the same bone resolves to one canonical root label.
		//
		// Labels are 1-based (1 … nSeeds).  Index 0 in parent[] is unused.
		const int maxLabel = nSeeds + 1;
		std::vector<int> parent(static_cast<std::size_t>(maxLabel));
		std::iota(parent.begin(), parent.end(), 0);

		// find() with path compression — not thread-safe but only called serially.
		std::function<int(int)> find = [&](int x) -> int
			{
				if (parent[static_cast<std::size_t>(x)] != x)
					parent[static_cast<std::size_t>(x)] =
					find(parent[static_cast<std::size_t>(x)]);
				return parent[static_cast<std::size_t>(x)];
			};

		const auto unite = [&](int a, int b)
			{
				a = find(a);
				b = find(b);
				if (a != b)
					parent[static_cast<std::size_t>(a)] = b;
			};

		// 6-connected adjacency scan — sufficient to detect boundaries between
		// 26-connected BFS regions (any two touching regions share at least one
		// 6-connected face voxel pair).
		constexpr int adj6[6][3] = {
			{ 1,0,0},{-1,0,0},
			{ 0,1,0},{ 0,-1,0},
			{ 0,0,1},{ 0,0,-1}
		};

		for (vtkIdType z = 0; z < NZ; ++z)
			for (vtkIdType y = 0; y < NY; ++y)
				for (vtkIdType x = 0; x < NX; ++x)
				{
					const auto aLbl = static_cast<int>(
						labelMap[static_cast<std::size_t>(flatIdx(x, y, z))].loadRelaxed());
					if (aLbl == 0) continue;

					for (const auto& d : adj6)
					{
						const vtkIdType nx_ = x + d[0];
						const vtkIdType ny_ = y + d[1];
						const vtkIdType nz_ = z + d[2];

						if (nx_ < 0 || nx_ >= NX ||
							ny_ < 0 || ny_ >= NY ||
							nz_ < 0 || nz_ >= NZ)
							continue;

						const auto bLbl = static_cast<int>(
							labelMap[static_cast<std::size_t>(
							flatIdx(nx_, ny_, nz_))].loadRelaxed());

						if (bLbl != 0 && bLbl != aLbl)
							unite(aLbl, bLbl);
					}
				}

		if (progressCb) progressCb(75);

		// ── 6. Build canonical BoneIsland records ─────────────────────────
		// Group BfsResults by their canonical root label.  Merge voxelCount
		// and expand bounding boxes across all fragments that share a root.
		// The seed chosen for a merged island is the one with the most voxels
		// (most representative starting point).

		// canonical root label → index into mergedResults
		std::unordered_map<int, std::size_t> rootToIdx;
		rootToIdx.reserve(static_cast<std::size_t>(nSeeds));

		struct MergedResult
		{
			int       canonicalRoot = 0;
			quint8    label = 0u;  // final 1-based output label
			vtkIdType voxelCount = 0;
			int       bbMin[3] = {};
			int       bbMax[3] = {};
			int       seedIndex = -1;  // seed with the most voxels
			vtkIdType seedVoxelCount = 0;  // voxelCount of the representative seed
		};

		std::vector<MergedResult> mergedResults;
		mergedResults.reserve(static_cast<std::size_t>(nSeeds));

		for (int s = 0; s < nSeeds; ++s)
		{
			const BfsResult& r = bfsResults[static_cast<std::size_t>(s)];
			if (!r.valid || r.voxelCount == 0)
				continue;

			const int root = find(static_cast<int>(r.label));

			auto it = rootToIdx.find(root);
			if (it == rootToIdx.end())
			{
				// First fragment for this root — create a new merged entry.
				MergedResult mr;
				mr.canonicalRoot = root;
				mr.voxelCount = r.voxelCount;
				mr.bbMin[0] = r.bbMin[0]; mr.bbMin[1] = r.bbMin[1]; mr.bbMin[2] = r.bbMin[2];
				mr.bbMax[0] = r.bbMax[0]; mr.bbMax[1] = r.bbMax[1]; mr.bbMax[2] = r.bbMax[2];
				mr.seedIndex = s;
				mr.seedVoxelCount = r.voxelCount;
				rootToIdx[root] = mergedResults.size();
				mergedResults.push_back(mr);
			}
			else
			{
				// Additional fragment — accumulate into existing entry.
				MergedResult& mr = mergedResults[it->second];
				mr.voxelCount += r.voxelCount;
				mr.bbMin[0] = std::min(mr.bbMin[0], r.bbMin[0]);
				mr.bbMin[1] = std::min(mr.bbMin[1], r.bbMin[1]);
				mr.bbMin[2] = std::min(mr.bbMin[2], r.bbMin[2]);
				mr.bbMax[0] = std::max(mr.bbMax[0], r.bbMax[0]);
				mr.bbMax[1] = std::max(mr.bbMax[1], r.bbMax[1]);
				mr.bbMax[2] = std::max(mr.bbMax[2], r.bbMax[2]);

				// Promote to representative seed if this fragment is larger.
				if (r.voxelCount > mr.seedVoxelCount)
				{
					mr.seedIndex = s;
					mr.seedVoxelCount = r.voxelCount;
				}
			}
		}

		// Assign final 1-based output labels in order of merged entry index.
		for (std::size_t m = 0; m < mergedResults.size(); ++m)
			mergedResults[m].label = static_cast<quint8>(m + 1u);

		// Build remapping table: BFS label → final output label.
		// All fragments under the same root get the same final label.
		std::vector<quint8> labelRemap(static_cast<std::size_t>(maxLabel), 0u);
		for (const MergedResult& mr : mergedResults)
		{
			// Walk every BfsResult whose root matches this merged entry and remap.
			for (int s = 0; s < nSeeds; ++s)
			{
				const BfsResult& r = bfsResults[static_cast<std::size_t>(s)];
				if (!r.valid || r.voxelCount == 0)
					continue;
				if (find(static_cast<int>(r.label)) == mr.canonicalRoot)
					labelRemap[static_cast<std::size_t>(r.label)] = mr.label;
			}
		}

		if (progressCb) progressCb(80);

		// ── 7. Parallel output pack with remapping ────────────────────────
		// Remap every atomic label in labelMap to the final merged label and
		// write it directly into the output vtkImageData scalar buffer.
		outLabelImage = vtkSmartPointer<vtkImageData>::New();
		outLabelImage->SetDimensions(dims);
		outLabelImage->SetSpacing(spacing);
		outLabelImage->SetOrigin(origin);
		outLabelImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

		auto* outPtr = static_cast<quint8*>(outLabelImage->GetScalarPointer());

		{
			QVector<int> idx(nVox);
			std::iota(idx.begin(), idx.end(), 0);

			const QAtomicInteger<quint8>* lmConst = labelMap.get();
			const quint8* remapConst = labelRemap.data();

			QtConcurrent::blockingMap(idx,
				[lmConst, remapConst, outPtr](int i)
				{
					const quint8 raw = lmConst[i].loadRelaxed();
					outPtr[i] = (raw == 0u)
						? 0u
						: remapConst[static_cast<std::size_t>(raw)];
				});
		}

		if (progressCb) progressCb(90);

		// ── 8. Finalise BoneIsland records ───────────────────────────────
		auto packVec3 = [](const double v[3]) -> QJsonArray
			{
				return QJsonArray{ v[0], v[1], v[2] };
			};

		std::vector<BoneIsland> islands;
		islands.reserve(mergedResults.size());

		for (const MergedResult& mr : mergedResults)
		{
			const SeedSetup& ss = setups[static_cast<std::size_t>(mr.seedIndex)];

			double bbIdxMin[3] = {
				static_cast<double>(mr.bbMin[0]),
				static_cast<double>(mr.bbMin[1]),
				static_cast<double>(mr.bbMin[2])
			};
			double bbIdxMax[3] = {
				static_cast<double>(mr.bbMax[0]),
				static_cast<double>(mr.bbMax[1]),
				static_cast<double>(mr.bbMax[2])
			};
			double bbWorldMin[3] = {}, bbWorldMax[3] = {};
			reslicedImage->TransformContinuousIndexToPhysicalPoint(bbIdxMin, bbWorldMin);
			reslicedImage->TransformContinuousIndexToPhysicalPoint(bbIdxMax, bbWorldMax);

			QJsonObject islandJson;
			islandJson[QStringLiteral("label")] = static_cast<int>(mr.label);
			islandJson[QStringLiteral("voxelCount")] = static_cast<qint64>(mr.voxelCount);
			islandJson[QStringLiteral("seedWorld")] = packVec3(ss.world);
			islandJson[QStringLiteral("bbMin")] = packVec3(bbWorldMin);
			islandJson[QStringLiteral("bbMax")] = packVec3(bbWorldMax);

			qDebug("segmentBoneIslandsParallel: merged island label=%d  "
				"%lld voxels  BB [%d,%d,%d]-[%d,%d,%d]",
				static_cast<int>(mr.label),
				static_cast<long long>(mr.voxelCount),
				mr.bbMin[0], mr.bbMin[1], mr.bbMin[2],
				mr.bbMax[0], mr.bbMax[1], mr.bbMax[2]);

			BoneIsland island;
			island.label = static_cast<int>(mr.label);
			island.voxelCount = mr.voxelCount;
			island.seedWorld[0] = ss.world[0];
			island.seedWorld[1] = ss.world[1];
			island.seedWorld[2] = ss.world[2];
			island.seedVoxel[0] = ss.voxel[0];
			island.seedVoxel[1] = ss.voxel[1];
			island.seedVoxel[2] = ss.voxel[2];
			island.json = islandJson;

			islands.push_back(island);
		}

		if (progressCb) progressCb(100);

		return islands;
	}

	// -----------------------------------------------------------------------
	// Inward seed adjustment for iterative region growing
	//
	// At higher thresholds the original landmark surface tips may fall below
	// the threshold criterion, causing the BFS to find no island.  This helper
	// walks each seed one voxel-step at a time toward the PCA centroid along
	// its known eigen-axis direction and returns the position of the first
	// voxel whose scalar value satisfies scalar >= threshold.
	//
	// Step size is half the smallest voxel spacing so no voxel is skipped.
	// -----------------------------------------------------------------------
	std::vector<std::array<double, 3>> computeInwardAdjustedSeeds(
		vtkImageData* image,
		double                                     threshold,
		const std::vector<std::array<double, 3>>& originalSeedsWorld,
		const PcaResult& pca)
	{
		// Start with a copy — any seed that cannot be adjusted is returned unchanged.
		std::vector<std::array<double, 3>> adjusted = originalSeedsWorld;

		if (!image || originalSeedsWorld.empty() || !pca.valid)
			return adjusted;

		vtkDataArray* scalars = image->GetPointData()->GetScalars();
		if (!scalars)
			return adjusted;

		const double* spacing = image->GetSpacing();
		const int* dims = image->GetDimensions();

		// Sub-voxel step: half the smallest dimension spacing.
		const double stepSize = 0.5 * std::min({ spacing[0], spacing[1], spacing[2] });

		const int nSeeds = static_cast<int>(originalSeedsWorld.size());

		for (int s = 0; s < nSeeds; ++s)
		{
			// Determine which eigen axis and which tip this seed represents.
			// Seeds are packed as: axis=s/2, tip=s%2 (0=positive, 1=negative).
			const int    axis = s / 2;
			const int    tip = s % 2;

			// Inward direction toward the centroid:
			//   positive tip sits at  centroid + R*axes[axis]  -> inward = -axes[axis]
			//   negative tip sits at  centroid - R*axes[axis]  -> inward = +axes[axis]
			const double sign = (tip == 0) ? -1.0 : +1.0;
			const double inward[3] = {
				sign * pca.axes[axis][0],
				sign * pca.axes[axis][1],
				sign * pca.axes[axis][2]
			};

			const auto& sw = originalSeedsWorld[static_cast<std::size_t>(s)];

			// Maximum walk distance: from the seed to the centroid.
			const double dx = pca.centroid[0] - sw[0];
			const double dy = pca.centroid[1] - sw[1];
			const double dz = pca.centroid[2] - sw[2];
			const double distToCentroid = std::sqrt(dx * dx + dy * dy + dz * dz);

			if (distToCentroid < stepSize)
				continue; // seed is essentially at the centroid; leave unchanged

			const int maxSteps = static_cast<int>(std::ceil(distToCentroid / stepSize));
			bool      found = false;

			for (int step = 0; step <= maxSteps; ++step)
			{
				const double t = static_cast<double>(step) * stepSize;
				const double pos[3] = {
					sw[0] + t * inward[0],
					sw[1] + t * inward[1],
					sw[2] + t * inward[2]
				};

				// Map world position to nearest voxel index.
				double contIdx[3] = { 0.0, 0.0, 0.0 };
				image->TransformPhysicalPointToContinuousIndex(pos, contIdx);

				const int ix = static_cast<int>(std::lround(contIdx[0]));
				const int iy = static_cast<int>(std::lround(contIdx[1]));
				const int iz = static_cast<int>(std::lround(contIdx[2]));

				if (ix < 0 || ix >= dims[0] ||
					iy < 0 || iy >= dims[1] ||
					iz < 0 || iz >= dims[2])
					continue;

				const vtkIdType flat =
					static_cast<vtkIdType>(iz) * dims[1] * dims[0]
					+ static_cast<vtkIdType>(iy) * dims[0]
					+ static_cast<vtkIdType>(ix);

				if (scalars->GetTuple1(flat) >= threshold)
				{
					adjusted[static_cast<std::size_t>(s)] = { pos[0], pos[1], pos[2] };
					qDebug("computeInwardAdjustedSeeds: seed %d adjusted after %d steps "
						   "(%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)",
						   s, step,
						   sw[0], sw[1], sw[2],
						   pos[0], pos[1], pos[2]);
					found = true;
					break;
				}
			}

			if (!found)
			{
				qDebug("computeInwardAdjustedSeeds: seed %d — no qualifying voxel found "
					   "along inward walk; original position retained.", s);
			}
		}

		return adjusted;
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

		// High-luminance pastel palette - bright enough to contrast against the
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
	// Bone island segmentation - VTK morphological pipeline (alternate)
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
		// Step 1: Gaussian smooth
		// ------------------------------------------------------------------
		auto smoother = vtkSmartPointer<vtkImageGaussianSmooth>::New();
		smoother->SetInputData(reslicedImage);
		smoother->SetStandardDeviation(smoothStdDev);
		smoother->SetRadiusFactor(2.0);
		smoother->Update();

		qDebug("segmentBoneIslandsAlternate: Gaussian smooth done (stdDev=%.2f).", smoothStdDev);
		if (progressCb) progressCb(10);

		// ------------------------------------------------------------------
		// Step 2: Erosion
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
		// Step 3: Dilation
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
		// Step 4: Seeded vtkImageThresholdConnectivity - one pass per seed.
		// ------------------------------------------------------------------
		vtkImageData* morphOutput = dilate->GetOutput();

		const double* origin = reslicedImage->GetOrigin();
		const double* spacing = reslicedImage->GetSpacing();
		const int* dims = reslicedImage->GetDimensions();

		const vtkIdType nx = dims[0];
		const vtkIdType ny = dims[1];
		const vtkIdType nz = dims[2];
		const vtkIdType totalVoxels = nx * ny * nz;

		std::vector<unsigned char> labelMap(static_cast<std::size_t>(totalVoxels), 0u);

		auto flatIdx = [&](vtkIdType x, vtkIdType y, vtkIdType z) -> vtkIdType
			{
				return z * ny * nx + y * nx + x;
			};

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

			// Skip if already claimed by a previous seed
			if (labelMap[static_cast<std::size_t>(
				flatIdx(seedVox[0], seedVox[1], seedVox[2]))] != 0u)
			{
				qWarning("segmentBoneIslandsAlternate: seed %d voxel (%d,%d,%d) "
						 "already labelled; skipped.",
						 s, seedVox[0], seedVox[1], seedVox[2]);
				continue;
			}

			auto conn = vtkSmartPointer<vtkImageThresholdConnectivity>::New();
			conn->SetInputData(morphOutput);
			conn->ThresholdByUpper(threshold);
			conn->SetInValue(1.0);
			conn->SetOutValue(0.0);
			conn->ReplaceInOn();
			conn->ReplaceOutOn();
			auto seedPoints = vtkSmartPointer<vtkPoints>::New();
			seedPoints->InsertNextPoint(seedW[0], seedW[1], seedW[2]);
			conn->SetSeedPoints(seedPoints);
			conn->Update();

			vtkImageData* connOut = conn->GetOutput();
			vtkDataArray* connScalars = connOut->GetPointData()->GetScalars();

			if (!connScalars)
			{
				qWarning("segmentBoneIslandsAlternate: seed %d - connectivity filter "
						 "produced no scalars; skipped.", s);
				continue;
			}

			const vtkIdType seedFlat = flatIdx(seedVox[0], seedVox[1], seedVox[2]);
			if (connScalars->GetTuple1(seedFlat) < 0.5)
			{
				qWarning("segmentBoneIslandsAlternate: seed %d (%.2f, %.2f, %.2f) "
						 "- voxel (%d,%d,%d) is below threshold after morphology; skipped.",
						 s, seedW[0], seedW[1], seedW[2],
						 seedVox[0], seedVox[1], seedVox[2]);
				continue;
			}

			const unsigned char islandLabel =
				static_cast<unsigned char>(islands.size() + 1u);

			vtkIdType voxelCount = 0;
			int bbVoxMin[3] = { seedVox[0], seedVox[1], seedVox[2] };
			int bbVoxMax[3] = { seedVox[0], seedVox[1], seedVox[2] };

			for (int k = 0; k < dims[2]; ++k)
				for (int j = 0; j < dims[1]; ++j)
					for (int i = 0; i < dims[0]; ++i)
					{
						const vtkIdType idx = flatIdx(i, j, k);
						const auto      idxSz = static_cast<std::size_t>(idx);

						if (connScalars->GetTuple1(idx) < 0.5) continue;
						if (labelMap[idxSz] != 0u)             continue;

						labelMap[idxSz] = islandLabel;
						++voxelCount;

						bbVoxMin[0] = std::min(bbVoxMin[0], i);
						bbVoxMin[1] = std::min(bbVoxMin[1], j);
						bbVoxMin[2] = std::min(bbVoxMin[2], k);
						bbVoxMax[0] = std::max(bbVoxMax[0], i);
						bbVoxMax[1] = std::max(bbVoxMax[1], j);
						bbVoxMax[2] = std::max(bbVoxMax[2], k);
					}

			qDebug("segmentBoneIslandsAlternate: seed %d - island label %u, %lld voxels, "
				   "BB voxel [%d,%d,%d]-[%d,%d,%d]",
				   s, static_cast<unsigned>(islandLabel),
				   static_cast<long long>(voxelCount),
				   bbVoxMin[0], bbVoxMin[1], bbVoxMin[2],
				   bbVoxMax[0], bbVoxMax[1], bbVoxMax[2]);

			double bbIdxMin[3] = { static_cast<double>(bbVoxMin[0]),
								   static_cast<double>(bbVoxMin[1]),
								   static_cast<double>(bbVoxMin[2]) };
			double bbIdxMax[3] = { static_cast<double>(bbVoxMax[0]),
								   static_cast<double>(bbVoxMax[1]),
								   static_cast<double>(bbVoxMax[2]) };
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

			if (progressCb)
			{
				const int pct = 40 + static_cast<int>(
					50.0 * (s + 1) / static_cast<double>(nSeeds));
				progressCb(pct);
			}
		}

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
	// buildGraphCutSeedImages
	//
	// Foreground seed paths: 6 straight-line segments from the bone centroid
	// (midpoint of the longest-axis landmark pair) out to each of the 6
	// landmark surface points.
	//
	// Naming convention (by eigenvalue rank of paired distance):
	//   S = smallest eigenvalue axis  (shortest paired distance)
	//   M = medium  eigenvalue axis
	//   L = largest eigenvalue axis   (longest paired distance)
	//   Pos / Neg = positive / negative eigenvector direction
	//
	// centroidSpos, centroidSneg, centroidMpos, centroidMneg, centroidLneg
	//   used as foreground seeds (5 of 6 segments).
	//
	// centroidLpos EXCLUDED from foreground seeds.
	//   The Lpos tip is on the side adjacent to a neighbouring bone.
	//   Including this segment risks seeding through a touch-point and
	//   pulling the adjacent bone into the foreground label.
	//
	// Background rays: one threshold-gated outward ray per selected landmark.
	//   Spos, Sneg, Mpos, Mneg, Lneg 5 background rays.
	//   Lpos no background ray (same reason as above).
	//
	// All background rays skip the first 10 voxels past the landmark surface
	// before beginning the threshold gate (cortex-skip buffer).
	// -----------------------------------------------------------------------
	void buildGraphCutSeedImages(
		vtkImageData* reslicedImage,
		const std::array<std::array<std::array<double, 3>, 2>, 3>& landmarkPoints,
		const double                             eigenvectors[3][3],
		vtkSmartPointer<vtkImageData>& outForegroundSeeds,
		vtkSmartPointer<vtkImageData>& outBackgroundSeeds,
		double                                   threshold)
	{
		const double* origin = reslicedImage->GetOrigin();
		const double* spacing = reslicedImage->GetSpacing();
		const int* dims = reslicedImage->GetDimensions();
		vtkDataArray* scalars = reslicedImage->GetPointData()->GetScalars();

		const vtkIdType nx = dims[0];
		const vtkIdType ny = dims[1];
		const vtkIdType nz = dims[2];
		const vtkIdType totalVoxels = nx * ny * nz;

		const double step = std::min({ spacing[0], spacing[1], spacing[2] });

		auto allocSeedImage = [&]() -> vtkSmartPointer<vtkImageData>
			{
				auto img = vtkSmartPointer<vtkImageData>::New();
				img->SetDimensions(dims);
				img->SetSpacing(spacing);
				img->SetOrigin(origin);
				img->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
				// sizeof(unsigned char) == 1, so byte count == totalVoxels.
				// This memset is only correct for VTK_UNSIGNED_CHAR scalars.
				std::memset(img->GetScalarPointer(), 0,
					static_cast<std::size_t>(totalVoxels) * sizeof(unsigned char));
				return img;
			};

		outForegroundSeeds = allocSeedImage();
		outBackgroundSeeds = allocSeedImage();

		auto* fgPtr = static_cast<unsigned char*>(outForegroundSeeds->GetScalarPointer());
		auto* bgPtr = static_cast<unsigned char*>(outBackgroundSeeds->GetScalarPointer());

		// ------------------------------------------------------------------
		// Helper: mark a world-space point in a seed buffer.
		// ------------------------------------------------------------------
		auto markVoxel = [&](unsigned char* buf, const double w[3])
			{
				const int ix = static_cast<int>((w[0] - origin[0]) / spacing[0] + 0.5);
				const int iy = static_cast<int>((w[1] - origin[1]) / spacing[1] + 0.5);
				const int iz = static_cast<int>((w[2] - origin[2]) / spacing[2] + 0.5);
				if (ix < 0 || ix >= dims[0] ||
					iy < 0 || iy >= dims[1] ||
					iz < 0 || iz >= dims[2])
					return;
				buf[iz * ny * nx + iy * nx + ix] = 1u;
			};

		// ------------------------------------------------------------------
		// Helper: sample the scalar value at a world-space point.
		// Returns 0 when the point is outside the image extent.
		// ------------------------------------------------------------------
		auto sampleScalar = [&](const double w[3]) -> double
			{
				const int ix = static_cast<int>((w[0] - origin[0]) / spacing[0] + 0.5);
				const int iy = static_cast<int>((w[1] - origin[1]) / spacing[1] + 0.5);
				const int iz = static_cast<int>((w[2] - origin[2]) / spacing[2] + 0.5);
				if (ix < 0 || ix >= dims[0] ||
					iy < 0 || iy >= dims[1] ||
					iz < 0 || iz >= dims[2])
					return 0.0;
				const vtkIdType flat = static_cast<vtkIdType>(iz) * ny * nx
					+ static_cast<vtkIdType>(iy) * nx + ix;
				return scalars->GetTuple1(flat);
			};

		// ------------------------------------------------------------------
		// Helper: walk a straight-line foreground segment from p0 to p1.
		// ------------------------------------------------------------------
		auto walkSegment = [&](unsigned char* buf,
							   const double   p0[3],
							   const double   p1[3])
			{
				const double dx = p1[0] - p0[0];
				const double dy = p1[1] - p0[1];
				const double dz = p1[2] - p0[2];
				const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (len < 1e-6) { markVoxel(buf, p0); return; }
				const double dir[3] = { dx / len, dy / len, dz / len };
				double cur[3] = { p0[0], p0[1], p0[2] };
				double walked = 0.0;
				while (walked <= len)
				{
					markVoxel(buf, cur);
					cur[0] += dir[0] * step;
					cur[1] += dir[1] * step;
					cur[2] += dir[2] * step;
					walked += step;
				}
			};

		// ------------------------------------------------------------------
		// Helper: walk a threshold-gated background ray from 'start' outward
		// in 'dir'.  Skips the first cortexSkipVoxels steps past the landmark
		// surface before the threshold gate activates.  Stops immediately upon
		// re-entering any bone-density tissue (scalar >= threshold).
		// ------------------------------------------------------------------
		constexpr int cortexSkipVoxels = 10;

		auto walkBgRay = [&](const double start[3], const double dir[3])
			{
				const double bbMin[3] = { origin[0], origin[1], origin[2] };
				const double bbMax[3] = {
					origin[0] + (dims[0] - 1) * spacing[0],
					origin[1] + (dims[1] - 1) * spacing[1],
					origin[2] + (dims[2] - 1) * spacing[2]
				};

				double tExit = 0.0;
				if (!rayAabbExit(start, dir, bbMin, bbMax, tExit))
					return;

				const double skipDist = cortexSkipVoxels * step;

				double cur[3] = { start[0] + dir[0] * skipDist,
								  start[1] + dir[1] * skipDist,
								  start[2] + dir[2] * skipDist };
				double walked = skipDist;
				vtkIdType bgVoxelsMarked = 0;

				while (walked <= tExit)
				{
					if (sampleScalar(cur) >= threshold)
					{
						qDebug("buildGraphCutSeedImages: BG ray stopped at bone "
							   "re-entry after %.1f mm (%lld voxels marked).",
							   walked, static_cast<long long>(bgVoxelsMarked));
						break;
					}

					markVoxel(bgPtr, cur);
					++bgVoxelsMarked;

					cur[0] += dir[0] * step;
					cur[1] += dir[1] * step;
					cur[2] += dir[2] * step;
					walked += step;
				}
			};

		// ------------------------------------------------------------------
		// Sort axes by paired landmark distance (lPos-lNeg span) ascending.
		//   axisInfos[0] = S axis (smallest eigenvalue / shortest span)
		//   axisInfos[1] = M axis (medium)
		//   axisInfos[2] = L axis (largest eigenvalue / longest span)
		// ------------------------------------------------------------------
		struct AxisInfo { int axisIdx; double pairedDist; };
		std::array<AxisInfo, 3> axisInfos;
		for (int i = 0; i < 3; ++i)
		{
			const double* lPos = landmarkPoints[static_cast<std::size_t>(i)][0].data();
			const double* lNeg = landmarkPoints[static_cast<std::size_t>(i)][1].data();
			const double dx = lPos[0] - lNeg[0];
			const double dy = lPos[1] - lNeg[1];
			const double dz = lPos[2] - lNeg[2];
			axisInfos[static_cast<std::size_t>(i)] = {
				i, std::sqrt(dx * dx + dy * dy + dz * dz)
			};
		}
		std::sort(axisInfos.begin(), axisInfos.end(),
			[](const AxisInfo& a, const AxisInfo& b)
			{ return a.pairedDist < b.pairedDist; });

		// Resolve named axis indices after sort
		const int sAxisIdx = axisInfos[0].axisIdx;   // smallest eigenvalue
		const int mAxisIdx = axisInfos[1].axisIdx;   // medium eigenvalue
		const int lAxisIdx = axisInfos[2].axisIdx;   // largest eigenvalue

		qDebug("buildGraphCutSeedImages: S=axis%d(%.2f)  M=axis%d(%.2f)  L=axis%d(%.2f)",
			   sAxisIdx, axisInfos[0].pairedDist,
			   mAxisIdx, axisInfos[1].pairedDist,
			   lAxisIdx, axisInfos[2].pairedDist);

		// Resolve the 6 landmark pointers by name
		const double* centroidSpos = landmarkPoints[static_cast<std::size_t>(sAxisIdx)][0].data();
		const double* centroidSneg = landmarkPoints[static_cast<std::size_t>(sAxisIdx)][1].data();
		const double* centroidMpos = landmarkPoints[static_cast<std::size_t>(mAxisIdx)][0].data();
		const double* centroidMneg = landmarkPoints[static_cast<std::size_t>(mAxisIdx)][1].data();
		const double* centroidLpos = landmarkPoints[static_cast<std::size_t>(lAxisIdx)][0].data();
		const double* centroidLneg = landmarkPoints[static_cast<std::size_t>(lAxisIdx)][1].data();

		// The bone centroid is the midpoint of the L-axis landmark pair.
		// All 6 foreground segments radiate from this point outward to their
		// respective landmark tips.
		const double boneCentroid[3] = {
			0.5 * (centroidLpos[0] + centroidLneg[0]),
			0.5 * (centroidLpos[1] + centroidLneg[1]),
			0.5 * (centroidLpos[2] + centroidLneg[2])
		};

		qDebug("buildGraphCutSeedImages: boneCentroid=(%.2f, %.2f, %.2f)",
			   boneCentroid[0], boneCentroid[1], boneCentroid[2]);

		// ------------------------------------------------------------------
		// Foreground segments - centroid ? each landmark tip.
		//
		// centroidLpos is EXCLUDED: that tip is adjacent to a neighbouring
		// bone and the segment would cross the touch-point, pulling the wrong
		// bone into the foreground label.
		//
		// 5 segments used:
		//   centroid ? Spos
		//   centroid ? Sneg
		//   centroid ? Mpos
		//   centroid ? Mneg
		//   centroid ? Lneg
		// ------------------------------------------------------------------
		walkSegment(fgPtr, boneCentroid, centroidSpos);
		qDebug("buildGraphCutSeedImages: FG centroid?Spos");

		walkSegment(fgPtr, boneCentroid, centroidSneg);
		qDebug("buildGraphCutSeedImages: FG centroid?Sneg");

		walkSegment(fgPtr, boneCentroid, centroidMpos);
		qDebug("buildGraphCutSeedImages: FG centroid?Mpos");

		walkSegment(fgPtr, boneCentroid, centroidMneg);
		qDebug("buildGraphCutSeedImages: FG centroid?Mneg");

		walkSegment(fgPtr, boneCentroid, centroidLneg);
		qDebug("buildGraphCutSeedImages: FG centroid?Lneg");

		// centroidLpos - intentionally excluded from foreground seeds.
		qDebug("buildGraphCutSeedImages: FG centroid?Lpos SKIPPED (adjacent bone side).");

		// ------------------------------------------------------------------
		// Background rays - outward from 5 selected landmark tips.
		//
		// Each ray fires in the outward eigenvector direction from the
		// landmark surface point, skips cortexSkipVoxels past the surface,
		// then marks soft-tissue / air voxels until bone re-entry.
		//
		// Lpos ? no background ray (same exclusion as foreground above).
		// ------------------------------------------------------------------

		// Spos: outward along +S eigenvector
		{
			const double dirSpos[3] = { eigenvectors[sAxisIdx][0],
										 eigenvectors[sAxisIdx][1],
										 eigenvectors[sAxisIdx][2] };
			walkBgRay(centroidSpos, dirSpos);
			qDebug("buildGraphCutSeedImages: BG ray from Spos");
		}

		// Sneg: outward along -S eigenvector
		{
			const double dirSneg[3] = { -eigenvectors[sAxisIdx][0],
										-eigenvectors[sAxisIdx][1],
										-eigenvectors[sAxisIdx][2] };
			walkBgRay(centroidSneg, dirSneg);
			qDebug("buildGraphCutSeedImages: BG ray from Sneg");
		}

		// Mpos: outward along +M eigenvector
		{
			const double dirMpos[3] = { eigenvectors[mAxisIdx][0],
										 eigenvectors[mAxisIdx][1],
										 eigenvectors[mAxisIdx][2] };
			walkBgRay(centroidMpos, dirMpos);
			qDebug("buildGraphCutSeedImages: BG ray from Mpos");
		}

		// Mneg: outward along -M eigenvector
		{
			const double dirMneg[3] = { -eigenvectors[mAxisIdx][0],
										-eigenvectors[mAxisIdx][1],
										-eigenvectors[mAxisIdx][2] };
			walkBgRay(centroidMneg, dirMneg);
			qDebug("buildGraphCutSeedImages: BG ray from Mneg");
		}

		// Lneg: outward along -L eigenvector
		{
			const double dirLneg[3] = { -eigenvectors[lAxisIdx][0],
										-eigenvectors[lAxisIdx][1],
										-eigenvectors[lAxisIdx][2] };
			walkBgRay(centroidLneg, dirLneg);
			qDebug("buildGraphCutSeedImages: BG ray from Lneg");
		}

		// Lpos - intentionally excluded from background rays.
		qDebug("buildGraphCutSeedImages: BG ray from Lpos SKIPPED (adjacent bone side).");

		outForegroundSeeds->Modified();
		outBackgroundSeeds->Modified();

		qDebug("buildGraphCutSeedImages: seed images built "
			   "(5 FG segments from centroid, 5 threshold-gated BG rays).");
	}

	// -----------------------------------------------------------------------
	// Bone island segmentation - ITK ImageGridCutFilter (graph cut)
	// -----------------------------------------------------------------------
	// For a typical Scanco .isq or DICOM bone CT crop (voxel size ~0.05-0.1 mm):
	//
	//   sigma           = 200.0  - 600.0
	//     Start at 300. Increase if the interior fragments; decrease if it leaks.
	//
	//   minIslandVoxels = 100    - 500
	//     A 1 mm^3 cube at 0.05 mm voxel spacing = 8000 voxels.
	//     50 is safe for removing noise; raise to 500+ if many small spurious
	//     fragments survive after tuning sigma.

	std::vector<BoneIsland> segmentBoneIslandsGraphCut(
		vtkImageData* reslicedImage,
		double                                     threshold,
		const std::vector<std::array<double, 3>>& foregroundSeedsWorld,
		const std::vector<std::array<double, 3>>& backgroundSeedsWorld,
		vtkSmartPointer<vtkImageData>& outLabelImage,
		double                                     sigma,
		vtkIdType                                  minIslandVoxels,
		const std::function<void(int)>& progressCb)
	{
		if (!reslicedImage || foregroundSeedsWorld.empty())
			return {};

		vtkDataArray* srcScalars = reslicedImage->GetPointData()->GetScalars();
		if (!srcScalars)
		{
			qWarning("segmentBoneIslandsGraphCut: resliced image has no scalar data.");
			return {};
		}

		if (progressCb) progressCb(0);

		constexpr unsigned int Dim = 3;

		using ShortPixel = short;
		using BinaryPixel = unsigned char;
		using LabelPixel = unsigned short;
		using InputImage = itk::Image<ShortPixel, Dim>;
		using BinaryImage = itk::Image<BinaryPixel, Dim>;
		using LabelImage = itk::Image<LabelPixel, Dim>;

		// ------------------------------------------------------------------
		// Step 1 - VTK ? ITK (cast to short)
		// ------------------------------------------------------------------
		vtkSmartPointer<vtkImageData> shortInput = reslicedImage;
		if (reslicedImage->GetScalarType() != VTK_SHORT)
		{
			auto castVTK = vtkSmartPointer<vtkImageShiftScale>::New();
			castVTK->SetInputData(reslicedImage);
			castVTK->SetOutputScalarTypeToShort();
			castVTK->SetShift(0.0);
			castVTK->SetScale(1.0);
			castVTK->Update();
			shortInput = castVTK->GetOutput();
		}

		using VtkToItk = itk::VTKImageToImageFilter<InputImage>;
		auto vtkToItk = VtkToItk::New();
		vtkToItk->SetInput(shortInput);
		vtkToItk->Update();
		InputImage::ConstPointer itkInput = vtkToItk->GetOutput();

		const int* dims = reslicedImage->GetDimensions();
		if (progressCb) progressCb(10);

		// ------------------------------------------------------------------
		// Step 2 - foreground seed image.
		// Mark each foreground seed voxel and its 6 face-neighbours as FG=1.
		// ------------------------------------------------------------------
		auto fgImage = BinaryImage::New();
		fgImage->SetRegions(itkInput->GetLargestPossibleRegion());
		fgImage->CopyInformation(itkInput);
		fgImage->Allocate();
		fgImage->FillBuffer(0u);

		constexpr itk::OffsetValueType stencil[7][3] = {
			{ 0, 0, 0},
			{ 1, 0, 0}, {-1, 0, 0},
			{ 0, 1, 0}, { 0,-1, 0},
			{ 0, 0, 1}, { 0, 0,-1}
		};

		for (const auto& sw : foregroundSeedsWorld)
		{
			double cont[3];
			reslicedImage->TransformPhysicalPointToContinuousIndex(sw.data(), cont);
			for (const auto& off : stencil)
			{
				const itk::Index<3> ni = {
					static_cast<itk::IndexValueType>(std::lround(cont[0])) + off[0],
					static_cast<itk::IndexValueType>(std::lround(cont[1])) + off[1],
					static_cast<itk::IndexValueType>(std::lround(cont[2])) + off[2]
				};
				if (fgImage->GetLargestPossibleRegion().IsInside(ni))
					fgImage->SetPixel(ni, 1u);
			}
		}

		if (progressCb) progressCb(20);

		// ------------------------------------------------------------------
		// Step 3 - background seed image.
		// Use explicit background seeds when provided; fall back to the
		// automatic bottom-5%-of-range strategy otherwise.
		// ------------------------------------------------------------------
		auto bgImage = BinaryImage::New();
		bgImage->SetRegions(itkInput->GetLargestPossibleRegion());
		bgImage->CopyInformation(itkInput);
		bgImage->Allocate();
		bgImage->FillBuffer(0u);

		if (!backgroundSeedsWorld.empty())
		{
			// Explicit background seeds - same 7-point stencil as foreground
			for (const auto& sw : backgroundSeedsWorld)
			{
				double cont[3];
				reslicedImage->TransformPhysicalPointToContinuousIndex(sw.data(), cont);
				for (const auto& off : stencil)
				{
					const itk::Index<3> ni = {
						static_cast<itk::IndexValueType>(std::lround(cont[0])) + off[0],
						static_cast<itk::IndexValueType>(std::lround(cont[1])) + off[1],
						static_cast<itk::IndexValueType>(std::lround(cont[2])) + off[2]
					};
					if (bgImage->GetLargestPossibleRegion().IsInside(ni))
						bgImage->SetPixel(ni, 1u);
				}
			}
		}
		else
		{
			// Automatic fallback: lowest 5 % of scalar range
			double scalarRange[2];
			reslicedImage->GetScalarRange(scalarRange);
			const double bgCeiling =
				scalarRange[0] + 0.05 * (scalarRange[1] - scalarRange[0]);

			using BgThreshFilter =
				itk::BinaryThresholdImageFilter<InputImage, BinaryImage>;
			auto bgThresh = BgThreshFilter::New();
			bgThresh->SetInput(itkInput);
			bgThresh->SetLowerThreshold(static_cast<ShortPixel>(scalarRange[0]));
			bgThresh->SetUpperThreshold(
				static_cast<ShortPixel>(std::floor(bgCeiling)));
			bgThresh->SetInsideValue(1u);
			bgThresh->SetOutsideValue(0u);
			bgThresh->Update();

			itk::ImageRegionConstIterator<BinaryImage> srcIt(
				bgThresh->GetOutput(),
				bgThresh->GetOutput()->GetLargestPossibleRegion());
			itk::ImageRegionIterator<BinaryImage> dstIt(
				bgImage, bgImage->GetLargestPossibleRegion());
			for (srcIt.GoToBegin(), dstIt.GoToBegin();
				 !srcIt.IsAtEnd(); ++srcIt, ++dstIt)
				dstIt.Set(srcIt.Get());
		}

		if (progressCb) progressCb(30);

		// ------------------------------------------------------------------
		// Step 4 - graph cut
		// ------------------------------------------------------------------
		using GCFilter = GraphCut::FilterType<InputImage, BinaryImage, BinaryImage, BinaryImage>;
		auto gcFilter = GCFilter::New();
		gcFilter->SetInputImage(itkInput.GetPointer());
		gcFilter->SetForegroundImage(fgImage.GetPointer());
		gcFilter->SetBackgroundImage(bgImage.GetPointer());
		gcFilter->SetSigma(sigma);
		gcFilter->SetBoundaryDirectionTypeToNoDirection();
		gcFilter->SetForegroundPixelValue(1u);   // ADD: explicit FG output value
		gcFilter->SetBackgroundPixelValue(0u);   // ADD: explicit BG output value
		gcFilter->Update();

		qDebug("segmentBoneIslandsGraphCut: graph cut done (sigma=%.1f).", sigma);
		if (progressCb) progressCb(70);

		// ------------------------------------------------------------------
		// Step 5 - connected-component labelling on the binary FG mask
		// ------------------------------------------------------------------
		using CCFilter = itk::ConnectedComponentImageFilter<BinaryImage, LabelImage>;
		using RelabelFilter = itk::RelabelComponentImageFilter<LabelImage, LabelImage>;

		auto cc = CCFilter::New();
		cc->SetInput(gcFilter->GetOutput());
		cc->SetFullyConnected(false);
		cc->Update();

		auto relabel = RelabelFilter::New();
		relabel->SetInput(cc->GetOutput());
		relabel->SetMinimumObjectSize(static_cast<unsigned long>(minIslandVoxels));
		relabel->Update();

		const int nLabels = static_cast<int>(relabel->GetNumberOfObjects());
		qDebug("segmentBoneIslandsGraphCut: %d component(s) after relabelling "
			   "(minIslandVoxels=%lld).", nLabels,
			   static_cast<long long>(minIslandVoxels));

		if (nLabels == 0)
		{
			qWarning("segmentBoneIslandsGraphCut: no components survived the minimum-size filter.");
			return {};
		}

		if (progressCb) progressCb(85);

		// ------------------------------------------------------------------
		// Step 6 - ITK label image ? VTK
		// ------------------------------------------------------------------
		using ItkToVtk = itk::ImageToVTKImageFilter<LabelImage>;
		auto itkToVtk = ItkToVtk::New();
		itkToVtk->SetInput(relabel->GetOutput());
		itkToVtk->Update();

		outLabelImage = vtkSmartPointer<vtkImageData>::New();
		outLabelImage->DeepCopy(itkToVtk->GetOutput());

		// ------------------------------------------------------------------
		// Step 7 - build BoneIsland metadata (single O(N) sweep)
		// ------------------------------------------------------------------
		vtkDataArray* labelScalars = outLabelImage->GetPointData()->GetScalars();

		const vtkIdType totalVoxels =
			static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];

		struct IslandAccum
		{
			vtkIdType         voxelCount = 0;
			std::array<int, 3> bbMin = { INT_MAX, INT_MAX, INT_MAX };
			std::array<int, 3> bbMax = { INT_MIN, INT_MIN, INT_MIN };
		};

		std::vector<IslandAccum> accum(static_cast<std::size_t>(nLabels));

		for (vtkIdType k = 0; k < dims[2]; ++k)
			for (vtkIdType j = 0; j < dims[1]; ++j)
				for (vtkIdType i = 0; i < dims[0]; ++i)
				{
					const vtkIdType flat = k * dims[1] * dims[0] + j * dims[0] + i;
					const int lbl = static_cast<int>(labelScalars->GetTuple1(flat));
					if (lbl < 1 || lbl > nLabels) continue;

					IslandAccum& a = accum[static_cast<std::size_t>(lbl - 1)];
					++a.voxelCount;
					a.bbMin[0] = std::min(a.bbMin[0], static_cast<int>(i));
					a.bbMin[1] = std::min(a.bbMin[1], static_cast<int>(j));
					a.bbMin[2] = std::min(a.bbMin[2], static_cast<int>(k));
					a.bbMax[0] = std::max(a.bbMax[0], static_cast<int>(i));
					a.bbMax[1] = std::max(a.bbMax[1], static_cast<int>(j));
					a.bbMax[2] = std::max(a.bbMax[2], static_cast<int>(k));
				}

		std::vector<BoneIsland> islands;
		islands.reserve(static_cast<std::size_t>(nLabels));

		auto packVec3 = [](const double v[3]) -> QJsonArray {
			return QJsonArray{ v[0], v[1], v[2] };
			};

		for (int li = 0; li < nLabels; ++li)
		{
			const IslandAccum& a = accum[static_cast<std::size_t>(li)];

			double idxMin[3] = { static_cast<double>(a.bbMin[0]),
								 static_cast<double>(a.bbMin[1]),
								 static_cast<double>(a.bbMin[2]) };
			double idxMax[3] = { static_cast<double>(a.bbMax[0]),
								 static_cast<double>(a.bbMax[1]),
								 static_cast<double>(a.bbMax[2]) };
			double wMin[3] = {}, wMax[3] = {};
			reslicedImage->TransformContinuousIndexToPhysicalPoint(idxMin, wMin);
			reslicedImage->TransformContinuousIndexToPhysicalPoint(idxMax, wMax);

			// Assign provenance to the nearest foreground seed
			const double cx = 0.5 * (wMin[0] + wMax[0]);
			const double cy = 0.5 * (wMin[1] + wMax[1]);
			const double cz = 0.5 * (wMin[2] + wMax[2]);

			const double* nearestSeed = foregroundSeedsWorld[0].data();
			double minD2 = std::numeric_limits<double>::max();
			for (const auto& sw : foregroundSeedsWorld)
			{
				const double d2 = (cx - sw[0]) * (cx - sw[0])
					+ (cy - sw[1]) * (cy - sw[1])
					+ (cz - sw[2]) * (cz - sw[2]);
				if (d2 < minD2) { minD2 = d2; nearestSeed = sw.data(); }
			}

			const int islandLabel = li + 1;
			QJsonObject islandJson;
			islandJson[QStringLiteral("label")] = islandLabel;
			islandJson[QStringLiteral("voxelCount")] = static_cast<qint64>(a.voxelCount);
			islandJson[QStringLiteral("seedWorld")] = packVec3(nearestSeed);
			islandJson[QStringLiteral("bbMin")] = packVec3(wMin);
			islandJson[QStringLiteral("bbMax")] = packVec3(wMax);

			BoneIsland island;
			island.label = islandLabel;
			island.voxelCount = a.voxelCount;
			island.seedWorld[0] = nearestSeed[0];
			island.seedWorld[1] = nearestSeed[1];
			island.seedWorld[2] = nearestSeed[2];
			island.json = islandJson;
			islands.push_back(island);

			qDebug("segmentBoneIslandsGraphCut: island %d  voxels=%lld  "
				   "BB [%d,%d,%d]-[%d,%d,%d]",
				   islandLabel, static_cast<long long>(a.voxelCount),
				   a.bbMin[0], a.bbMin[1], a.bbMin[2],
				   a.bbMax[0], a.bbMax[1], a.bbMax[2]);
		}

		if (progressCb) progressCb(100);
		return islands;
	}

	// -----------------------------------------------------------------------
	// Graph-cut seed image visualiser
	// -----------------------------------------------------------------------

	vtkSmartPointer<vtkActor> makeSeedImageActor(
		vtkImageData* seedImage,
		double        r, double g, double b,
		double        pointSize)
	{
		if (!seedImage)
			return vtkSmartPointer<vtkActor>::New(); // empty actor - safe no-op

		const double* origin = seedImage->GetOrigin();
		const double* spacing = seedImage->GetSpacing();
		const int* dims = seedImage->GetDimensions();
		vtkDataArray* scalars = seedImage->GetPointData()->GetScalars();

		if (!scalars)
			return vtkSmartPointer<vtkActor>::New();

		auto pts = vtkSmartPointer<vtkPoints>::New();
		auto cells = vtkSmartPointer<vtkCellArray>::New();

		// Reserve an upper-bound to avoid repeated reallocation.
		// The seed images are sparse so actual usage is usually << total.
		const vtkIdType totalVoxels =
			static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];
		pts->Allocate(totalVoxels / 10);

		for (vtkIdType k = 0; k < dims[2]; ++k)
			for (vtkIdType j = 0; j < dims[1]; ++j)
				for (vtkIdType i = 0; i < dims[0]; ++i)
				{
					const vtkIdType flat = k * dims[1] * dims[0] + j * dims[0] + i;
					if (scalars->GetTuple1(flat) < 0.5)
						continue;

					// World-space voxel centre
					const double wx = origin[0] + i * spacing[0];
					const double wy = origin[1] + j * spacing[1];
					const double wz = origin[2] + k * spacing[2];

					const vtkIdType ptId = pts->InsertNextPoint(wx, wy, wz);
					cells->InsertNextCell(1, &ptId); // one vtkVertex per seed voxel
				}

		qDebug("makeSeedImageActor: %lld seed voxels extracted.",
			   static_cast<long long>(pts->GetNumberOfPoints()));

		auto pd = vtkSmartPointer<vtkPolyData>::New();
		pd->SetPoints(pts);
		pd->SetVerts(cells);

		auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
		mapper->SetInputData(pd);
		mapper->ScalarVisibilityOff();

		auto actor = vtkSmartPointer<vtkActor>::New();
		actor->SetMapper(mapper);
		actor->GetProperty()->SetRepresentationToPoints();
		actor->GetProperty()->SetPointSize(static_cast<float>(pointSize));
		actor->GetProperty()->SetColor(r, g, b);
		actor->GetProperty()->SetLighting(false);  // flat colour, unaffected by lights

		return actor;
	}

	// -----------------------------------------------------------------------
	// Region statistics helpers
	// -----------------------------------------------------------------------

	RegionStats computeRegionStats(vtkImageData* reslicedImage,
								   vtkImageData* labelImage)
	{
		if (!reslicedImage || !labelImage)
			return {};

		vtkDataArray* scalars = reslicedImage->GetPointData()->GetScalars();
		vtkDataArray* labels = labelImage->GetPointData()->GetScalars();
		if (!scalars || !labels)
			return {};

		const vtkIdType n = reslicedImage->GetNumberOfPoints();

		// Pass 1: mean
		double    sum = 0.0;
		vtkIdType count = 0;
		for (vtkIdType i = 0; i < n; ++i)
		{
			if (labels->GetTuple1(i) > 0.0)
			{
				sum += scalars->GetTuple1(i);
				++count;
			}
		}
		if (count == 0)
			return {};

		const double mean = sum / static_cast<double>(count);

		// Pass 2: variance (population)
		double sq = 0.0;
		for (vtkIdType i = 0; i < n; ++i)
		{
			if (labels->GetTuple1(i) > 0.0)
			{
				const double d = scalars->GetTuple1(i) - mean;
				sq += d * d;
			}
		}

		return { mean, std::sqrt(sq / static_cast<double>(count)) };
	}

	double computeRegionVolumeMm3(vtkImageData* labelImage)
	{
		if (!labelImage)
			return 0.0;

		const double* sp = labelImage->GetSpacing();
		const double  voxelVol = sp[0] * sp[1] * sp[2];

		vtkDataArray* labels = labelImage->GetPointData()->GetScalars();
		if (!labels)
			return 0.0;

		vtkIdType count = 0;
		const vtkIdType n = labelImage->GetNumberOfPoints();
		for (vtkIdType i = 0; i < n; ++i)
			if (labels->GetTuple1(i) > 0.0) ++count;

		return static_cast<double>(count) * voxelVol;
	}

} // namespace PrototypeHelpers
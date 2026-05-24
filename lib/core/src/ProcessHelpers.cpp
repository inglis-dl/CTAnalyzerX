#include "ProcessHelpers.h"
#include "VoxelLineIterator.h"

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
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include <vtkCellArray.h>
#include <vtkDataArray.h>
#include <vtkDiscreteFlyingEdges3D.h>
#include <vtkImageContinuousDilate3D.h>
#include <vtkImageData.h>
#include <vtkImageGaussianSmooth.h>
#include <vtkImageShiftScale.h>
#include <vtkImageThresholdConnectivity.h>
#include <vtkLine.h>
#include <vtkMath.h>
#include <vtkMatrix3x3.h>
#include <vtkPoints.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkTextProperty.h>
#include <vtkThreshold.h>
#include <vtkUnsignedCharArray.h>

namespace ProcessHelpers
{
	namespace
	{
		Vec3 ToVec3(const double v[3])
		{
			return Vec3(v[0], v[1], v[2]);
		}

		void ToArray(const Vec3& v, double out[3])
		{
			out[0] = v.GetX();
			out[1] = v.GetY();
			out[2] = v.GetZ();
		}
	}

	vtkIdType flatten(const int& ix, const int& iy, const int& iz, const int dims[3])
	{
		return
			static_cast<vtkIdType>(iz) * dims[1] * dims[0] +
			static_cast<vtkIdType>(iy) * dims[0] +
			static_cast<vtkIdType>(ix);
	}

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

	QJsonObject computeScalarThresholdStats(vtkImageData* image, const double& threshold)
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

	// -----------------------------------------------------------------------
	// PCA
	// -----------------------------------------------------------------------

	bool computePca(vtkImageData* image, const double& threshold,
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

		if (progressCb) progressCb(0);

		Vec3 sum(0.0);
		vtkIdType count = 0;

		for (int k = 0; k < dims[2]; ++k)
		{
			for (int j = 0; j < dims[1]; ++j)
			{
				for (int i = 0; i < dims[0]; ++i)
				{
					const vtkIdType idx = flatten(i, j, k, dims);
					if (scalars->GetTuple1(idx) < threshold)
						continue;

					sum += Vec3(
						origin[0] + i * spacing[0],
						origin[1] + j * spacing[1],
						origin[2] + k * spacing[2]);

					++count;
				}
			}
		}

		if (progressCb) progressCb(40);

		if (count < 3)
		{
			qWarning("computePca: fewer than 3 above-threshold voxels (%lld); PCA skipped.",
					 static_cast<long long>(count));
			return false;
		}

		const double invCount = 1.0 / static_cast<double>(count);
		const Vec3 centroid = sum * invCount;
		result.centroid[0] = centroid.GetX();
		result.centroid[1] = centroid.GetY();
		result.centroid[2] = centroid.GetZ();

		double c00 = 0.0, c01 = 0.0, c02 = 0.0;
		double c11 = 0.0, c12 = 0.0;
		double c22 = 0.0;

		Vec3 bbMin(std::numeric_limits<double>::max());
		Vec3 bbMax(std::numeric_limits<double>::lowest());

		for (int k = 0; k < dims[2]; ++k)
		{
			for (int j = 0; j < dims[1]; ++j)
			{
				for (int i = 0; i < dims[0]; ++i)
				{
					const vtkIdType idx = flatten(i, j, k, dims);
					if (scalars->GetTuple1(idx) < threshold)
						continue;

					const Vec3 p(
						origin[0] + i * spacing[0],
						origin[1] + j * spacing[1],
						origin[2] + k * spacing[2]);

					const Vec3 d = p - centroid;

					c00 += d[0] * d[0];  c01 += d[0] * d[1];  c02 += d[0] * d[2];
					c11 += d[1] * d[1];  c12 += d[1] * d[2];
					c22 += d[2] * d[2];

					bbMin[0] = std::min(bbMin[0], p[0]);
					bbMin[1] = std::min(bbMin[1], p[1]);
					bbMin[2] = std::min(bbMin[2], p[2]);
					bbMax[0] = std::max(bbMax[0], p[0]);
					bbMax[1] = std::max(bbMax[1], p[1]);
					bbMax[2] = std::max(bbMax[2], p[2]);
				}
			}
		}

		if (progressCb) progressCb(80);

		c00 *= invCount; c01 *= invCount; c02 *= invCount;
		c11 *= invCount; c12 *= invCount;
		c22 *= invCount;

		double row0[3] = { c00, c01, c02 };
		double row1[3] = { c01, c11, c12 };
		double row2[3] = { c02, c12, c22 };
		double* cov[3] = { row0, row1, row2 };

		double evecData[3][3];
		double* evecs[3] = { evecData[0], evecData[1], evecData[2] };
		double evals[3];

		vtkMath::Jacobi(cov, evals, evecs);

		for (int i = 0; i < 3; ++i)
		{
			result.axes[i][0] = evecs[0][i];
			result.axes[i][1] = evecs[1][i];
			result.axes[i][2] = evecs[2][i];
			vtkMath::Normalize(result.axes[i]);
			result.eigenvalues[i] = evals[i];
		}

		for (int i = 0; i < 3; ++i)
		{
			int dominantComponent = 0;
			double maxAbs = 0.0;
			for (int d = 0; d < 3; ++d)
			{
				const double a = std::abs(result.axes[i][d]);
				if (a > maxAbs) { maxAbs = a; dominantComponent = d; }
			}

			if (result.axes[i][dominantComponent] < 0.0)
			{
				result.axes[i][0] = -result.axes[i][0];
				result.axes[i][1] = -result.axes[i][1];
				result.axes[i][2] = -result.axes[i][2];
			}
		}

		const Vec3 span = bbMax - bbMin;
		result.circumRadius = 0.5 * std::sqrt(span[0] * span[0] + span[1] * span[1] + span[2] * span[2]);

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
		const double tol = 1e-12;
		for (int a = 0; a < 3; ++a)
		{
			const double d = rayDir[a];
			if (std::abs(d) < tol)
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
		const double tol = 1e-12;
		for (int a = 0; a < 3; ++a)
		{
			const double d = rayDir[a];

			if (std::abs(d) < tol)
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

	bool rayAabbIntersect(const Vec3& rayOrigin, const Vec3& rayDir,
					  const Vec3& bbMin, const Vec3& bbMax,
					  double& tEntry, double& tExit)
	{
		return rayAabbIntersect(rayOrigin.GetData(), rayDir.GetData(),
			bbMin.GetData(), bbMax.GetData(), tEntry, tExit);
	}

	bool rayAabbExit(const Vec3& rayOrigin, const Vec3& rayDir,
					 const Vec3& bbMin, const Vec3& bbMax,
					 double& tExit)
	{
		return rayAabbExit(rayOrigin.GetData(), rayDir.GetData(),
			bbMin.GetData(), bbMax.GetData(), tExit);
	}

	bool IntersectLineWithBox(const vtkVector3d& c,	const vtkVector3d& e,
		vtkImageData* image, vtkVector3d& p0, vtkVector3d& p1)
	{
		const double* origin = image->GetOrigin();
		const double* spacing = image->GetSpacing();
		const int* dims = image->GetDimensions();

		double bounds[6];
		bounds[0] = origin[0];
		bounds[1] = origin[0] + spacing[0] * (dims[0] - 1);
		bounds[2] = origin[1];
		bounds[3] = origin[1] + spacing[1] * (dims[1] - 1);
		bounds[4] = origin[2];
		bounds[5] = origin[2] + spacing[2] * (dims[2] - 1);

		double tmin = -std::numeric_limits<double>::infinity();
		double tmax = std::numeric_limits<double>::infinity();
		const double tol = 1e-12;
		for (int d = 0; d < 3; ++d)
		{
			double c_d = c[d];
			double e_d = e[d];

			double bmin = bounds[2 * d];
			double bmax = bounds[2 * d + 1];

			if (std::abs(e_d) < tol)
			{
				// line parallel to slab -> must already be inside
				if (c_d < bmin || c_d > bmax)
					return false;
				continue;
			}

			double t1 = (bmin - c_d) / e_d;
			double t2 = (bmax - c_d) / e_d;

			if (t1 > t2) std::swap(t1, t2);

			tmin = std::max(tmin, t1);
			tmax = std::min(tmax, t2);

			if (tmin > tmax)
				return false;
		}

		// Compute actual intersection points
		p0 = c + tmin * e;
		p1 = c + tmax * e;

		return true;
	}

	void orientPcaAxesForCanonicalReslice(vtkImageData* image,
										  const double& threshold,
										  PcaResult& pca, const bool& flip)
	{
		if (!pca.valid || !image)
			return;

		vtkDataArray* const scalars = image->GetPointData()->GetScalars();
		if (!scalars)
			return;

		const double* const spacing = image->GetSpacing();
		const double* const origin = image->GetOrigin();
		const int* const dims = image->GetDimensions();

		const Vec3 centroid(pca.centroid);

		// ------------------------------------------------------------------
		// Mandatory primary-axis decision.
		// Pick +e0 or -e0 first, and never let secondary-axis alignment alter
		// the chosen primary direction.
		// ------------------------------------------------------------------
		Vec3 axis0(pca.axes[0]);
		const double tol = 1e-12;
		if (axis0.Normalize() <= tol)
			return;

		const Vec3 negAxis0 = -axis0;

		Vec3 tipPos;
		Vec3 tipNeg;
		findSurfacePointFromBoundary(image, centroid, axis0, threshold, tipPos);
		findSurfacePointFromBoundary(image, centroid, negAxis0, threshold, tipNeg);

		const Vec3 dPos = tipPos - centroid;
		const Vec3 dNeg = tipNeg - centroid;
		if (dNeg.SquaredNorm() > dPos.SquaredNorm())
		{
			axis0 = negAxis0;
		}

		// Cached axis-aligned bounds.
		double bounds[6] = {};
		image->GetBounds(bounds);

		const Vec3 bbMin(bounds[0], bounds[2], bounds[4]);
		const Vec3 bbMax(bounds[1], bounds[3], bounds[5]);

		// Secondary axis is aligned only after axis0 is finalized.
		Vec3 axis1(pca.axes[1]);
		axis1 -= axis0 * axis0.Dot(axis1);
		if (axis1.Normalize() <= tol)
		{
			axis1 = Vec3(pca.axes[2]);
			axis1 -= axis0 * axis0.Dot(axis1);
			if (axis1.Normalize() <= tol)
				return;
		}

		// Third axis (smallest eigenvalue) before alignment.
		Vec3 axis2(pca.axes[2]);
		axis2 -= axis0 * axis0.Dot(axis2);
		axis2 -= axis1 * axis1.Dot(axis1);
		if (axis2.Normalize() <= tol)
			return;

		// ------------------------------------------------------------------
		// Secondary axis (e1) alignment using the revised algorithm.
		//
		// 1. Compute nearest bone voxels p_e2_1, p_e2_2 from image bounds
		//    to PCA centroid along e2 and -e2 (smallest eigenvalue vector).
		// 2. Compute distance d between p_e2_1, p_e2_2.
		// 3. Establish point c2 at distance -d from c along corrected axis e0.
		// 4. Establish voxel line v_c2_c from c2 to c along corrected axis e0.
		// 5. For each voxel k on v_c2_c:
		//      - compute nearest bone voxels p_e1_1_k, p_e1_2_k from image
		//        bounds along e1 and -e1 (middle eigenvalue vector e1).
		// 6. Compute maximum distance dmax_pos of all p_e1_1_k voxels.
		// 7. Compute maximum distance dmax_neg of all p_e1_2_k voxels.
		// 8. Align axis e1 to point in the direction of max(dmax_pos, dmax_neg).
		// 9. Update e2 as cross product of revised e0 × revised e1.
		// ------------------------------------------------------------------

		// Step 1: find nearest bone voxels along ±axis2 (smallest eigenvalue).
		Vec3 p_e2_1;
		Vec3 p_e2_2;
		findSurfacePointFromBoundary(image, centroid, axis2, threshold, p_e2_1);
		findSurfacePointFromBoundary(image, centroid, -axis2, threshold, p_e2_2);

		// Step 2: compute distance d between p_e2_1 and p_e2_2.
		const Vec3 delta_e2 = p_e2_2 - p_e2_1;
		const double d = delta_e2.Norm();

		// Step 3: establish point c2 at distance -d from c along corrected axis e0.
		const Vec3 c2 = centroid - (axis0 * d);

		// Step 4: establish voxel line v_c2_c from c2 to c along corrected axis e0.
		VoxelLine line(image, c2, centroid);

		// Step 5-7: probe ±axis1 for each voxel k on v_c2_c.
		double dmax_pos = -1.0;
		double dmax_neg = -1.0;

		for (VoxelLineIterator it = line.begin(); it != line.end(); ++it)
		{
			if (!it.InBounds())
				continue;

			const Vec3d voxelWorld = it.ToWorld();
			const Vec3 samplePt(voxelWorld[0], voxelWorld[1], voxelWorld[2]);

			Vec3 p_e1_1_k;
			Vec3 p_e1_2_k;
			findSurfacePointFromBoundary(image, samplePt, axis1, threshold, p_e1_1_k);
			findSurfacePointFromBoundary(image, samplePt, -axis1, threshold, p_e1_2_k);

			const double dist_pos = (p_e1_1_k - samplePt).Norm();
			const double dist_neg = (p_e1_2_k - samplePt).Norm();

			dmax_pos = std::max(dmax_pos, dist_pos);
			dmax_neg = std::max(dmax_neg, dist_neg);
		}

		// Step 8: align axis e1 to point in the direction of max(dmax_pos, dmax_neg).
		// use flip when exporting the PCA aligned binary bone mask
		//
		if (flip)
		{
			if (dmax_pos > dmax_neg)
				axis1 = -axis1;
		}
		else
		{
			if (dmax_neg > dmax_pos)
				axis1 = -axis1;
		}

		// Step 9: update e2 as cross product of revised e0 × revised e1.
		axis2 = axis0.Cross(axis1);
		if (axis2.Normalize() <= tol)
			return;

		ToArray(axis0, pca.axes[0]);
		ToArray(axis1, pca.axes[1]);
		ToArray(axis2, pca.axes[2]);
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
		const Vec3 c(centroid);
		const Vec3 d(axisDir);

		double bounds[6] = {};
		image->GetBounds(bounds);

		const Vec3 bbMin(bounds[0], bounds[2], bounds[4]);
		const Vec3 bbMax(bounds[1], bounds[3], bounds[5]);

		double tExit = 0.0;
		if (!rayAabbExit(c, d, bbMin, bbMax, tExit))
		{
			outWorld[0] = centroid[0];
			outWorld[1] = centroid[1];
			outWorld[2] = centroid[2];
			return;
		}

		// Walk from the far image boundary back toward the centroid.
		const Vec3 startWorld = c + (d * tExit);
		VoxelLine line(image, startWorld, c);

		VoxelLineIterator it = line.begin();
		if (it.AdvanceToFirst(threshold))
		{
			const auto world = it.ToWorld();
			outWorld[0] = world[0];
			outWorld[1] = world[1];
			outWorld[2] = world[2];
			return;
		}

		// Fallback: return centroid if no qualifying voxel is found.
		outWorld[0] = centroid[0];
		outWorld[1] = centroid[1];
		outWorld[2] = centroid[2];
	}

	void findSurfacePointFromBoundary(vtkImageData* image,
								  const Vec3& centroid,
								  const Vec3& axisDir,
								  double threshold,
								  Vec3& outWorld)
	{
		double out[3] = { centroid.GetX(), centroid.GetY(), centroid.GetZ() };
		findSurfacePointFromBoundary(image, centroid.GetData(), axisDir.GetData(), threshold, out);
		outWorld = Vec3(out);
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
			// Divide the voxel range into fixed-size chunks proportional to the
			// thread count.  Avoids allocating a nVox-element QVector<int> index
			// vector and replaces the per-voxel virtual GetTuple1 dispatch with a
			// direct typed pointer read (4-8× fewer cycles for float/int16 data).
			const void* rawScalars = reslicedImage->GetScalarPointer();
			const int   scalarType = reslicedImage->GetScalarType();

			const int nChunks = QThread::idealThreadCount() * 4;
			const int chunkSize = std::max(1, (nVox + nChunks - 1) / nChunks);

			QVector<QPair<int, int>> chunks;
			chunks.reserve(nChunks);
			for (int s = 0; s < nVox; s += chunkSize)
				chunks.append({ s, std::min(s + chunkSize, nVox) });

			QtConcurrent::blockingMap(chunks,
				[&binary, rawScalars, scalarType, threshold, scalars]
				(const QPair<int, int>& range)
				{
					const int lo = range.first;
					const int hi = range.second;
					switch (scalarType)
					{
						case VTK_FLOAT:
						{
							const auto* p = static_cast<const float*>(rawScalars) + lo;
							for (int i = lo; i < hi; ++i, ++p)
								if (static_cast<double>(*p) >= threshold)
									binary[static_cast<std::size_t>(i)] = 1u;
							break;
						}
						case VTK_SHORT:
						{
							const auto* p = static_cast<const short*>(rawScalars) + lo;
							for (int i = lo; i < hi; ++i, ++p)
								if (static_cast<double>(*p) >= threshold)
									binary[static_cast<std::size_t>(i)] = 1u;
							break;
						}
						case VTK_UNSIGNED_SHORT:
						{
							const auto* p = static_cast<const unsigned short*>(rawScalars) + lo;
							for (int i = lo; i < hi; ++i, ++p)
								if (static_cast<double>(*p) >= threshold)
									binary[static_cast<std::size_t>(i)] = 1u;
							break;
						}
						case VTK_UNSIGNED_CHAR:
						{
							const auto* p = static_cast<const unsigned char*>(rawScalars) + lo;
							for (int i = lo; i < hi; ++i, ++p)
								if (static_cast<double>(*p) >= threshold)
									binary[static_cast<std::size_t>(i)] = 1u;
							break;
						}
						default:
						// Fallback: virtual dispatch for uncommon scalar types.
						for (int i = lo; i < hi; ++i)
							if (scalars->GetTuple1(i) >= threshold)
								binary[static_cast<std::size_t>(i)] = 1u;
						break;
					}
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

					// Flat-vector BFS queue: contiguous allocation avoids the
					// repeated small-block allocations made by std::deque (the
					// backing store of std::queue).  bfsHead advances without
					// popping so no elements are ever moved or freed mid-BFS.
					std::vector<std::array<vtkIdType, 3>> bfsQueue;
					bfsQueue.reserve(4096);
					bfsQueue.push_back({ ss.voxel[0], ss.voxel[1], ss.voxel[2] });
					result.valid = true;

					for (std::size_t bfsHead = 0; bfsHead < bfsQueue.size(); ++bfsHead)
					{
						const auto cur = bfsQueue[bfsHead];

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
								bfsQueue.push_back({ nx_, ny_, nz_ });
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
			const QAtomicInteger<quint8>* lmConst = labelMap.get();
			const quint8* remapConst = labelRemap.data();

			const int nChunks = QThread::idealThreadCount() * 4;
			const int chunkSize = std::max(1, (nVox + nChunks - 1) / nChunks);

			QVector<QPair<int, int>> chunks;
			chunks.reserve(nChunks);
			for (int s = 0; s < nVox; s += chunkSize)
				chunks.append({ s, std::min(s + chunkSize, nVox) });

			QtConcurrent::blockingMap(chunks,
				[lmConst, remapConst, outPtr](const QPair<int, int>& range)
				{
					const int lo = range.first;
					const int hi = range.second;
					for (int i = lo; i < hi; ++i)
					{
						const quint8 raw = lmConst[i].loadRelaxed();
						outPtr[i] = (raw == 0u)
							? 0u
							: remapConst[static_cast<std::size_t>(raw)];
					}
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
	// Orphan island identification — unseeded connected component labelling
	//
	// Pass 1  Build binary threshold mask                      O(N)
	// Pass 2  BFS flood-fill assigns a unique int component id
	//         to every 26-connected foreground region          O(N)
	// Pass 3  Walk seed world points; mark their component ids
	//         as seeded                                        O(seeds)
	// Pass 4  Write orphan mask: 1 where component is unseeded O(N)
	// -----------------------------------------------------------------------
	void identifyOrphanIslands(
		vtkImageData* reslicedImage,
		double threshold,
		const std::vector<std::array<double, 3>>& seedsWorld,
		vtkSmartPointer<vtkImageData>& outOrphanMask,
		const std::function<void(int)>& progressCb)
	{
		outOrphanMask = nullptr;

		if (!reslicedImage || seedsWorld.empty())
			return;

		const double* origin = reslicedImage->GetOrigin();
		const double* spacing = reslicedImage->GetSpacing();
		const int* dims = reslicedImage->GetDimensions();
		vtkDataArray* scalars = reslicedImage->GetPointData()->GetScalars();

		if (!scalars)
		{
			qWarning("identifyOrphanIslands: resliced image has no scalar data.");
			return;
		}

		if (progressCb) progressCb(0);

		const vtkIdType NX = dims[0];
		const vtkIdType NY = dims[1];
		const vtkIdType NZ = dims[2];
		const vtkIdType totalVoxels = NX * NY * NZ;

		const auto flatIdx =
			[NX, NY](vtkIdType x, vtkIdType y, vtkIdType z) -> vtkIdType
			{ return z * NY * NX + y * NX + x; };

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

		// ── Pass 1: binary threshold mask ────────────────────────────────
		std::vector<uint8_t> binary(static_cast<std::size_t>(totalVoxels), 0u);
		for (vtkIdType i = 0; i < totalVoxels; ++i)
			if (scalars->GetTuple1(i) >= threshold)
				binary[static_cast<std::size_t>(i)] = 1u;

		if (progressCb) progressCb(10);

		// ── Pass 2: unseeded BFS connected component labelling ────────────
		// -1 = background; >= 0 = component id.
		std::vector<int> componentMap(
			static_cast<std::size_t>(totalVoxels), -1);

		int nextComponent = 0;

		for (vtkIdType z = 0; z < NZ; ++z)
		{
			for (vtkIdType y = 0; y < NY; ++y)
			{
				for (vtkIdType x = 0; x < NX; ++x)
				{
					const std::size_t seed =
						static_cast<std::size_t>(flatIdx(x, y, z));

					if (binary[seed] == 0u || componentMap[seed] >= 0)
						continue;

					const int compId = nextComponent++;
					componentMap[seed] = compId;

					std::queue<std::array<vtkIdType, 3>> bfsQueue;
					bfsQueue.push({ x, y, z });

					while (!bfsQueue.empty())
					{
						const auto cur = bfsQueue.front();
						bfsQueue.pop();

						const vtkIdType cx = cur[0];
						const vtkIdType cy = cur[1];
						const vtkIdType cz = cur[2];

						for (const auto& off : offsets)
						{
							const vtkIdType nx_ = cx + off[0];
							const vtkIdType ny_ = cy + off[1];
							const vtkIdType nz_ = cz + off[2];

							if (nx_ < 0 || nx_ >= NX ||
								ny_ < 0 || ny_ >= NY ||
								nz_ < 0 || nz_ >= NZ)
								continue;

							const std::size_t nIdx =
								static_cast<std::size_t>(flatIdx(nx_, ny_, nz_));

							if (binary[nIdx] == 0u || componentMap[nIdx] >= 0)
								continue;

							componentMap[nIdx] = compId;
							bfsQueue.push({ nx_, ny_, nz_ });
						}
					}
				}
			}

			// Progress: BFS pass maps to [10, 70].
			if ((z & 0xF) == 0 && progressCb)
			{
				progressCb(10 + static_cast<int>(
					60.0 * (z + 1) / static_cast<double>(NZ)));
			}
		}

		if (progressCb) progressCb(70);

		qDebug("identifyOrphanIslands: %d connected foreground component(s) found.",
			nextComponent);

		// ── Pass 3: mark which components contain at least one seed ───────
		std::unordered_set<int> seededComponents;
		seededComponents.reserve(static_cast<std::size_t>(seedsWorld.size()));

		for (const auto& sw : seedsWorld)
		{
			double cont[3] = {};
			reslicedImage->TransformPhysicalPointToContinuousIndex(
				sw.data(), cont);

			const int ix = static_cast<int>(std::lround(cont[0]));
			const int iy = static_cast<int>(std::lround(cont[1]));
			const int iz = static_cast<int>(std::lround(cont[2]));

			if (ix < 0 || ix >= dims[0] ||
				iy < 0 || iy >= dims[1] ||
				iz < 0 || iz >= dims[2])
				continue;

			const int comp = componentMap[
				static_cast<std::size_t>(flatIdx(ix, iy, iz))];

			if (comp >= 0)
			{
				seededComponents.insert(comp);
				qDebug("identifyOrphanIslands: seed (%.2f,%.2f,%.2f)"
					   " -> component %d",
					   sw[0], sw[1], sw[2], comp);
			}
		}

		const int orphanComponentCount =
			nextComponent - static_cast<int>(seededComponents.size());

		qDebug("identifyOrphanIslands: %zu seeded component(s), "
			   "%d orphan component(s).",
			seededComponents.size(), orphanComponentCount);

		if (progressCb) progressCb(80);

		// ── Pass 4: build orphan mask ─────────────────────────────────────
		outOrphanMask = vtkSmartPointer<vtkImageData>::New();
		outOrphanMask->SetDimensions(dims);
		outOrphanMask->SetSpacing(spacing);
		outOrphanMask->SetOrigin(origin);
		outOrphanMask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

		auto* outPtr = static_cast<uint8_t*>(outOrphanMask->GetScalarPointer());

		vtkIdType orphanVoxels = 0;
		for (vtkIdType i = 0; i < totalVoxels; ++i)
		{
			const int comp = componentMap[static_cast<std::size_t>(i)];
			if (comp >= 0 && seededComponents.count(comp) == 0)
			{
				outPtr[i] = 1u;
				++orphanVoxels;
			}
			else
			{
				outPtr[i] = 0u;
			}
		}

		qDebug("identifyOrphanIslands: %lld orphan voxel(s) written to mask.",
			static_cast<long long>(orphanVoxels));

		if (progressCb) progressCb(100);
	}

	std::vector<std::array<double, 3>> computeInwardAdjustedSeeds(
	vtkImageData* image,
	double threshold,
	const std::vector<std::array<double, 3>>& originalSeedsWorld,
	const PcaResult& pca)
	{
		std::vector<std::array<double, 3>> adjusted = originalSeedsWorld;

		if (!image || originalSeedsWorld.empty() || !pca.valid)
			return adjusted;

		const int nSeeds = static_cast<int>(originalSeedsWorld.size());

		for (int s = 0; s < nSeeds; ++s)
		{
			// Keep the existing seed packing convention:
			// axis = s / 2, tip = s % 2 (0 = positive, 1 = negative).
			const int axis = s / 2;
			const int tip = s % 2;

			// Inward direction toward the centroid:
			//   positive tip sits at centroid + R * axes[axis] -> inward = -axes[axis]
			//   negative tip sits at centroid - R * axes[axis] -> inward = +axes[axis]
			const double sign = (tip == 0) ? -1.0 : +1.0;
			const Vec3d inward(
				sign * pca.axes[axis][0],
				sign * pca.axes[axis][1],
				sign * pca.axes[axis][2]);

			const std::array<double, 3> start = originalSeedsWorld[static_cast<std::size_t>(s)];
			const std::array<double, 3> end =
			{
				pca.centroid[0],
				pca.centroid[1],
				pca.centroid[2]
			};

			// VoxelLine defines the segment; VoxelLineIterator walks it.
			VoxelLine line(image, start, end);
			bool found = false;
			for (VoxelLineIterator it = line.begin(); it != line.end(); ++it)
			{
				// Use the iterator's scalar helpers for the threshold test.
				if (!it.IsAbove(threshold))
					continue;

				const auto world = it.ToWorld();
				adjusted[static_cast<std::size_t>(s)] =
				{
					world[0],
					world[1],
					world[2]
				};

				qDebug("computeInwardAdjustedSeeds: seed %d adjusted to "
					   "(%.2f, %.2f, %.2f)",
					   s, world[0], world[1], world[2]);

				found = true;
				break;
			}

			if (!found)
			{
				qDebug("computeInwardAdjustedSeeds: seed %d — no qualifying voxel found "
					   "toward centroid; original position retained.", s);
			}
		}

		return adjusted;
	}
}
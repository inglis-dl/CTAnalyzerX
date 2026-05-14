#include "PrototypeHelpers.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkColorTransferFunction.h>
#include <vtkDiscreteFlyingEdges3D.h>
#include <vtkImageContinuousDilate3D.h>
#include <vtkImageContinuousErode3D.h>
#include <vtkImageData.h>
#include <vtkImageGaussianSmooth.h>
#include <vtkImageThresholdConnectivity.h>
#include <vtkLine.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRegularPolygonSource.h>
#include <vtkScalarBarActor.h>
#include <vtkSphereSource.h>
#include <vtkTextProperty.h>

#include <QJsonArray>

// ITK headers required by segmentBoneIslandsGraphCut
#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
#include <QJsonObject>
#include <vtkImageShiftScale.h>
#include <itkImage.h>
#include <itkVTKImageToImageFilter.h>
#include <itkImageToVTKImageFilter.h>
#include <itkBinaryThresholdImageFilter.h>
#include <itkConnectedComponentImageFilter.h>
#include <itkRelabelComponentImageFilter.h>

#include "GraphCut.h" // selects GridCut or Kolmogorov backend
#endif // CTAXPROTOTYPE_ENABLE_GRAPH_CUT

namespace PrototypeHelpers
{
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

	std::vector<ProcessHelpers::BoneIsland> segmentBoneIslandsAlternate(
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

		std::vector<ProcessHelpers::BoneIsland> islands;
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

			ProcessHelpers::BoneIsland island;
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

#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
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

	std::vector<ProcessHelpers::BoneIsland> segmentBoneIslandsGraphCut(
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

		std::vector<ProcessHelpers::BoneIsland> islands;
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

			ProcessHelpers::BoneIsland island;
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

#endif // CTAXPROTOTYPE_ENABLE_GRAPH_CUT

} // namespace PrototypeHelpers
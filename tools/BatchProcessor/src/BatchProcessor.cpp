#include "BatchProcessor.h"
#include "ImageLoader.h"

#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>
#include <QtConcurrent/QtConcurrent>

#include <vtkDataArray.h>
#include <vtkExtractVOI.h>
#include <vtkImageContinuousDilate3D.h>
#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkMatrix3x3.h>
#include <vtkMatrix4x4.h>
#include <vtkNIFTIImageWriter.h>
#include <vtkPointData.h>
#include <vtkTransform.h>

#include <functional>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {


    // ---------------------------------------------------------------------------
    // pcaResultToJson  —  mirrors PrototypeMainWindow::pcaResultToJson()
    // ---------------------------------------------------------------------------
    QJsonObject pcaResultToJson(const ProcessHelpers::PcaResult& pca)
    {
        auto packVec3 = [](const double v[3]) -> QJsonArray
            {
                return QJsonArray{ v[0], v[1], v[2] };
            };

        QJsonArray axesArray;
        for (int i = 0; i < 3; ++i)
        {
            QJsonObject axisObj;
            axisObj[QStringLiteral("index")] = i;
            axisObj[QStringLiteral("eigenvalue")] = pca.eigenvalues[i];
            axisObj[QStringLiteral("direction")] = packVec3(pca.axes[i]);
            axesArray.append(axisObj);
        }

        QJsonObject obj;
        obj[QStringLiteral("centroid")] = packVec3(pca.centroid);
        obj[QStringLiteral("circumRadius")] = pca.circumRadius;
        obj[QStringLiteral("axes")] = axesArray;
        return obj;
    }

    // ---------------------------------------------------------------------------
    // IterationHelper
    //
    // Headless equivalent of IterationProgressDialog.  Runs one Auto pass:
    //   1. runIterations() at baseline settings.
    //   2. If refinement is available (target island count reached, ≥ 2 rows),
    //      applyRefinement() then runIterations() once more.
    // ---------------------------------------------------------------------------
    class IterationHelper
    {
    public:
        using IterateFunc = std::function<
            std::vector<ProcessHelpers::BoneIsland>(double threshold, bool firstIteration)>;
        using ResetFunc = std::function<void()>;

        explicit IterationHelper(
            double baselineThreshold,
            double regionMean,
            double regionStdDev,
            double regionVolumeMm3,
            const double voxelSpacing[3])
            : m_baseThreshold(baselineThreshold)
            , m_baseStdDev(regionStdDev)
            , m_voxelVolMm3(voxelSpacing[0] * voxelSpacing[1] * voxelSpacing[2])
            , m_multiplier(0.1)
            , m_startThreshold(baselineThreshold)
            , m_finalThreshold(baselineThreshold)
            , m_volumeThreshold(1.0)
            , m_totalIterationsRun(0)
            , m_refinementApplied(false)
            , m_refinementAvailable(false)
        {
        }

        void setIterateCallback(IterateFunc fn) { m_iterateFunc = std::move(fn); }
        void setResetCallback(ResetFunc fn) { m_resetFunc = std::move(fn); }

        // Returns the threshold at which the final iteration stopped.
        // Returns the baseline threshold if no iterations ran.
        double finalThreshold() const
        {
            return m_table.empty() ? m_baseThreshold : m_table.back().threshold;
        }

        // Returns the total number of iterations that completed across all
        // runIterations() passes (initial + optional refinement pass).
        int totalIterations() const { return m_totalIterationsRun; }

        // Public entry point — mirrors IterationProgressDialog::runAuto().
        void runAuto()
        {
            if (!m_iterateFunc)
                return;

            m_table.clear();
            runIterations();

            if (m_refinementAvailable)
            {
                applyRefinement();
                runIterations();
            }
        }

    private:
        
        void runIterations()
        {
            if (!m_iterateFunc)
                return;

            // When a custom start threshold is active (e.g. set via applyRefinement)
            // call updateFinalThreshold so the final-threshold label stays accurate.
            if (m_refinementApplied)
                updateFinalThreshold(20, m_multiplier);

            const int    maxIter = 20;
            const double multiplier = m_multiplier;
            const double startThreshold = effectiveStartThreshold();
            const int    maxIslands = 2;

            // Clear per-run data so a second run starts from a clean table.
            m_iterationThresholds.clear();
            m_iterationVolumes.clear();
            m_table.clear();

            bool targetReached = false;
            int  targetRowM = -1;

            for (int iter = 1; iter <= maxIter; ++iter)
            {
                const double threshold =
                    startThreshold
                    + static_cast<double>(iter) * multiplier * m_baseStdDev;

                const auto islands = m_iterateFunc(threshold, iter == 1);

                if (islands.empty())
                {
                    qDebug("IterationHelper: iter=%d — no islands; stopping.", iter);
                    break;
                }

                double totalVoxels = 0.0;
                for (const auto& isl : islands)
                    totalVoxels += static_cast<double>(isl.voxelCount);
                const double volumeMm3x1k = totalVoxels * m_voxelVolMm3 * 1000.0;

                appendIterationRow(iter, threshold, volumeMm3x1k, islands);

                if (static_cast<int>(islands.size()) >= maxIslands)
                {
                    qDebug("IterationHelper: iter=%d — target island count %d reached "
                           "(%zu islands); stopping.",
                           iter, maxIslands, islands.size());

                    targetReached = true;
                    targetRowM = static_cast<int>(m_table.size()) - 1;
                    break;
                }
            }

            // Post-loop convergence check.
            bool converged = false;
            if (targetReached
                && targetRowM >= 1
                && targetRowM < static_cast<int>(m_iterationVolumes.size()))
            {
                const double volChange = std::abs(
                    m_iterationVolumes[static_cast<std::size_t>(targetRowM)]
                    - m_iterationVolumes[static_cast<std::size_t>(targetRowM - 1)]);

                if (volChange <= m_volumeThreshold)
                {
                    qDebug("IterationHelper: converged — volume change %.4f <= threshold %.4f.",
                           volChange, m_volumeThreshold);
                    converged = true;
                }
            }

            // Refinement is available when the target was reached, there are at least
            // two rows to bracket, and the volume change is above the threshold.
            m_refinementAvailable = targetReached && targetRowM >= 1 && !converged;

            // Accumulate the count across passes so totalIterations() reflects
            // both the initial run and any subsequent refinement pass.
            m_totalIterationsRun += static_cast<int>(m_table.size());
        }

        void appendIterationRow(int iter, double threshold, double volumeMm3x1k,
            const std::vector<ProcessHelpers::BoneIsland>& islands)
        {
            m_iterationThresholds.push_back(threshold);
            m_iterationVolumes.push_back(volumeMm3x1k);

            tableRow row;
            row.iter = iter;
            row.threshold = threshold;
            row.volume = volumeMm3x1k;
            row.nisland = static_cast<int>(islands.size());
            m_table.push_back(row);
        }

        void applyRefinement()
        {
            if (m_table.empty() || m_iterationThresholds.empty())
                return;

            const int nRow = static_cast<int>(m_table.size());
            const int minRow = nRow - 2;
            const int maxRow = nRow - 1;

            const int nData = static_cast<int>(m_iterationThresholds.size());
            if (minRow >= maxRow || minRow < 0 || maxRow >= nData)
                return;

            const double threshMin = m_iterationThresholds[static_cast<std::size_t>(minRow)];
            const double threshMax = m_iterationThresholds[static_cast<std::size_t>(maxRow)];
            const int    iters = 20;

            m_startThreshold = threshMin;
            m_refinementApplied = true;

            if (iters >= 2 && m_baseStdDev > 0.0)
            {
                const double newMultiplier =
                    (threshMax - threshMin)
                    / (static_cast<double>(iters - 1) * m_baseStdDev);

                m_multiplier = newMultiplier;
            }

            updateFinalThreshold(iters, m_multiplier);
        }

        double effectiveStartThreshold() const
        {
            return m_refinementApplied ? m_startThreshold : m_baseThreshold;
        }

        void updateFinalThreshold(int iterations, double multiplier)
        {
            m_finalThreshold = effectiveStartThreshold()
                + static_cast<double>(iterations) * multiplier * m_baseStdDev;
        }

        struct tableRow {
            int    iter = -1;
            double threshold = 0.0;
            int    nisland = 0;
            double volume = 0.0;
        };

        std::vector<tableRow>   m_table;
        double                  m_multiplier;
        double                  m_startThreshold;
        double                  m_finalThreshold;
        double                  m_baseThreshold;
        double                  m_baseStdDev;
        double                  m_voxelVolMm3;
        double                  m_volumeThreshold;
        int                     m_totalIterationsRun;
        bool                    m_refinementApplied;
        bool                    m_refinementAvailable;
        IterateFunc             m_iterateFunc;
        ResetFunc               m_resetFunc;
        std::vector<double>     m_iterationThresholds;
        std::vector<double>     m_iterationVolumes;
    };
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

BatchProcessor::BatchProcessor()
    : m_imageLoader(vtkSmartPointer<ImageLoader>::New())
{
}

BatchProcessor::~BatchProcessor() = default;

// ---------------------------------------------------------------------------
// processSidecarFile
// ---------------------------------------------------------------------------

ProcessingRunResult BatchProcessor::processSidecarFile(
    const QString& sidecarPath,
    const QString& outputFolderPath,
    CropLoadedCallback    onCropLoaded,
    StageProgressCallback onStageAdvanced)
{
    // Convenience: invoke onStageAdvanced only if it was provided.
    auto reportStage = [&onStageAdvanced](ProcessingStage stage)
        {
            if (onStageAdvanced)
                onStageAdvanced(stage);
        };

    ProcessingRunResult result;
    result.inputRootDir = QFileInfo(sidecarPath).absolutePath();
    result.sidecarBasename = QFileInfo(sidecarPath).baseName();

    reportStage(ProcessingStage::Load);
    if (!loadSidecarAndImage(sidecarPath))
    {
        qWarning() << "Failed to load sidecar or image for:" << sidecarPath;
        return result;
    }

    result.cropBasename = QFileInfo(m_cropPath).baseName();
    if (onCropLoaded)
        onCropLoaded(result.cropBasename);

    result.baselineOtsuThreshold = m_baselineOtsuThreshold;

    QString actualOutputDir = outputFolderPath.isEmpty()
        ? QFileInfo(sidecarPath).absolutePath()
        : outputFolderPath;

    QDir outputDirCreator(actualOutputDir);
    if (!outputDirCreator.exists() && !outputDirCreator.mkpath("."))
    {
        qWarning() << "Failed to create output directory:" << actualOutputDir;
        return result;
    }

    reportStage(ProcessingStage::Reslice);
    performReslicing();

    reportStage(ProcessingStage::Landmark);
    performLandmarking();

    reportStage(ProcessingStage::Segment);
    performSegmentation();

    reportStage(ProcessingStage::Clean);
    performClean();

    reportStage(ProcessingStage::Export);
    performExport(actualOutputDir);

    result.finalThreshold = m_finalSegmentationThreshold;
    result.totalIterations = m_iterationCount;
    result.segmentedBoneVolumeMm3 = computeSegmentedBoneVolume(
        m_reslicedImage ? m_reslicedImage.Get() : m_image.Get());

    reportStage(ProcessingStage::Write);
    const QString outputPath = QDir(actualOutputDir).filePath(
        QFileInfo(sidecarPath).baseName() + QStringLiteral("_processed.json"));

    if (!writeOutputSidecar(outputPath))
    {
        qWarning() << "Failed to write output sidecar for:" << sidecarPath;
        return result;
    }

    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// computeSegmentedBoneVolume
// ---------------------------------------------------------------------------

double BatchProcessor::computeSegmentedBoneVolume(vtkImageData* spacingRef) const
{
    if (!spacingRef || m_islands.empty())
        return 0.0;

    const auto largestIt = std::max_element(
        m_islands.begin(), m_islands.end(),
        [](const ProcessHelpers::BoneIsland& a, const ProcessHelpers::BoneIsland& b)
        { return a.voxelCount < b.voxelCount; });

    double sp[3];
    spacingRef->GetSpacing(sp);
    const double voxelVolume = sp[0] * sp[1] * sp[2]; // mm³ per voxel

    return static_cast<double>(largestIt->voxelCount) * voxelVolume;
}

// ---------------------------------------------------------------------------
// loadSidecarAndImage  —  adapted from PrototypeMainWindow::loadFromSidecar()
// ---------------------------------------------------------------------------

bool BatchProcessor::loadSidecarAndImage(const QString& sidecarPath)
{
    if (!QFileInfo::exists(sidecarPath))
    {
        qWarning() << "Could not open sidecar file for reading:" << sidecarPath;
        return false;
    }

    const QJsonObject sidecar = ProcessHelpers::readJsonObjectFileOrThrow(sidecarPath);
    const QString     cropPath = ProcessHelpers::cropPathFromSidecarOrThrow(sidecar);

    if (!QFileInfo::exists(cropPath))
    {
        qWarning() << "Crop output does not exist at path:" << cropPath;
        return false;
    }

    const double threshold = ProcessHelpers::thresholdFromSidecar(sidecar);
    if (!std::isfinite(threshold))
    {
        qWarning() << "No finite threshold found in sidecar. "
            "Threshold-based steps will be skipped for:" << sidecarPath;
        return false;
    }

    m_imageLoader->SetInputPath(cropPath);
    m_imageLoader->SetImageType(ImageLoader::ImageType::NIFTI);
    m_imageLoader->Update();

    auto out = m_imageLoader->GetOutput();
    if (!out)
    {
        qWarning() << "ImageLoader returned null output for:" << cropPath;
        return false;
    }

    const int* dims = out->GetDimensions();
    if (dims[0] <= 1 || dims[1] <= 1 || dims[2] <= 1)
    {
        qWarning() << "ImageLoader produced invalid volume dimensions for:"
            << cropPath
            << "Dimensions:" << dims[0] << "x" << dims[1] << "x" << dims[2];
        return false;
    }

    m_threshold = threshold;
    m_baselineOtsuThreshold = threshold;
    m_sidecarPath = sidecarPath;
    m_cropPath = cropPath;

    setImage(out);
    return true;
}

// ---------------------------------------------------------------------------
// setImage
// ---------------------------------------------------------------------------

void BatchProcessor::setImage(vtkImageData* image)
{
    m_originalImage = vtkSmartPointer<vtkImageData>::New();
    m_originalImage->DeepCopy(image);

    m_labelImage = nullptr;
    m_islands.clear();

    m_image = image;

    m_pca.valid = false;
    m_landmarkResult = QJsonObject{};
    m_landmarkPoints = {};

    m_imageStats = ProcessHelpers::computeScalarThresholdStats(m_image, m_threshold);
}

// ---------------------------------------------------------------------------
// performReslicing  —  adapted from PrototypeMainWindow::onReslice()
// ---------------------------------------------------------------------------

void BatchProcessor::performReslicing()
{
    // Step 1: compute PCA on the current (original) image.
    const bool ok = ProcessHelpers::computePca(m_image, m_threshold, m_pca, nullptr);
    if (!ok || !m_pca.valid)
    {
        qWarning() << "PCA computation failed during reslicing.";
        return;
    }

    // Cache the original-image PCA JSON (written once before reslice exists).
    if (!m_reslicedImage)
        m_originalPcaJson = pcaResultToJson(m_pca);

    // Step 2: build reslice axes from PCA result.
    m_lastResliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
    m_lastResliceAxes->Identity();

    for (int row = 0; row < 3; ++row)
    {
        m_lastResliceAxes->SetElement(row, 0, m_pca.axes[0][row]);
        m_lastResliceAxes->SetElement(row, 1, m_pca.axes[1][row]);
        m_lastResliceAxes->SetElement(row, 2, m_pca.axes[2][row]);
        m_lastResliceAxes->SetElement(row, 3, m_pca.centroid[row]);
    }

    const double bgMean =
        m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);

    auto reslice = vtkSmartPointer<vtkImageReslice>::New();
    reslice->SetInputData(m_image);
    reslice->SetResliceAxes(m_lastResliceAxes);
    reslice->SetInterpolationModeToCubic();
    reslice->AutoCropOutputOn();
    reslice->SetOutputDimensionality(3);
    reslice->SetBackgroundLevel(bgMean);
    reslice->SetNumberOfThreads(QThread::idealThreadCount());
    reslice->Update();

    if (!reslice->GetOutput())
    {
        qWarning() << "vtkImageReslice produced null output.";
        return;
    }

    m_reslicedImage = vtkSmartPointer<vtkImageData>::New();
    m_reslicedImage->DeepCopy(reslice->GetOutput());

    // Step 3: update m_image to the resliced volume and recompute statistics
    // and PCA so that performLandmarking() operates on the correct image —
    // mirrors PrototypeMainWindow::onReslice() → setImage(m_reslicedImage).
    m_image = m_reslicedImage;
    m_imageStats = ProcessHelpers::computeScalarThresholdStats(m_image, m_threshold);

    ProcessHelpers::PcaResult reslicedPca;
    if (ProcessHelpers::computePca(m_image, m_threshold, reslicedPca, nullptr)
        && reslicedPca.valid)
    {
        m_pca = reslicedPca;
        m_reslicedPcaJson = pcaResultToJson(reslicedPca);
        qDebug("performReslicing: resliced PCA cached.");
    }
    else
    {
        qWarning() << "PCA recomputation on resliced image failed.";
    }
}

// ---------------------------------------------------------------------------
// performLandmarking  —  adapted from PrototypeMainWindow::onLandmark()
// ---------------------------------------------------------------------------

void BatchProcessor::performLandmarking()
{
    if (!m_pca.valid)
    {
        qWarning("performLandmarking: no valid PCA result; run performReslicing first.");
        return;
    }
    if (!m_image)
    {
        qWarning("performLandmarking: no image cached.");
        return;
    }
    if (!std::isfinite(m_threshold))
    {
        qWarning("performLandmarking: threshold is not finite.");
        return;
    }

    QJsonArray jsonLandmarks;

    auto packVec3 = [](const double v[3]) -> QJsonArray
        {
            return QJsonArray{ v[0], v[1], v[2] };
        };

    for (int i = 0; i < 3; ++i)
    {
        const double axisDirPos[3] = { m_pca.axes[i][0],  m_pca.axes[i][1],  m_pca.axes[i][2] };
        const double axisDirNeg[3] = { -m_pca.axes[i][0], -m_pca.axes[i][1], -m_pca.axes[i][2] };

        ProcessHelpers::findSurfacePointFromBoundary(
            m_image, m_pca.centroid, axisDirPos, m_threshold,
            m_landmarkPoints[static_cast<std::size_t>(i)][0].data());
        ProcessHelpers::findSurfacePointFromBoundary(
            m_image, m_pca.centroid, axisDirNeg, m_threshold,
            m_landmarkPoints[static_cast<std::size_t>(i)][1].data());

        const double* lPos = m_landmarkPoints[static_cast<std::size_t>(i)][0].data();
        const double* lNeg = m_landmarkPoints[static_cast<std::size_t>(i)][1].data();

        QJsonObject axisObj;
        axisObj[QStringLiteral("index")] = i;
        axisObj[QStringLiteral("eigenvalue")] = m_pca.eigenvalues[i];
        axisObj[QStringLiteral("eigenvector")] = packVec3(m_pca.axes[i]);
        axisObj[QStringLiteral("landmarkPos")] = packVec3(lPos);
        axisObj[QStringLiteral("landmarkNeg")] = packVec3(lNeg);
        jsonLandmarks.append(axisObj);
    }

    m_landmarkResult = QJsonObject{};
    m_landmarkResult[QStringLiteral("centroid")] = packVec3(m_pca.centroid);
    m_landmarkResult[QStringLiteral("circumRadius")] = m_pca.circumRadius;
    m_landmarkResult[QStringLiteral("threshold")] = m_threshold;
    m_landmarkResult[QStringLiteral("axes")] = jsonLandmarks;

    m_landmarkJson = QJsonObject{};
    m_landmarkJson[QStringLiteral("sourceSidecar")] = m_sidecarPath;
    m_landmarkJson[QStringLiteral("cropImage")] = m_cropPath;
    m_landmarkJson[QStringLiteral("threshold")] = m_threshold;

    if (!m_originalPcaJson.isEmpty())
        m_landmarkJson[QStringLiteral("originalPca")] = m_originalPcaJson;
    if (!m_reslicedPcaJson.isEmpty())
        m_landmarkJson[QStringLiteral("reslicedPca")] = m_reslicedPcaJson;

    m_landmarkJson[QStringLiteral("landmarks")] = m_landmarkResult;
}

// ---------------------------------------------------------------------------
// performSegmentation  —  adapted from PrototypeMainWindow::onRegions()
// ---------------------------------------------------------------------------

void BatchProcessor::performSegmentation()
{
    if (!m_reslicedImage)
    {
        qWarning("performSegmentation: no resliced image; run performReslicing first.");
        return;
    }
    if (m_landmarkResult.isEmpty())
    {
        qWarning("performSegmentation: no landmark points; run performLandmarking first.");
        return;
    }
    if (!std::isfinite(m_threshold))
    {
        qWarning("performSegmentation: threshold is not finite.");
        return;
    }

    // Collect the 6 landmark world-space seed points.
    std::vector<std::array<double, 3>> seeds;
    seeds.reserve(6);
    for (int i = 0; i < 3; ++i)
        for (int d = 0; d < 2; ++d)
        {
            const double* pt =
                m_landmarkPoints[static_cast<std::size_t>(i)]
                [static_cast<std::size_t>(d)].data();
            seeds.push_back({ pt[0], pt[1], pt[2] });
        }

    // Initial region grow at baseline threshold.
    qDebug("performSegmentation: initial region grow  threshold=%.4f", m_threshold);

    vtkSmartPointer<vtkImageData> labelImage;
    std::vector<ProcessHelpers::BoneIsland> islands =
        ProcessHelpers::segmentBoneIslandsParallel(
            m_reslicedImage, m_threshold, seeds, labelImage, nullptr);

    if (islands.empty())
    {
        qWarning("performSegmentation: no bone islands found at baseline threshold.");
        return;
    }

    applyIslandSegmentationResult(islands, labelImage);

    // Compute baseline stats for the iteration helper.
    const ProcessHelpers::RegionStats baseStats =
        ProcessHelpers::computeRegionStats(m_reslicedImage, labelImage);
    const double baseVolume =
        ProcessHelpers::computeRegionVolumeMm3(labelImage);

    qDebug("performSegmentation: baseline — mean=%.2f  stdDev=%.2f  volume=%.3f x10^-3 mm³",
           baseStats.mean, baseStats.stdDev, baseVolume * 1000.0);

    // Run the Auto iteration loop (mirrors IterationProgressDialog::runAuto()).
    IterationHelper helper(
        m_threshold,
        baseStats.mean,
        baseStats.stdDev,
        baseVolume,
        m_reslicedImage->GetSpacing());

    helper.setIterateCallback(
        [this, seeds](double threshold, bool firstIteration)
            -> std::vector<ProcessHelpers::BoneIsland>
        {
            const std::vector<std::array<double, 3>> adjustedSeeds =
                ProcessHelpers::computeInwardAdjustedSeeds(
                    m_reslicedImage, threshold, seeds, m_pca);

            vtkSmartPointer<vtkImageData> iterLabel;
            auto iterIslands = ProcessHelpers::segmentBoneIslandsParallel(
                m_reslicedImage, threshold, adjustedSeeds, iterLabel, nullptr);

            if (!iterIslands.empty())
                applyIslandSegmentationResult(iterIslands, iterLabel);

            // Incremental orphan accumulation — reset at the first iteration of
            // each Run so stale data from a prior pass is not carried forward.
            if (firstIteration)
                m_orphanMaskImage = nullptr;

            if (m_reslicedImage)
            {
                vtkSmartPointer<vtkImageData> iterOrphanMask;
                ProcessHelpers::identifyOrphanIslands(
                    m_reslicedImage, threshold, adjustedSeeds,
                    iterOrphanMask, nullptr);

                if (iterOrphanMask)
                {
                    if (!m_orphanMaskImage)
                    {
                        m_orphanMaskImage = iterOrphanMask;
                    }
                    else
                    {
                        const vtkIdType nVox = m_orphanMaskImage->GetNumberOfPoints();
                        if (iterOrphanMask->GetNumberOfPoints() != nVox)
                        {
                            qWarning("performSegmentation orphan OR-merge: "
                                     "dimension mismatch; skipping.");
                        }
                        else
                        {
                            auto* accPtr = static_cast<uint8_t*>(
                                m_orphanMaskImage->GetScalarPointer());
                            const auto* newPtr = static_cast<const uint8_t*>(
                                iterOrphanMask->GetScalarPointer());
                            for (vtkIdType i = 0; i < nVox; ++i)
                                accPtr[i] |= newPtr[i];
                            m_orphanMaskImage->Modified();
                        }
                    }
                }
            }

            return iterIslands;
        });

    helper.runAuto();
    m_iterationCount = helper.totalIterations();
    m_finalSegmentationThreshold = helper.finalThreshold();
}

// ---------------------------------------------------------------------------
// writeOutputSidecar
// ---------------------------------------------------------------------------

bool BatchProcessor::writeOutputSidecar(const QString& outputPath)
{
    qDebug() << "Writing output sidecar to:" << outputPath;

    if (m_landmarkJson.isEmpty())
    {
        qDebug("writeOutputSidecar: no landmark data to write; skipping.");
        return true;
    }

    QFile saveFile(outputPath);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        qWarning() << "Could not open output sidecar file for writing:" << outputPath;
        return false;
    }

    QJsonDocument saveDoc(m_landmarkJson);
    saveFile.write(saveDoc.toJson(QJsonDocument::Indented));
    return true;
}

// ---------------------------------------------------------------------------
// applyIslandSegmentationResult
// ---------------------------------------------------------------------------

void BatchProcessor::applyIslandSegmentationResult(
    const std::vector<ProcessHelpers::BoneIsland>& islands,
    vtkSmartPointer<vtkImageData> labelImage)
{
    m_labelImage = labelImage;
    m_islands = islands;
}

// ---------------------------------------------------------------------------
// applyIslandRetentionFilter
// ---------------------------------------------------------------------------

void BatchProcessor::applyIslandRetentionFilter(const QSet<int>& retainedLabels)
{
    Q_UNUSED(retainedLabels);
    // No VolumeView in batch mode — nothing to hide.
}

// ---------------------------------------------------------------------------
// applyInverseResliceToOriginal
//   Ports PrototypeMainWindow::applyInverseResliceToOriginal() verbatim.
//   Projects cleaned resliced voxels back into the original image space by
//   building the inverse of the IndexMatrix used during the forward reslice.
// ---------------------------------------------------------------------------

vtkSmartPointer<vtkImageData> BatchProcessor::applyInverseResliceToOriginal() const
{
    if (!m_reslicedImage || !m_originalImage ||
        !m_lastResliceAxes || !std::isfinite(m_threshold))
    {
        qWarning("applyInverseResliceToOriginal: pre-conditions not met.");
        return nullptr;
    }

    const int* origDims = m_originalImage->GetDimensions();
    const int* reslDims = m_reslicedImage->GetDimensions();

    vtkDataArray* origScalars = m_originalImage->GetPointData()->GetScalars();
    vtkDataArray* reslScalars = m_reslicedImage->GetPointData()->GetScalars();
    if (!origScalars || !reslScalars)
    {
        qWarning("applyInverseResliceToOriginal: scalar arrays missing.");
        return nullptr;
    }

    // Helpers that replicate the IndexMatrix element math from vtkImageReslice.cxx.
    auto buildOutMatrix = [](vtkImageData* img) -> vtkSmartPointer<vtkMatrix4x4>
        {
            const double* sp = img->GetSpacing();
            const double* org = img->GetOrigin();
            double dir[9];
            auto* dm = img->GetDirectionMatrix();
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    dir[3 * i + j] = dm->GetElement(i, j);

            auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
            mat->Zero();
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                    mat->SetElement(i, j, dir[3 * i + j] * sp[j]);
                mat->SetElement(i, 3, org[i]);
            }
            mat->SetElement(3, 3, 1.0);
            return mat;
        };

    auto buildInMatrix = [](vtkImageData* img) -> vtkSmartPointer<vtkMatrix4x4>
        {
            const double* sp = img->GetSpacing();
            const double* org = img->GetOrigin();
            double dir[9], invDir[9];
            auto* dm = img->GetDirectionMatrix();
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    dir[3 * i + j] = dm->GetElement(i, j);
            vtkMatrix3x3::Invert(dir, invDir);

            auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
            mat->Zero();
            for (int i = 0; i < 3; ++i)
            {
                double t = 0.0;
                for (int j = 0; j < 3; ++j)
                {
                    mat->SetElement(i, j, invDir[3 * i + j] / sp[i]);
                    t -= invDir[3 * i + j] * org[j] / sp[i];
                }
                mat->SetElement(i, 3, t);
            }
            mat->SetElement(3, 3, 1.0);
            return mat;
        };

    // origOutMatrix : original index → original physical
    const auto origOutMatrix = buildOutMatrix(m_originalImage);

    // invResliceAxes : original physical → resliced physical
    // Use m_lastResliceAxes (captured at reslice time) not the current m_pca.
    auto invResliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
    vtkMatrix4x4::Invert(m_lastResliceAxes, invResliceAxes);

    // reslInMatrix : resliced physical → resliced index
    const auto reslInMatrix = buildInMatrix(m_reslicedImage);

    // newIndexMatrix = reslInMatrix × invResliceAxes × origOutMatrix
    auto xform = vtkSmartPointer<vtkTransform>::New();
    xform->SetMatrix(invResliceAxes);
    xform->PreMultiply();
    xform->Concatenate(origOutMatrix);
    xform->PostMultiply();
    xform->Concatenate(reslInMatrix);

    auto newIndexMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
    xform->GetMatrix(newIndexMatrix);

    // Deep-copy the original; selectively overwrite cleaned bone voxels.
    auto output = vtkSmartPointer<vtkImageData>::New();
    output->DeepCopy(m_originalImage);
    vtkDataArray* outScalars = output->GetPointData()->GetScalars();

    const int rNX = reslDims[0];
    const int rNY = reslDims[1];
    const int rNZ = reslDims[2];

    vtkIdType replaced = 0;
    vtkIdType outOfRange = 0;

    double origIdx[4] = { 0.0, 0.0, 0.0, 1.0 };
    double reslContIdx[4] = {};

    for (int k = 0; k < origDims[2]; ++k)
    {
        origIdx[2] = static_cast<double>(k);
        for (int j = 0; j < origDims[1]; ++j)
        {
            origIdx[1] = static_cast<double>(j);
            for (int i = 0; i < origDims[0]; ++i)
            {
                const vtkIdType origFlat =
                    static_cast<vtkIdType>(k) * origDims[1] * origDims[0]
                    + static_cast<vtkIdType>(j) * origDims[0]
                    + i;

                if (origScalars->GetTuple1(origFlat) < m_threshold)
                    continue;

                origIdx[0] = static_cast<double>(i);
                newIndexMatrix->MultiplyPoint(origIdx, reslContIdx);

                const int ri = static_cast<int>(std::lround(reslContIdx[0]));
                const int rj = static_cast<int>(std::lround(reslContIdx[1]));
                const int rk = static_cast<int>(std::lround(reslContIdx[2]));

                if (ri < 0 || ri >= rNX ||
                    rj < 0 || rj >= rNY ||
                    rk < 0 || rk >= rNZ)
                {
                    ++outOfRange;
                    continue;
                }

                const vtkIdType reslFlat =
                    static_cast<vtkIdType>(rk) * rNY * rNX
                    + static_cast<vtkIdType>(rj) * rNX
                    + ri;

                const double reslVal = reslScalars->GetTuple1(reslFlat);
                if (reslVal < m_threshold)
                {
                    outScalars->SetTuple1(origFlat, reslVal);
                    ++replaced;
                }
            }
        }
    }

    outScalars->Modified();
    output->Modified();

    qDebug("applyInverseResliceToOriginal: %lld voxel(s) replaced  "
           "%lld out-of-reslice-range.",
           static_cast<long long>(replaced),
           static_cast<long long>(outOfRange));

    return output;
}

// ---------------------------------------------------------------------------
// performClean  —  adapted from PrototypeMainWindow::onClean()
// ---------------------------------------------------------------------------

void BatchProcessor::performClean()
{
    qDebug("performClean: island-targeted clean triggered.");

    if (!m_reslicedImage)
    {
        qWarning("performClean: no resliced image available.");
        return;
    }
    if (!m_labelImage)
    {
        qWarning("performClean: no label image; run performSegmentation first.");
        return;
    }
    if (m_islands.empty())
    {
        qWarning("performClean: no segmented islands available.");
        return;
    }
    if (!std::isfinite(m_threshold))
    {
        qWarning("performClean: threshold is not finite.");
        return;
    }

    // Retain the largest island; remove all others.
    const int largestLabel =
        std::max_element(m_islands.begin(), m_islands.end(),
            [](const ProcessHelpers::BoneIsland& a,
        const ProcessHelpers::BoneIsland& b)
            { return a.voxelCount < b.voxelCount; })->label;

    std::vector<int> labelsToRemove;
    for (const auto& isl : m_islands)
        if (isl.label != largestLabel)
            labelsToRemove.push_back(isl.label);

    const int nDilations = 1;

    if (labelsToRemove.empty())
    {
        qDebug("performClean: no islands selected for removal.");
        return;
    }

    const double* spacing = m_reslicedImage->GetSpacing();
    const double* origin = m_reslicedImage->GetOrigin();
    const int* dims = m_reslicedImage->GetDimensions();
    const vtkIdType totalVoxels =
        static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];

    vtkDataArray* labelScalars = m_labelImage->GetPointData()->GetScalars();
    vtkDataArray* reslicedScalars = m_reslicedImage->GetPointData()->GetScalars();

    if (!labelScalars || !reslicedScalars)
    {
        qWarning("performClean: scalar arrays missing.");
        return;
    }

    const double bgMean =
        m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);

    const std::unordered_set<int> removeSet(labelsToRemove.begin(), labelsToRemove.end());

    std::array<bool, 256> removeTable{};
    for (int lbl : labelsToRemove)
        if (lbl >= 0 && lbl < 256)
            removeTable[static_cast<std::size_t>(lbl)] = true;

    // Step 1: build combined binary mask (selected islands OR orphans).
    vtkDataArray* orphanScalars = nullptr;
    if (m_orphanMaskImage)
    {
        orphanScalars = m_orphanMaskImage->GetPointData()->GetScalars();
        qDebug("performClean: orphan mask available — merged into removal mask.");
    }

    auto maskImage = vtkSmartPointer<vtkImageData>::New();
    maskImage->SetDimensions(dims);
    maskImage->SetSpacing(spacing);
    maskImage->SetOrigin(origin);
    maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    const auto* labelPtr = static_cast<const unsigned char*>(m_labelImage->GetScalarPointer());
    const auto* orphanPtr = orphanScalars
        ? static_cast<const unsigned char*>(m_orphanMaskImage->GetScalarPointer())
        : nullptr;

    auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
    for (vtkIdType i = 0; i < totalVoxels; ++i)
    {
        const bool inIsland = removeTable[labelPtr[i]];
        const bool inOrphan = orphanPtr && orphanPtr[i] > 0u;
        maskPtr[i] = (inIsland || inOrphan) ? 1u : 0u;
    }
    maskImage->Modified();

    // Step 2: dilate the combined mask with a single (2N+1)³ kernel.
    const int kernelSide = 2 * nDilations + 1;

    auto dilateFilter = vtkSmartPointer<vtkImageContinuousDilate3D>::New();
    dilateFilter->SetInputData(maskImage);
    dilateFilter->SetKernelSize(kernelSide, kernelSide, kernelSide);
    dilateFilter->Update();

    auto dilatedMask = vtkSmartPointer<vtkImageData>::New();
    dilatedMask->DeepCopy(dilateFilter->GetOutput());

    vtkDataArray* dilatedScalars = dilatedMask->GetPointData()->GetScalars();
    if (!dilatedScalars)
    {
        qWarning("performClean: dilation produced no scalars.");
        return;
    }

    // Step 3: replace all dilated-mask voxels with background mean.
    vtkIdType replacedIsland = 0;
    vtkIdType replacedOrphan = 0;

    const auto* dilatedPtr = static_cast<const unsigned char*>(dilatedMask->GetScalarPointer());

    for (vtkIdType i = 0; i < totalVoxels; ++i)
    {
        if (dilatedPtr[i] == 0u)
            continue;

        reslicedScalars->SetTuple1(i, bgMean);

        const bool inIsland = removeTable[labelPtr[i]];
        const bool inOrphan = orphanPtr && orphanPtr[i] > 0u;
        if (inOrphan && !inIsland) ++replacedOrphan;
        else                       ++replacedIsland;
    }

    reslicedScalars->Modified();
    m_reslicedImage->Modified();

    qDebug("performClean: %lld island voxel(s) + %lld orphan voxel(s) replaced "
           "with background (%d dilation pass(es)).",
           static_cast<long long>(replacedIsland),
           static_cast<long long>(replacedOrphan),
           nDilations);

    m_orphanMaskImage = nullptr;

    // Step 4: update retention filter (no-op in headless mode).
    QSet<int> retainedLabels;
    for (const auto& isl : m_islands)
        if (!removeSet.count(isl.label))
            retainedLabels.insert(isl.label);

    applyIslandRetentionFilter(retainedLabels);

    m_image = m_reslicedImage;
}

// ---------------------------------------------------------------------------
// performExport  —  adapted from PrototypeMainWindow::onExport()
// ---------------------------------------------------------------------------

void BatchProcessor::performExport(const QString& outputPath)
{
    if (!m_reslicedImage || !m_originalImage ||
        !m_lastResliceAxes || !std::isfinite(m_threshold))
    {
        qWarning("performExport: pre-conditions not met; skipping.");
        return;
    }

    // Step 1: inverse-reslice cleaned image back into original space.
    const auto invResult = applyInverseResliceToOriginal();
    if (!invResult)
    {
        qWarning("performExport: inverse reslice failed; aborting.");
        return;
    }

    // Step 2a: PCA on the inverse-resliced result.
    ProcessHelpers::PcaResult exportPca;
    if (!ProcessHelpers::computePca(invResult, m_threshold, exportPca, nullptr)
        || !exportPca.valid)
    {
        qWarning("performExport: PCA on inverse-resliced image failed; aborting.");
        return;
    }

    // Step 2b: rotate the image using the PCA axes.
    auto resliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
    resliceAxes->Identity();
    for (int row = 0; row < 3; ++row)
    {
        resliceAxes->SetElement(row, 0, exportPca.axes[0][row]);
        resliceAxes->SetElement(row, 1, exportPca.axes[1][row]);
        resliceAxes->SetElement(row, 2, exportPca.axes[2][row]);
        resliceAxes->SetElement(row, 3, exportPca.centroid[row]);
    }

    const double bgMean =
        m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);

    auto resliceFilter = vtkSmartPointer<vtkImageReslice>::New();
    resliceFilter->SetInputData(invResult);
    resliceFilter->SetResliceAxes(resliceAxes);
    resliceFilter->SetInterpolationModeToCubic();
    resliceFilter->AutoCropOutputOn();
    resliceFilter->SetOutputDimensionality(3);
    resliceFilter->SetBackgroundLevel(bgMean);
    resliceFilter->SetNumberOfThreads(QThread::idealThreadCount());
    resliceFilter->Update();

    auto rotatedImage = resliceFilter->GetOutput();

    // Step 3b: recompute PCA on the rotated image.
    ProcessHelpers::PcaResult rotatedPca;
    if (!ProcessHelpers::computePca(rotatedImage, m_threshold, rotatedPca, nullptr)
        || !rotatedPca.valid)
    {
        qWarning("performExport: PCA on rotated image failed; aborting.");
        return;
    }

    // Step 4: find 6 surface landmark seeds on the rotated image.
    std::array<std::array<double, 3>, 3> landmarkPos;
    std::array<std::array<double, 3>, 3> landmarkNeg;

    for (int i = 0; i < 3; ++i)
    {
        const double axisDirPos[3] = {
            rotatedPca.axes[i][0], rotatedPca.axes[i][1], rotatedPca.axes[i][2]
        };
        const double axisDirNeg[3] = {
            -rotatedPca.axes[i][0], -rotatedPca.axes[i][1], -rotatedPca.axes[i][2]
        };

        ProcessHelpers::findSurfacePointFromBoundary(
            rotatedImage, rotatedPca.centroid, axisDirPos, m_threshold,
            landmarkPos[i].data());
        ProcessHelpers::findSurfacePointFromBoundary(
            rotatedImage, rotatedPca.centroid, axisDirNeg, m_threshold,
            landmarkNeg[i].data());
    }

    std::vector<std::array<double, 3>> seeds;
    seeds.reserve(6);
    for (int i = 0; i < 3; ++i)
    {
        seeds.push_back(landmarkPos[i]);
        seeds.push_back(landmarkNeg[i]);
    }

    // Step 5: region-grow from seeds at baseline threshold.
    vtkSmartPointer<vtkImageData> labelImage;
    const auto islands = ProcessHelpers::segmentBoneIslandsParallel(
        rotatedImage, m_threshold, seeds, labelImage, nullptr);

    if (!labelImage || islands.empty())
    {
        qWarning("performExport: region grow produced no usable result; aborting.");
        return;
    }

    const auto largestIslandIt = std::max_element(
        islands.begin(), islands.end(),
        [](const ProcessHelpers::BoneIsland& a,
        const ProcessHelpers::BoneIsland& b)
        { return a.voxelCount < b.voxelCount; });

    const int largestLabel = largestIslandIt->label;

    qDebug("performExport: %zu island(s) — largest label=%d  voxelCount=%lld",
           islands.size(),
           largestLabel,
           static_cast<long long>(largestIslandIt->voxelCount));

    // Steps 6+7 fused: binarize and find tight bounding box in one pass.
    int lblExtent[6];
    labelImage->GetExtent(lblExtent);

    const int lblNX = lblExtent[1] - lblExtent[0] + 1;
    const int lblNY = lblExtent[3] - lblExtent[2] + 1;
    const int lblNZ = lblExtent[5] - lblExtent[4] + 1;
    const vtkIdType totalVoxels =
        static_cast<vtkIdType>(lblNX) * lblNY * lblNZ;

    auto maskImage = vtkSmartPointer<vtkImageData>::New();
    maskImage->SetExtent(lblExtent);
    maskImage->SetSpacing(labelImage->GetSpacing());
    maskImage->SetOrigin(labelImage->GetOrigin());
    maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
    const auto* lblPtr = static_cast<const unsigned char*>(labelImage->GetScalarPointer());
    const auto  lbl8 = static_cast<unsigned char>(largestLabel);

    int  boundsMin[3] = { lblExtent[1], lblExtent[3], lblExtent[5] };
    int  boundsMax[3] = { lblExtent[0], lblExtent[2], lblExtent[4] };
    bool anyForeground = false;

    for (int k = lblExtent[4]; k <= lblExtent[5]; ++k)
        for (int j = lblExtent[2]; j <= lblExtent[3]; ++j)
            for (int i = lblExtent[0]; i <= lblExtent[1]; ++i)
            {
                const vtkIdType flat =
                    static_cast<vtkIdType>(k - lblExtent[4]) * lblNY * lblNX
                    + static_cast<vtkIdType>(j - lblExtent[2]) * lblNX
                    + (i - lblExtent[0]);

                if (lblPtr[flat] != lbl8)
                {
                    maskPtr[flat] = 0u;
                    continue;
                }

                maskPtr[flat] = 255u;
                anyForeground = true;
                if (i < boundsMin[0]) boundsMin[0] = i;
                if (j < boundsMin[1]) boundsMin[1] = j;
                if (k < boundsMin[2]) boundsMin[2] = k;
                if (i > boundsMax[0]) boundsMax[0] = i;
                if (j > boundsMax[1]) boundsMax[1] = j;
                if (k > boundsMax[2]) boundsMax[2] = k;
            }

    maskImage->Modified();

    if (!anyForeground)
    {
        qWarning("performExport: no foreground voxels in binarized mask; aborting.");
        return;
    }

    // Step 8: inflate bounds by 10-voxel margin, clamped to extent.
    constexpr int margin = 10;
    const int voiMinX = std::max(lblExtent[0], boundsMin[0] - margin);
    const int voiMinY = std::max(lblExtent[2], boundsMin[1] - margin);
    const int voiMinZ = std::max(lblExtent[4], boundsMin[2] - margin);
    const int voiMaxX = std::min(lblExtent[1], boundsMax[0] + margin);
    const int voiMaxY = std::min(lblExtent[3], boundsMax[1] + margin);
    const int voiMaxZ = std::min(lblExtent[5], boundsMax[2] + margin);

    // Determine output paths.
    QString grayPath, maskPath;
    QString targetDir;

    if (!outputPath.isEmpty())
    {
        // Use the explicitly provided output directory
        targetDir = outputPath;
    }
    else if (!m_sidecarPath.isEmpty() && !m_cropPath.isEmpty())
    {
        // Original logic: derive directory from m_sidecarPath if both are available
        targetDir = QFileInfo(m_sidecarPath).absolutePath();
    }
    else
    {
        // Original fallback: use temporary directory
        targetDir = QDir::temp().path();
    }

    // Determine base name for export files.
    QString cropBaseName;
    if (!m_cropPath.isEmpty())
    {
        cropBaseName = QFileInfo(m_cropPath).completeBaseName();
    }
    else
    {
        // Fallback for when m_cropPath is empty, matching original temp file naming convention
        cropBaseName = QStringLiteral("export");
    }

    grayPath = QDir(targetDir).filePath(cropBaseName + QStringLiteral("_export_grayscale.nii"));
    maskPath = QDir(targetDir).filePath(cropBaseName + QStringLiteral("_export_mask.nii"));

    // Steps 9+10: concurrent VOI extract and NIfTI write.
    auto futureGray = QtConcurrent::run(
        [resliceFilter,
         voiMinX, voiMaxX, voiMinY, voiMaxY, voiMinZ, voiMaxZ,
         grayPath]()
        {
            auto extract = vtkSmartPointer<vtkExtractVOI>::New();
            extract->SetInputConnection(resliceFilter->GetOutputPort());
            extract->SetVOI(voiMinX, voiMaxX, voiMinY, voiMaxY, voiMinZ, voiMaxZ);

            auto writer = vtkSmartPointer<vtkNIFTIImageWriter>::New();
            writer->SetInputConnection(extract->GetOutputPort());
            writer->SetFileName(grayPath.toUtf8().constData());
            writer->Write();
        });

    auto futureMask = QtConcurrent::run(
        [maskImage,
         voiMinX, voiMaxX, voiMinY, voiMaxY, voiMinZ, voiMaxZ,
         maskPath]()
        {
            auto extract = vtkSmartPointer<vtkExtractVOI>::New();
            extract->SetInputData(maskImage);
            extract->SetVOI(voiMinX, voiMaxX, voiMinY, voiMaxY, voiMinZ, voiMaxZ);

            auto writer = vtkSmartPointer<vtkNIFTIImageWriter>::New();
            writer->SetInputConnection(extract->GetOutputPort());
            writer->SetFileName(maskPath.toUtf8().constData());
            writer->Write();
        });

    futureGray.waitForFinished();
    futureMask.waitForFinished();

    qDebug("performExport: grayscale written to '%s'.", qUtf8Printable(grayPath));
    qDebug("performExport: mask written to '%s'.", qUtf8Printable(maskPath));
}
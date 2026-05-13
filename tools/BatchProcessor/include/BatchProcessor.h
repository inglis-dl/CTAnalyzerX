#pragma once

#include "BatchProcessorTypes.h"
#include "ProcessHelpers.h"

#include <QString>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QSet> // Needed for applyIslandRetentionFilter

#include <vtkSmartPointer.h>

#include <array>
#include <vector>
#include <limits>

class vtkImageData;
class vtkMatrix4x4;
class ImageLoader;

// ---------------------------------------------------------------------------
// ProcessingRunResult: populated by processSidecarFile for CSV reporting
// ---------------------------------------------------------------------------

struct ProcessingRunResult
{
    bool    success = false;
    QString inputRootDir;
    QString sidecarBasename;
    QString cropBasename;
    double  baselineOtsuThreshold = 0.0;
    double  finalThreshold = 0.0;
    int     totalIterations = 0;
    double  segmentedBoneVolumeMm3 = 0.0;
};

// Both the struct and the vector wrapper must be declared so Qt can
// copy them into the event queue for cross-thread queued connections.
Q_DECLARE_METATYPE(ProcessingRunResult)
Q_DECLARE_METATYPE(QVector<ProcessingRunResult>)

// ---------------------------------------------------------------------------
// BatchProcessor
// ---------------------------------------------------------------------------

class BatchProcessor
{
public:
    BatchProcessor();
    ~BatchProcessor();

    // Progress callback invoked once the crop path is known (after load),
    // providing the crop image basename so callers can update UI mid-file.
    using CropLoadedCallback = std::function<void(const QString& cropBasename)>;

    // Invoked synchronously on the calling thread at the start of each stage.
    using StageProgressCallback = std::function<void(ProcessingStage stage)>;

    // Main processing function for a single sidecar file.
    // outputFolderPath is optional; pass QString() to write beside the sidecar.
    ProcessingRunResult processSidecarFile(
        const QString& sidecarPath,
        const QString& outputFolderPath = QString(),
        CropLoadedCallback   onCropLoaded = nullptr,
        StageProgressCallback onStageAdvanced = nullptr);

private:
    // Adapted from PrototypeMainWindow for headless operation
    bool loadSidecarAndImage(const QString& sidecarPath);
    void setImage(vtkImageData* image);
    void performLandmarking();
    void performReslicing();
    void performSegmentation(); // Corresponds to onRegions()
    void performClean();
    void performExport(const QString& outputExportDir = QString());
    bool writeOutputSidecar(const QString& outputPath);

    // Helpers from PrototypeMainWindow (declarations only; implementation needs source)
    void applyIslandSegmentationResult(
        const std::vector<ProcessHelpers::BoneIsland>& islands,
        vtkSmartPointer<vtkImageData>                    labelImage);

    // Assuming applyIslandRetentionFilter is also an internal helper needed
    void applyIslandRetentionFilter(const QSet<int>& retainedLabels);
    vtkSmartPointer<vtkImageData> applyInverseResliceToOriginal() const;

    // Computes the segmented bone volume (mm^3) from the largest island
    // using the spacing of the provided image.
    double computeSegmentedBoneVolume(vtkImageData* spacingRef) const;

    // State members (minimal set from PrototypeMainWindow)
    QString m_sidecarPath;  // Current sidecar path being processed
    QString m_cropPath;
    double  m_threshold = std::numeric_limits<double>::quiet_NaN();
    double  m_baselineOtsuThreshold = 0.0;   // Read from source sidecar JSON
    double  m_finalSegmentationThreshold = 0.0;   // Set after iteration loop converges
    int     m_iterationCount = 0;      // Tracked by iterative threshold loop

    vtkSmartPointer<vtkImageData> m_image;         // current image after processing step
    vtkSmartPointer<vtkImageData> m_originalImage; // original loaded image
    vtkSmartPointer<ImageLoader>  m_imageLoader;   // Image loading component

    // Processing results
    std::array<std::array<std::array<double, 3>, 2>, 3> m_landmarkPoints{};
    QJsonObject m_landmarkResult;
    QJsonObject m_landmarkJson;
    ProcessHelpers::PcaResult m_pca;

    vtkSmartPointer<vtkImageData> m_reslicedImage;
    vtkSmartPointer<vtkImageData> m_labelImage;
    vtkSmartPointer<vtkImageData> m_orphanMaskImage;
    vtkSmartPointer<vtkMatrix4x4> m_lastResliceAxes;

    std::vector<ProcessHelpers::BoneIsland> m_islands;

    QJsonObject m_originalPcaJson;
    QJsonObject m_reslicedPcaJson;
    QJsonObject m_imageStats;
};

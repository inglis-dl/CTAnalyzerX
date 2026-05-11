#pragma once

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

class BatchProcessor
{
public:
    BatchProcessor();
    ~BatchProcessor();

    // Main processing function for a single sidecar file
    bool processSidecarFile(const QString& sidecarPath, const QString& outputFolderPath);

private:
    // Adapted from PrototypeMainWindow for headless operation
    bool loadSidecarAndImage(const QString& sidecarPath);
    void setImage(vtkImageData* image);
    void performLandmarking();
    void performReslicing();
    void performSegmentation(); // Corresponds to onRegions()
    void performClean();
    void performExport(const QString& outputPath);
    bool writeOutputSidecar(const QString& outputPath);

    // Helpers from PrototypeMainWindow (declarations only; implementation needs source)
    void applyIslandSegmentationResult(
        const std::vector<ProcessHelpers::BoneIsland>& islands,
        vtkSmartPointer<vtkImageData>                    labelImage);

    // Assuming applyIslandRetentionFilter is also an internal helper needed
    void applyIslandRetentionFilter(const QSet<int>& retainedLabels);
    vtkSmartPointer<vtkImageData> applyInverseResliceToOriginal() const;

    // State members (minimal set from PrototypeMainWindow)
    QString m_sidecarPath; // Current sidecar path being processed
    QString m_cropPath;
    double m_threshold = std::numeric_limits<double>::quiet_NaN();

    vtkSmartPointer<vtkImageData> m_image;         // current image after processing step
    vtkSmartPointer<vtkImageData> m_originalImage; // original loaded image
    vtkSmartPointer<ImageLoader> m_imageLoader; // Image loading component

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

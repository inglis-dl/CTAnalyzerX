#pragma once

#include <QString>
#include <QMetaType>

enum class ProcessingStage : int
{
    Load = 0,
    Reslice = 1,
    Landmark = 2,
    Segment = 3,
    Clean = 4,
    Export = 5,
    Write = 6,
    StageCount = 7
};

// Required for Qt queued connections that carry ProcessingStage
// across thread boundaries.
Q_DECLARE_METATYPE(ProcessingStage)

inline QString processingStageLabel(ProcessingStage stage)
{
    switch (stage)
    {
        case ProcessingStage::Load:     return QStringLiteral("Loading sidecar and image\u2026");
        case ProcessingStage::Reslice:  return QStringLiteral("Reslicing\u2026");
        case ProcessingStage::Landmark: return QStringLiteral("Landmarking\u2026");
        case ProcessingStage::Segment:  return QStringLiteral("Segmenting\u2026");
        case ProcessingStage::Clean:    return QStringLiteral("Cleaning\u2026");
        case ProcessingStage::Export:   return QStringLiteral("Exporting NIfTI\u2026");
        case ProcessingStage::Write:    return QStringLiteral("Writing sidecar\u2026");
        default:                        return QStringLiteral("Unknown stage");
    }
}

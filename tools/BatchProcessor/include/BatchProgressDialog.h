#pragma once

#include "BatchProcessorTypes.h"

#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>

// ---------------------------------------------------------------------------
// BatchProgressDialog
// Lightweight progress window shown when --verbose is passed on the CLI.
// ---------------------------------------------------------------------------

class BatchProgressDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BatchProgressDialog(int totalFiles, QWidget* parent = nullptr);

signals:
    // Emitted when the user clicks Abort.  The worker checks this after each
    // file and stops early, then emits allFinished with partial results.
    void abortRequested();

public slots:
    // Call before processing each file (sidecar name known immediately).
    void onFileStarted(int index, const QString& sidecarBasename);

    // Call once the crop path is resolved inside the processor.
    void onCropBasenameKnown(const QString& cropBasename);

    void onStageAdvanced(ProcessingStage stage);

    // Call after each file completes (success or failure).
    void onFileFinished(int index, bool success);

    // Call when all files are done (or aborted); enables Close, disables Abort.
    void onAllFinished(int processedCount, int errorCount);

private:
    QLabel* m_statusLabel;      // "Processing file X of N"
    QLabel* m_sidecarLabel;     // Current sidecar basename
    QLabel* m_cropLabel;        // Current crop image basename
    QProgressBar* m_progressBar;
    QLabel* m_stageLabel;
    QProgressBar* m_stageProgressBar;
    QPushButton* m_abortButton;      // Enabled while running; stops after current file
    QPushButton* m_closeButton;      // Enabled only after allFinished
    int           m_totalFiles;
};

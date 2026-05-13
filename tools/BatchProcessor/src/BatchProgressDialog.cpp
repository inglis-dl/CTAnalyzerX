#include "BatchProgressDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFont>

BatchProgressDialog::BatchProgressDialog(int totalFiles, QWidget* parent)
    : QDialog(parent)
    , m_totalFiles(totalFiles)
{
    setWindowTitle(QStringLiteral("CTAXBatchProcessor \u2014 Progress"));
    setMinimumWidth(500);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    m_statusLabel = new QLabel(QStringLiteral("Starting\u2026"), this);
    m_sidecarLabel = new QLabel(QStringLiteral("\u2014"), this);
    m_cropLabel = new QLabel(QStringLiteral("\u2014"), this);
    m_stageLabel = new QLabel(QStringLiteral("\u2014"), this);
    m_progressBar = new QProgressBar(this);
    m_stageProgressBar = new QProgressBar(this);
    m_abortButton = new QPushButton(QStringLiteral("Abort"), this);
    m_closeButton = new QPushButton(QStringLiteral("Close"), this);

    QFont bold = m_statusLabel->font();
    bold.setBold(true);
    m_statusLabel->setFont(bold);

    // Primary bar — overall file count
    m_progressBar->setRange(0, totalFiles);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat(QStringLiteral("%v / %m files"));

    // Secondary bar — stages within the current file
    const int stageCount = static_cast<int>(ProcessingStage::StageCount);
    m_stageProgressBar->setRange(0, stageCount);
    m_stageProgressBar->setValue(0);
    m_stageProgressBar->setTextVisible(false);

    m_stageLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_stageLabel->setFont(bold);

    // Abort is enabled while work is in progress; Close is disabled until done.
    m_abortButton->setEnabled(true);
    m_closeButton->setEnabled(false);

    // --- "Current file" group box ---
    auto* groupBox = new QGroupBox(QStringLiteral("Current file"), this);
    auto* groupLayout = new QVBoxLayout(groupBox);

    auto* sidecarRow = new QHBoxLayout();
    sidecarRow->addWidget(new QLabel(QStringLiteral("Sidecar:"), groupBox));
    sidecarRow->addWidget(m_sidecarLabel, 1);

    auto* cropRow = new QHBoxLayout();
    cropRow->addWidget(new QLabel(QStringLiteral("Crop image:"), groupBox));
    cropRow->addWidget(m_cropLabel, 1);

    auto* stageRow = new QHBoxLayout();
    stageRow->addWidget(new QLabel(QStringLiteral("Stage:"), groupBox));
    stageRow->addWidget(m_stageLabel, 1);

    groupLayout->addLayout(sidecarRow);
    groupLayout->addLayout(cropRow);
    groupLayout->addLayout(stageRow);
    groupLayout->addSpacing(4);
    groupLayout->addWidget(m_stageProgressBar);

    // --- Button row ---
    auto* buttonRow = new QHBoxLayout();
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_abortButton);
    buttonRow->addWidget(m_closeButton);

    // --- Main layout ---
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_statusLabel);
    mainLayout->addWidget(m_progressBar);
    mainLayout->addWidget(groupBox);
    mainLayout->addSpacing(4);
    mainLayout->addLayout(buttonRow);

    connect(m_abortButton, &QPushButton::clicked,
            this, &BatchProgressDialog::abortRequested);
    connect(m_closeButton, &QPushButton::clicked,
            this, &QDialog::accept);
}

void BatchProgressDialog::onFileStarted(int index, const QString& sidecarBasename)
{
    m_statusLabel->setText(
        QStringLiteral("Processing file %1 of %2\u2026").arg(index + 1).arg(m_totalFiles));
    m_sidecarLabel->setText(sidecarBasename);
    m_cropLabel->setText(QStringLiteral("Loading\u2026"));
    m_stageLabel->setText(QStringLiteral("\u2014"));
    m_stageProgressBar->setValue(0);
    m_progressBar->setValue(index);
}

void BatchProgressDialog::onCropBasenameKnown(const QString& cropBasename)
{
    m_cropLabel->setText(cropBasename.isEmpty() ? QStringLiteral("\u2014") : cropBasename);
}

void BatchProgressDialog::onStageAdvanced(ProcessingStage stage)
{
    m_stageLabel->setText(processingStageLabel(stage));
    m_stageProgressBar->setValue(static_cast<int>(stage));
}

void BatchProgressDialog::onFileFinished(int index, bool /*success*/)
{
    m_stageProgressBar->setValue(static_cast<int>(ProcessingStage::StageCount));
    m_progressBar->setValue(index + 1);
}

void BatchProgressDialog::onAllFinished(int processedCount, int errorCount)
{
    const bool aborted = (processedCount + errorCount) < m_totalFiles;

    m_statusLabel->setText(
        aborted
            ? QStringLiteral("Aborted \u2014 %1 succeeded, %2 failed (%3 skipped).")
                  .arg(processedCount)
                  .arg(errorCount)
                  .arg(m_totalFiles - processedCount - errorCount)
            : QStringLiteral("Done \u2014 %1 succeeded, %2 failed.")
                  .arg(processedCount)
                  .arg(errorCount));

    m_sidecarLabel->setText(QStringLiteral("\u2014"));
    m_cropLabel->setText(QStringLiteral("\u2014"));
    m_stageLabel->setText(aborted ? QStringLiteral("Aborted")
                                  : QStringLiteral("Complete"));
    m_stageProgressBar->setValue(static_cast<int>(ProcessingStage::StageCount));

    // Leave the overall bar at the number of files actually processed,
    // not forced to the maximum, so it is visually clear work stopped early.
    m_abortButton->setEnabled(false);
    m_closeButton->setEnabled(true);
}
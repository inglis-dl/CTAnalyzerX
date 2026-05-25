#include "MorphometryProgressDialog.h"

#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

MorphometryProgressDialog::MorphometryProgressDialog(int totalFiles, QWidget* parent)
	: QDialog(parent)
	, m_totalFiles(totalFiles)
{
	setWindowTitle(QStringLiteral("CTAXMorphometry - Progress"));
	setMinimumWidth(560);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	m_statusLabel = new QLabel(QStringLiteral("Starting\u2026"), this);
	m_fileLabel = new QLabel(QStringLiteral("-"), this);
	m_stageLabel = new QLabel(QStringLiteral("-"), this);

	m_fileProgressBar = new QProgressBar(this);
	m_stageProgressBar = new QProgressBar(this);

	m_abortButton = new QPushButton(QStringLiteral("Abort"), this);
	m_closeButton = new QPushButton(QStringLiteral("Close"), this);

	QFont bold = m_statusLabel->font();
	bold.setBold(true);
	m_statusLabel->setFont(bold);
	m_stageLabel->setFont(bold);

	m_fileProgressBar->setRange(0, totalFiles);
	m_fileProgressBar->setValue(0);
	m_fileProgressBar->setTextVisible(true);
	m_fileProgressBar->setFormat(QStringLiteral("%v / %m files"));

	m_stageProgressBar->setRange(0, 6); // 0..5 stages + finished
	m_stageProgressBar->setValue(0);
	m_stageProgressBar->setTextVisible(false);

	m_abortButton->setEnabled(true);
	m_closeButton->setEnabled(false);

	auto* currentGroup = new QGroupBox(QStringLiteral("Current file"), this);
	auto* currentLayout = new QVBoxLayout(currentGroup);

	auto* fileRow = new QHBoxLayout();
	fileRow->addWidget(new QLabel(QStringLiteral("Mask file:"), currentGroup));
	fileRow->addWidget(m_fileLabel, 1);

	auto* stageRow = new QHBoxLayout();
	stageRow->addWidget(new QLabel(QStringLiteral("Stage:"), currentGroup));
	stageRow->addWidget(m_stageLabel, 1);

	currentLayout->addLayout(fileRow);
	currentLayout->addLayout(stageRow);
	currentLayout->addSpacing(4);
	currentLayout->addWidget(m_stageProgressBar);

	auto* buttonRow = new QHBoxLayout();
	buttonRow->addStretch(1);
	buttonRow->addWidget(m_abortButton);
	buttonRow->addWidget(m_closeButton);

	auto* mainLayout = new QVBoxLayout(this);
	mainLayout->addWidget(m_statusLabel);
	mainLayout->addWidget(m_fileProgressBar);
	mainLayout->addWidget(currentGroup);
	mainLayout->addSpacing(4);
	mainLayout->addLayout(buttonRow);

	connect(m_abortButton, &QPushButton::clicked, this, &MorphometryProgressDialog::abortRequested);
	connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

QString MorphometryProgressDialog::stageLabelFromId(int stageId)
{
	switch (stageId)
	{
		case 0: return QStringLiteral("Load");
		case 1: return QStringLiteral("Bone metrics");
		case 2: return QStringLiteral("Topology");
		case 3: return QStringLiteral("Void metrics");
		case 4: return QStringLiteral("Surfaces");
		case 5: return QStringLiteral("Complete");
		default: return QStringLiteral("Working");
	}
}

void MorphometryProgressDialog::onFileStarted(int index, int total, const QString& filePath)
{
	Q_UNUSED(total);
	m_statusLabel->setText(QStringLiteral("Processing file %1 of %2\u2026").arg(index + 1).arg(m_totalFiles));
	m_fileLabel->setText(QFileInfo(filePath).fileName());
	m_stageLabel->setText(QStringLiteral("Starting"));
	m_stageProgressBar->setValue(0);
	m_fileProgressBar->setValue(index);
}

void MorphometryProgressDialog::onFileStageChanged(int index, int total, const QString& filePath, int stageId)
{
	Q_UNUSED(index);
	Q_UNUSED(total);
	Q_UNUSED(filePath);

	m_stageLabel->setText(stageLabelFromId(stageId));
	m_stageProgressBar->setValue(stageId);
}

void MorphometryProgressDialog::onFileFinished(int index, int total, bool success, const QString& message)
{
	Q_UNUSED(total);
	Q_UNUSED(success);
	Q_UNUSED(message);

	m_stageProgressBar->setValue(6);
	m_fileProgressBar->setValue(index + 1);
}

void MorphometryProgressDialog::onAllFinished(int successCount, int errorCount)
{
	const bool aborted = (successCount + errorCount) < m_totalFiles;

	m_statusLabel->setText(
		aborted
			? QStringLiteral("Aborted - %1 succeeded, %2 failed (%3 skipped).")
				  .arg(successCount)
				  .arg(errorCount)
				  .arg(m_totalFiles - successCount - errorCount)
			: QStringLiteral("Done - %1 succeeded, %2 failed.")
				  .arg(successCount)
				  .arg(errorCount));

	m_fileLabel->setText(QStringLiteral("—"));
	m_stageLabel->setText(aborted ? QStringLiteral("Aborted") : QStringLiteral("Complete"));
	m_stageProgressBar->setValue(6);

	m_abortButton->setEnabled(false);
	m_closeButton->setEnabled(true);
}
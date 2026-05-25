#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;
class QString;

class MorphometryProgressDialog : public QDialog
{
	Q_OBJECT

public:
	explicit MorphometryProgressDialog(int totalFiles, QWidget* parent = nullptr);

signals:
	void abortRequested();

public slots:
	void onFileStarted(int index, int total, const QString& filePath);
	void onFileStageChanged(int index, int total, const QString& filePath, int stageId);
	void onFileFinished(int index, int total, bool success, const QString& message);
	void onAllFinished(int successCount, int errorCount);

private:
	static QString stageLabelFromId(int stageId);

	QLabel* m_statusLabel = nullptr;
	QLabel* m_fileLabel = nullptr;
	QLabel* m_stageLabel = nullptr;
	QProgressBar* m_fileProgressBar = nullptr;
	QProgressBar* m_stageProgressBar = nullptr;
	QPushButton* m_abortButton = nullptr;
	QPushButton* m_closeButton = nullptr;

	int m_totalFiles = 0;
};

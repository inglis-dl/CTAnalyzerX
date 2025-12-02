#pragma once

#include <QObject>
#include <QStateMachine>
#include <QState>
#include <QFinalState>
#include <atomic>
#include <QString>

class ImageProcessingStateMachine : public QObject
{
	Q_OBJECT
public:
	enum State {
		Idle = 0,
		LoadingImage,         // step 1
		DefiningCrop,         // step 2 (UI to define crop)
		ApplyingCrop,         // step 3 (apply crop and save tmp)
		LoadingCropped,       // step 4 (load & display cropped)
		PlacingFiducials,     // step 5
		InteractiveRotation,  // step 6 (interactive via widget)
		ApplyingRotation,     // step 7 (apply rotation + save)
		LoadingRotated,       // step 7b (reload rotated)
		ComputingThreshold,   // step 8 (Otsu/histogram)
		Segmenting,           // step 9 (region growing / isolate)
		SavingSegment,        // step 10 (save result)
		Completed,
		ErrorState
	};
	Q_ENUM(State)

		explicit ImageProcessingStateMachine(QObject* parent = nullptr);
	~ImageProcessingStateMachine() override = default;

	// Simple data holders (optional helpers)
	void setInputFilePath(const QString& p) { m_inputFile = p; }
	QString inputFilePath() const { return m_inputFile; }
	void setTempDir(const QString& d) { m_tempDir = d; }
	QString tempDir() const { return m_tempDir; }

	// New accessors for external checks
	State currentState() const { return m_currentState; }
	bool isActive() const { return m_active.load(); }

public slots:
	// External control
	void start();
	void cancel();

	// Called by workers/UI when a step completes
	void notifyImageLoaded();
	void notifyCropDefined();
	void notifyCropApplied();
	void notifyCroppedLoaded();
	void notifyFiducialsPlaced();
	void notifyInteractiveRotationFinished();
	void notifyRotationApplied();
	void notifyRotatedLoaded();
	void notifyThresholdComputed();
	void notifySegmentationDone();
	void notifySaved();
	void notifyFailed(const QString& reason);

signals:
	// Internal transition triggers (used by addTransition)
	void started();
	void canceled();
	void imageLoaded();
	void cropDefined();
	void cropApplied();
	void croppedLoaded();
	void fiducialsPlaced();
	void interactiveRotationFinished();
	void rotationApplied();
	void rotatedLoaded();
	void thresholdComputed();
	void segmentationDone();
	void saved();
	void failed(const QString& reason);

	// Emitted when entering states: connect these to actual work.
	void requestLoadImage();
	void requestDefineCrop();
	void requestApplyCrop();       // should create cropped tmp file and signal back path via VolumeView
	void requestLoadCropped();
	void requestPlaceFiducials();
	void requestStartInteractiveRotation();
	void requestApplyRotation();   // should create rotated tmp file
	void requestLoadRotated();
	void requestComputeThreshold();
	void requestSegment();
	void requestSaveSegment();

	// Terminal notifications
	void finished();
	void error(const QString& reason);

	// New: broadcast when internal state changes
	void stateChanged(ImageProcessingStateMachine::State newState);

private:
	QStateMachine* m_machine = nullptr;
	QState* m_idle = nullptr;
	QState* m_loading = nullptr;
	QState* m_definingCrop = nullptr;
	QState* m_applyingCrop = nullptr;
	QState* m_loadingCropped = nullptr;
	QState* m_placingFiducials = nullptr;
	QState* m_interactiveRotation = nullptr;
	QState* m_applyingRotation = nullptr;
	QState* m_loadingRotated = nullptr;
	QState* m_computingThreshold = nullptr;
	QState* m_segmenting = nullptr;
	QState* m_saving = nullptr;
	QFinalState* m_final = nullptr;

	// optional runtime info (paths)
	QString m_inputFile;
	QString m_tempDir;

	// runtime tracking (single source of truth for active/state)
	std::atomic<bool> m_active{ false };
	State m_currentState = Idle;
};
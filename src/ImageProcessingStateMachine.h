#pragma once

#include <QObject>
#include <QStateMachine>
#include <QState>
#include <QFinalState>
#include <atomic>
#include <QString>
#include <QJsonObject> // new

class ImageProcessingStateMachine : public QObject
{
	Q_OBJECT
public:
	enum State {
		Idle = 0,
		LoadingImage,         // step 1
		DefiningCrop,         // step 2 (UI to define crop)
		LoadingCropped,       // step 3 (load & display cropped)  <-- ApplyingCrop removed
		PlacingFiducials,     // step 4
		InteractiveRotation,  // step 5 (interactive via widget)
		ApplyingRotation,     // step 6 (apply rotation + save)
		LoadingRotated,       // step 6b (reload rotated)
		ComputingThreshold,   // step 7 (Otsu/histogram)
		Segmenting,           // step 8 (region growing / isolate)
		SavingSegment,        // step 9 (save result)
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

	// New: sidecar / provenance helpers (owned by state machine)
	// Read the JSON sidecar for the current input file into internal state
	bool readSidecarForInput();
	// Convenience: write a crop sidecar for an output derived image and append history
	bool writeCropSidecarForOutput(const QString& outPath, const QJsonObject& params);
	// Append a workflow step entry to an image's sidecar history
	bool appendHistoryToSidecar(const QString& imagePath, const QString& stepName, const QJsonObject& params);

	// Add an external signal->state transition so UI widgets can drive the state machine directly.
	// Example usage:
	//   addExternalTransition(DefiningCrop, LoadingCropped, widget, SIGNAL(someSignal()));
	bool addExternalTransition(State from, State to, QObject* sender, const char* signal);

	// Query last derived path produced by this state machine (if any)
	QString lastDerivedPath() const { return m_lastDerivedPath; }
	bool inputIsDerived() const { return m_isDerived; }

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
	// New: request the application to automatically save the cropped volume
	void requestSaveCropped();
	// requestApplyCrop removed (Apply step no longer part of state machine)
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
	// m_applyingCrop removed
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

	// Sidecar / provenance info (owned by state machine)
	QJsonObject m_sidecar;
	bool m_isDerived = false;
	QString m_derivedFrom;
	QString m_lastDerivedPath;

	// runtime tracking (single source of truth for active/state)
	std::atomic<bool> m_active{ false };
	State m_currentState = Idle;

	// helper to map enum -> QState* (keeps mapping internal)
	QAbstractState* stateForEnum(State s) const;
};
#pragma once

#include <QObject>
#include <QJsonObject>
#include <atomic>

class QStateMachine;
class QState;
class QFinalState;
class QAbstractState;

class ImageProcessingStateMachine : public QObject
{
	Q_OBJECT
public:
	enum State {
		Idle = 0,
		LoadingImage,
		DefiningCrop,
		LoadingCropped,
		PlacingFiducials,
		InteractiveRotation,
		ApplyingRotation,
		LoadingRotated,
		ComputingThreshold,
		Segmenting,
		SavingSegment,
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
	// Load a project JSON (sidecar) file and drive UI consumers via signals:
	//  - requestOpenImage(path) -> MainWindow should open the image at path
	//  - suggestedState(State) -> consumers (MainWindow / WorkflowPanelWidget) may update UI for next step
	//  - projectLoaded(sidecarPath) -> project was parsed and is considered active
	// Returns true if the project file was parsed successfully.
	bool loadProjectSidecarFile(const QString& sidecarPath);
	// Convenience: write a crop sidecar for an output derived image and append history
	bool writeCropSidecarForOutput(const QString& outPath, const QJsonObject& params);
	// Append a workflow step entry to an image's sidecar history
	// This will schedule an asynchronous write and return true if scheduled.
	bool appendHistoryToSidecar(const QString& imagePath, const QString& stepName, const QJsonObject& params);

	// Add an external signal->state transition so UI widgets can drive the state machine directly.
	bool addExternalTransition(State from, State to, QObject* sender, const char* signal);

	// Query last derived path produced by this state machine (if any)
	QString lastDerivedPath() const { return m_lastDerivedPath; }
	bool inputIsDerived() const { return m_isDerived; }

	static QString stateToString(State s);

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
	void requestLoadCropped();
	void requestPlaceFiducials();
	void requestStartInteractiveRotation();
	void requestApplyRotation();
	void requestLoadRotated();
	void requestComputeThreshold();
	void requestSegment();
	void requestSaveSegment();

	// Terminal notifications
	void finished();
	void error(const QString& reason);

	// New: broadcast when internal state changes
	void stateChanged(ImageProcessingStateMachine::State newState);

	// New: sidecar persistence notifications (emitted when async write completes or fails)
	void sidecarWritten(const QString& sidecarPath);
	void sidecarWriteFailed(const QString& imagePath, const QString& reason);
	// New: request UI to open a specific image file (path may be derived output or original source)
	void requestOpenImage(const QString& imagePath);
	// New: suggest a UI workflow state to be displayed (does not change internal QStateMachine state)
	void suggestedState(ImageProcessingStateMachine::State suggested);
	// Emitted when a project JSON (sidecar) is successfully loaded/parsed
	void projectLoaded(const QString& projectPath);

private:
	QStateMachine* m_machine = nullptr;
	QState* m_idle = nullptr;
	QState* m_loading = nullptr;
	QState* m_definingCrop = nullptr;
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
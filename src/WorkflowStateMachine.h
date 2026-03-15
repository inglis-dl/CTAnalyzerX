#pragma once

#include <QObject>
#include <QJsonObject>
#include <utility>
#include <QStateMachine>
#include <QState>
#include <QFinalState>
#include <QHistoryState>
#include <QSignalTransition>

class WorkflowStateMachine : public QObject
{
	Q_OBJECT
	// "running" == QStateMachine engine is running (always true after ctor in this design)
	Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)

	// "workflowActive" == a workflow job is currently in progress (this is what MainWindow really wants)
	Q_PROPERTY(bool workflowActive READ isWorkflowActive NOTIFY workflowActiveChanged)

	Q_PROPERTY(State currentState READ currentState NOTIFY stateChanged)

	// Make these writable so QState::assignProperty reliably updates + triggers your setters.
	Q_PROPERTY(bool canDefineCrop READ canDefineCrop WRITE setCanDefineCrop NOTIFY canDefineCropChanged)
	Q_PROPERTY(bool canSaveCrop READ canSaveCrop WRITE setCanSaveCrop NOTIFY canSaveCropChanged)
	Q_PROPERTY(bool canPlaceLandmarks READ canPlaceLandmarks WRITE setCanPlaceLandmarks NOTIFY canPlaceLandmarksChanged)

public:
	enum State {
		Idle = 0,
		LoadingImage,
		LoadingSidecar,
		ComputingThreshold,
		DefiningCrop,
		LoadingCropped,
		DefiningLandmarks,
		Completed,
		ErrorState
	};
	Q_ENUM(State)

	explicit WorkflowStateMachine(QObject* parent = nullptr);
	~WorkflowStateMachine() override = default;

	// Simple data holders (optional helpers)
	void setInputFilePath(const QString& p) { m_inputFile = p; }
	QString inputFilePath() const { return m_inputFile; }
	void setTempDir(const QString& d) { m_tempDir = d; }
	QString tempDir() const { return m_tempDir; }

	// Accessors - now derived from QStateMachine state
	State currentState() const;
	bool isRunning() const;

	// semantic "busy" flag for the overall workflow job
	bool isWorkflowActive() const;

	// State-dependent capability accessors (managed by assignProperty)
	bool canDefineCrop() const { return m_canDefineCrop; }
	bool canSaveCrop() const { return m_canSaveCrop; }
	bool canPlaceLandmarks() const { return m_canPlaceLandmarks; }

	// Sidecar / provenance helpers (owned by state machine)
	bool readSidecarForInput();
	bool loadProjectSidecarFile(const QString& sidecarPath);
	bool writeCropSidecarForOutput(const QString& outPath, const QJsonObject& params);
	bool appendHistoryToSidecar(const QString& imagePath, const QString& stepName, const QJsonObject& params);
	bool sidecarHasThreshold() const;

	// Add an external signal->state transition so UI widgets can drive the state machine directly.
	bool addExternalTransition(State from, State to, QObject* sender, const char* signal);

	// Query last derived path produced by this state machine (if any)
	QString lastDerivedPath() const { return m_lastDerivedPath; }
	bool inputIsDerived() const { return m_isDerived; }

	// History state persistence
	bool saveWorkflowState();
	bool restoreWorkflowState();

	static QString stateToString(State s);

public slots:
	// External control
	void start();
	void cancel();

	// Resume from saved state
	void resume();

	// allow MainWindow to push threshold result (so state machine can persist it)
	void onThresholdComputed(double threshold, const QString& method);

signals:
	// Core transition signals - external components emit these directly
	// The state machine listens via QSignalTransition
	void started();
	void canceled();
	void imageLoaded();
	void cropDefined();
	void cropApplied();
	void croppedLoaded();
	void landmarksPlaced();
	void thresholdComputed();
	void saved();
	void failed(const QString& reason);
	void resumeRequested();

	// Emitted when entering states: connect these to actual work.
	void requestLoadImage();
	void requestDefineCrop();
	void requestSaveCropped();
	void requestLoadCropped();
	void requestPlaceLandmarks();
	void requestComputeThreshold();
	void requestLoadLandmarks(const QJsonObject& landmarksData);
	void requestSaveLandmarks(const QJsonArray& landmarks);

	// Terminal notifications
	void finished();
	void error(const QString& reason);

	// Broadcast when internal state changes
	void stateChanged(WorkflowStateMachine::State newState);

	// Emitted when running state changes
	void runningChanged(bool running);

	// New: emitted when workflow starts/stops being "active" (non-idle)
	void workflowActiveChanged(bool active);

	// State-dependent capability change signals
	void canDefineCropChanged(bool can);
	void canSaveCropChanged(bool can);
	void canPlaceLandmarksChanged(bool can);

	// Sidecar persistence notifications (emitted when async write completes or fails)
	void sidecarWritten(const QString& sidecarPath);
	void sidecarWriteFailed(const QString& imagePath, const QString& reason);

	// Request UI to open a specific image file (path may be derived output or original source)
	void requestOpenImage(const QString& imagePath);

	// Suggest a UI workflow state to be displayed (does not change internal QStateMachine state)
	void suggestedState(WorkflowStateMachine::State suggested);

	// Emitted when a project JSON (sidecar) is successfully loaded/parsed
	void projectLoaded(const QString& projectPath);

	// Emitted when the state machine parses a sidecar and detects (or clears) a threshold.
	void thresholdChanged(bool present, double value);

	// Emitted when workflow state is restored
	void workflowRestored(WorkflowStateMachine::State restoredState);

	// internal transition signals (completion events)
	void sidecarReady();
	void thresholdReady();

	// user actions
	void cropReset();

private:
	QStateMachine* m_machine = nullptr;
	QState* m_idle = nullptr;
	QState* m_loading = nullptr;
	QState* m_definingCrop = nullptr;
	QState* m_loadingCropped = nullptr;
	QState* m_definingLandmarks = nullptr;
	QFinalState* m_final = nullptr;

	// Deep history state for workflow resumption
	QHistoryState* m_workflowHistory = nullptr;

	// Optional runtime info (paths)
	QString m_inputFile;
	QString m_tempDir;

	// Sidecar / provenance info (owned by state machine)
	QJsonObject m_sidecar;
	bool m_isDerived = false;
	QString m_derivedFrom;
	QString m_lastDerivedPath;

	// State-dependent capabilities (managed by QState::assignProperty)
	bool m_canDefineCrop = false;
	bool m_canSaveCrop = false;
	bool m_canPlaceLandmarks = false;

	// states
	QState* m_loadingSidecar = nullptr;
	QState* m_computingThreshold = nullptr;

	// sidecar helpers
	bool ensureSidecarForSource();
	bool sidecarHasCropOutput(QString* outPath) const;
	bool sidecarHasThresholdValue(double* outVal) const;
	bool writeThresholdToSidecar(double val, const QString& method);

	// Helper to map enum -> QState* (keeps mapping internal)
	QAbstractState* stateForEnum(State s) const;

	// Extract first recorded threshold from a sidecar operations[]
	std::pair<bool, double> parseThreshold(const QJsonObject& side) const;

	// Derive State enum from current QStateMachine configuration
	State deriveCurrentState() const;

	void setupWorkflowOrchestration();

	// Setup state change monitoring
	void connectStateChangeNotifications();

	// Configure state-dependent properties using assignProperty
	void configureStateProperties();

	// Internal setters for state-dependent capabilities (called by Qt property system)
	void setCanDefineCrop(bool can);
	void setCanSaveCrop(bool can);
	void setCanPlaceLandmarks(bool can);

	// State persistence
	QJsonObject serializeWorkflowState() const;
	bool deserializeWorkflowState(const QJsonObject& stateObj);
	void setupHistoryState();

	// helper: compute active state from enum
	bool computeWorkflowActive(State s) const;

	QString croppedImagePathFromSidecar() const;

};
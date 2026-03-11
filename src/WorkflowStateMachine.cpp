#include "WorkflowStateMachine.h"
#include "JsonUtils.h"

#include <QtConcurrent/QtConcurrent>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFinalState>
#include <QFuture>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QSignalTransition>
#include <QState>
#include <QStateMachine>

#include <limits>
#include <utility>

WorkflowStateMachine::WorkflowStateMachine(QObject* parent)
	: QObject(parent)
{
	m_machine = new QStateMachine(this);

	// Create states
	m_idle = new QState(m_machine);
	m_loading = new QState(m_machine);
	m_loadingSidecar = new QState(m_machine);
	m_computingThreshold = new QState(m_machine);
	m_definingCrop = new QState(m_machine);
	m_loadingCropped = new QState(m_machine);
	m_definingLandmarks = new QState(m_machine);
	m_final = new QFinalState(m_machine);

	// Transitions (linearized + explicit gates)
	m_idle->addTransition(this, &WorkflowStateMachine::started, m_loading);

	m_loading->addTransition(this, &WorkflowStateMachine::imageLoaded, m_loadingSidecar);

	m_loadingSidecar->addTransition(this, &WorkflowStateMachine::sidecarReady, m_computingThreshold);

	m_computingThreshold->addTransition(this, &WorkflowStateMachine::thresholdReady, m_definingCrop);

	m_definingCrop->addTransition(this, &WorkflowStateMachine::cropApplied, m_loadingCropped);
	m_loadingCropped->addTransition(this, &WorkflowStateMachine::croppedLoaded, m_definingLandmarks);

	m_definingLandmarks->addTransition(this, &WorkflowStateMachine::landmarksPlaced, m_final);

	// crop reset self-loop (stay in DefiningCrop, but can be used by UI to show feedback)
	m_definingCrop->addTransition(this, &WorkflowStateMachine::cropReset, m_definingCrop);

	// Cancel transitions
	QList<QState*> cancellable = { m_loading, m_loadingSidecar, m_computingThreshold, m_definingCrop, m_loadingCropped, m_definingLandmarks };
	for (QState* s : cancellable) {
		s->addTransition(this, &WorkflowStateMachine::canceled, m_idle);
	}

	// Configure state-dependent properties using assignProperty
	configureStateProperties();

	// Setup state change notifications
	connectStateChangeNotifications();

	// Setup history state for workflow resumption
	setupHistoryState();

	// Connect workflow orchestration logic to state entries
	setupWorkflowOrchestration();

	m_machine->setInitialState(m_idle);
	m_machine->start();
}

void WorkflowStateMachine::setupWorkflowOrchestration()
{
	connect(m_loading, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering LoadingImage state";

		// This loads the source image. MainWindow will emit imageLoaded() when done.
		emit requestLoadImage();
	});

	connect(m_loadingSidecar, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering LoadingSidecar state";

		if (!ensureSidecarForSource()) {
			emit failed(QStringLiteral("Could not create/read sidecar"));
			emit canceled(); // conservative: return to Idle
			return;
		}

		emit sidecarReady();
	});

	connect(m_computingThreshold, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering ComputingThreshold state";

		double val = 0.0;
		if (sidecarHasThresholdValue(&val)) {
			emit thresholdChanged(true, val);
			emit thresholdReady();
			return;
		}

		// Ask MainWindow to compute Otsu; on completion MainWindow must call:
		//   stateMachine->onThresholdComputed(value, "otsu");
		emit requestComputeThreshold();
	});

	connect(m_definingCrop, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering DefiningCrop state";

		// If crop already exists in sidecar, go directly to loading cropped image.
		QString cropPath;
		if (sidecarHasCropOutput(&cropPath) && !cropPath.isEmpty() && QFile::exists(cropPath)) {
			m_lastDerivedPath = cropPath;
			m_isDerived = true;

			emit cropApplied();
			emit requestOpenImage(cropPath);
			return;
		}

		// Otherwise let UI define crop
		emit requestDefineCrop();
	});

	connect(m_loadingCropped, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering LoadingCropped state";

		if (!m_lastDerivedPath.isEmpty() && QFile::exists(m_lastDerivedPath)) {
			// MainWindow should open it via requestOpenImage already; else request explicit load
			return;
		}

		emit requestLoadCropped();
	});

	// DefiningLandmarks left as-is for now (rotation/segmentation later)
	connect(m_definingLandmarks, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering DefiningLandmarks state";

		// Enforce invariant: landmarks must be placed on the cropped image.
		const QString desiredCrop = croppedImagePathFromSidecar();

		if (!desiredCrop.isEmpty() && QFile::exists(desiredCrop)) {
			// If we are not already showing the cropped image, switch to it now.
			// MainWindow will call croppedLoaded() when it opens a path == lastDerivedPath.
			if (m_lastDerivedPath != desiredCrop) {
				m_lastDerivedPath = desiredCrop;
			}

			// If the UI is currently showing source, request opening the crop.
			// We can't directly query Lightbox current file here, so we always request the crop
			// and rely on MainWindow open dedup / fast path.
			emit requestOpenImage(desiredCrop);

			// IMPORTANT: do NOT requestPlaceLandmarks() until we are sure the crop is opened.
			// MainWindow will emit croppedLoaded() when it opens lastDerivedPath; then we will re-enter
			// this state only if you transition, so instead we simply queue enabling placement here
			// after croppedLoaded() by using a connection once in ctor (see Patch 3).
			return;
		}

		// No crop found: fall back (should not happen in a normal resume-to-landmarks)
		qWarning() << "DefiningLandmarks entered but no cropped image path found in sidecar";
		emit requestPlaceLandmarks();
	});

	connect(m_final, &QFinalState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Workflow completed successfully";
		emit finished();
	});
}

void WorkflowStateMachine::configureStateProperties()
{
	m_idle->assignProperty(this, "canDefineCrop", false);
	m_idle->assignProperty(this, "canSaveCrop", false);
	m_idle->assignProperty(this, "canPlaceLandmarks", false);

	m_loading->assignProperty(this, "canDefineCrop", false);
	m_loading->assignProperty(this, "canSaveCrop", false);
	m_loading->assignProperty(this, "canPlaceLandmarks", false);

	m_loadingSidecar->assignProperty(this, "canDefineCrop", false);
	m_loadingSidecar->assignProperty(this, "canSaveCrop", false);
	m_loadingSidecar->assignProperty(this, "canPlaceLandmarks", false);

	m_computingThreshold->assignProperty(this, "canDefineCrop", false);
	m_computingThreshold->assignProperty(this, "canSaveCrop", false);
	m_computingThreshold->assignProperty(this, "canPlaceLandmarks", false);

	m_definingCrop->assignProperty(this, "canDefineCrop", true);
	m_definingCrop->assignProperty(this, "canSaveCrop", true);
	m_definingCrop->assignProperty(this, "canPlaceLandmarks", false);

	m_loadingCropped->assignProperty(this, "canDefineCrop", false);
	m_loadingCropped->assignProperty(this, "canSaveCrop", false);
	m_loadingCropped->assignProperty(this, "canPlaceLandmarks", false);

	m_definingLandmarks->assignProperty(this, "canDefineCrop", false);
	m_definingLandmarks->assignProperty(this, "canSaveCrop", false);
	m_definingLandmarks->assignProperty(this, "canPlaceLandmarks", true);
}

// Internal setters that emit change signals when Qt property system updates values
void WorkflowStateMachine::setCanDefineCrop(bool can)
{
	if (m_canDefineCrop != can) {
		m_canDefineCrop = can;
		emit canDefineCropChanged(can);
	}
}

void WorkflowStateMachine::setCanSaveCrop(bool can)
{
	if (m_canSaveCrop != can) {
		m_canSaveCrop = can;
		emit canSaveCropChanged(can);
	}
}

void WorkflowStateMachine::setCanPlaceLandmarks(bool can)
{
	if (m_canPlaceLandmarks != can) {
		m_canPlaceLandmarks = can;
		emit canPlaceLandmarksChanged(can);
	}
}

bool WorkflowStateMachine::addExternalTransition(State from, State to, QObject* sender, const char* signal)
{
	QAbstractState* fromState = stateForEnum(from);
	QAbstractState* toState = stateForEnum(to);

	if (!fromState || !toState || !sender || !signal || !m_machine)
	{
		return false;
	}

	QState* fromQState = qobject_cast<QState*>(fromState);
	if (!fromQState)
	{
		return false;
	}

	QSignalTransition* transition = fromQState->addTransition(sender, signal, toState);
	return transition != nullptr;
}

bool WorkflowStateMachine::computeWorkflowActive(State s) const
{
	switch (s) {
		case WorkflowStateMachine::LoadingImage:
		case WorkflowStateMachine::LoadingSidecar:
		case WorkflowStateMachine::ComputingThreshold:
		case WorkflowStateMachine::DefiningCrop:
		case WorkflowStateMachine::LoadingCropped:
		case WorkflowStateMachine::DefiningLandmarks:
		return true;
		case WorkflowStateMachine::Idle:
		case WorkflowStateMachine::Completed:
		case WorkflowStateMachine::ErrorState:
		default:
		return false;
	}
}

bool WorkflowStateMachine::isWorkflowActive() const
{
	return computeWorkflowActive(currentState());
}

WorkflowStateMachine::State WorkflowStateMachine::currentState() const
{
	return deriveCurrentState();
}

bool WorkflowStateMachine::isRunning() const
{
	return m_machine && m_machine->isRunning();
}

WorkflowStateMachine::State WorkflowStateMachine::deriveCurrentState() const
{
	if (!m_machine) return Idle;

	const auto& config = m_machine->configuration();

	if (config.contains(m_loading)) return LoadingImage;
	if (config.contains(m_loadingSidecar)) return LoadingSidecar;
	if (config.contains(m_computingThreshold)) return ComputingThreshold;
	if (config.contains(m_definingCrop)) return DefiningCrop;
	if (config.contains(m_loadingCropped)) return LoadingCropped;
	if (config.contains(m_definingLandmarks)) return DefiningLandmarks;
	if (config.contains(m_final)) return Completed;

	return Idle;
}

void WorkflowStateMachine::connectStateChangeNotifications()
{
	// Engine running changes (engine is always running in this design, but keep signals consistent)
	connect(m_machine, &QStateMachine::started, this, [this]() {
		emit runningChanged(true);
		const State s = deriveCurrentState();
		emit stateChanged(s);
		emit workflowActiveChanged(computeWorkflowActive(s));
	});

	connect(m_machine, &QStateMachine::stopped, this, [this]() {
		emit runningChanged(false);
		emit stateChanged(Idle);
		emit workflowActiveChanged(false);
	});

	connect(m_machine, &QStateMachine::finished, this, [this]() {
		// Note: with an always-running engine you typically don't want the machine to "finish".
		emit runningChanged(false);
		emit stateChanged(Completed);
		emit workflowActiveChanged(false);
	});

	auto connectState = [this](QState* state, State enumValue) {
		if (!state) return;
		connect(state, &QState::entered, this, [this, enumValue]() {
			emit stateChanged(enumValue);
			emit workflowActiveChanged(computeWorkflowActive(enumValue));
		});
		};

	connectState(m_idle, Idle);
	connectState(m_loading, LoadingImage);
	connectState(m_definingCrop, DefiningCrop);
	connectState(m_loadingCropped, LoadingCropped);
	connectState(m_definingLandmarks, DefiningLandmarks);
	connectState(m_loadingSidecar, LoadingSidecar);
	connectState(m_computingThreshold, ComputingThreshold);

	if (m_final) {
		connect(m_final, &QFinalState::entered, this, [this]() {
			emit stateChanged(Completed);
			emit workflowActiveChanged(false);
		});
	}
}

QAbstractState* WorkflowStateMachine::stateForEnum(State s) const
{
	switch (s) {
	case Idle:                 return m_idle;
	case LoadingImage:         return m_loading;
	case LoadingSidecar:       return m_loadingSidecar;
	case ComputingThreshold:   return m_computingThreshold;
	case DefiningCrop:         return m_definingCrop;
	case LoadingCropped:       return m_loadingCropped;
	case DefiningLandmarks:    return m_definingLandmarks;
	case Completed:            return m_final;
	case ErrorState:           return m_idle;
	default:                   return nullptr;
	}
}

QString WorkflowStateMachine::stateToString(State s)
{
	switch (s) {
	case Idle: return QStringLiteral("Idle");
	case LoadingImage: return QStringLiteral("LoadingImage");
	case LoadingSidecar: return QStringLiteral("LoadingSidecar");
	case ComputingThreshold: return QStringLiteral("ComputingThreshold");
	case DefiningCrop: return QStringLiteral("DefiningCrop");
	case LoadingCropped: return QStringLiteral("LoadingCropped");
	case DefiningLandmarks: return QStringLiteral("DefiningLandmarks");
	case Completed: return QStringLiteral("Completed");
	case ErrorState: return QStringLiteral("ErrorState");
	default: return QStringLiteral("Unknown");
	}
}

void WorkflowStateMachine::start()
{
	emit started();
}

void WorkflowStateMachine::cancel()
{
	emit canceled();
}

void WorkflowStateMachine::resume()
{
	if (!m_machine) return;

	if (restoreWorkflowState()) {
		State restoredState = currentState();
		emit workflowRestored(restoredState);
		qDebug() << "Resuming workflow from" << stateToString(restoredState);
	}

	emit resumeRequested();
}

void WorkflowStateMachine::setupHistoryState()
{
	m_workflowHistory = new QHistoryState(QHistoryState::DeepHistory, m_machine);
	m_workflowHistory->setDefaultState(m_idle);

	// Use a dedicated signal so we don't conflict with normal start()
	QSignalTransition* resumeTransition = new QSignalTransition(this, &WorkflowStateMachine::resumeRequested);
	m_idle->addTransition(resumeTransition);
	resumeTransition->setTargetState(m_workflowHistory);

	qDebug() << "History state configured for workflow resumption";
}

bool WorkflowStateMachine::saveWorkflowState()
{
	if (!m_machine) return false;

	State current = currentState();
	if (current == Idle || current == Completed || current == ErrorState) {
		return false;
	}

	QJsonObject stateObj = serializeWorkflowState();
	if (stateObj.isEmpty()) return false;

	QJsonObject updatedSidecar = m_sidecar;
	updatedSidecar.insert(QStringLiteral("workflowState"), stateObj);

	const QString sidecarPath = JsonUtils::sidecarPathForImage(m_inputFile);
	
	QMetaObject::invokeMethod(this, [this, sidecarPath, updatedSidecar]() {
		QFile f(sidecarPath);
		if (!f.open(QIODevice::WriteOnly)) {
			emit sidecarWriteFailed(m_inputFile, tr("Cannot open sidecar for write"));
			return;
		}
		QJsonDocument doc(updatedSidecar);
		f.write(doc.toJson(QJsonDocument::Indented));
		f.close();
		emit sidecarWritten(sidecarPath);
	}, Qt::QueuedConnection);

	return true;
}

bool WorkflowStateMachine::restoreWorkflowState()
{
	if (!m_sidecar.contains(QStringLiteral("workflowState"))) {
		return false;
	}

	QJsonObject stateObj = m_sidecar.value(QStringLiteral("workflowState")).toObject();
	if (stateObj.isEmpty()) return false;

	return deserializeWorkflowState(stateObj);
}

QJsonObject WorkflowStateMachine::serializeWorkflowState() const
{
	QJsonObject obj;
	obj.insert(QStringLiteral("currentState"), static_cast<int>(currentState()));
	obj.insert(QStringLiteral("inputFile"), m_inputFile);
	obj.insert(QStringLiteral("isDerived"), m_isDerived);
	obj.insert(QStringLiteral("derivedFrom"), m_derivedFrom);
	obj.insert(QStringLiteral("lastDerivedPath"), m_lastDerivedPath);

	QJsonObject capabilities;
	capabilities.insert(QStringLiteral("canDefineCrop"), m_canDefineCrop);
	capabilities.insert(QStringLiteral("canSaveCrop"), m_canSaveCrop);
	capabilities.insert(QStringLiteral("canPlaceLandmarks"), m_canPlaceLandmarks);
	obj.insert(QStringLiteral("capabilities"), capabilities);

	obj.insert(QStringLiteral("savedAt"), QDateTime::currentDateTime().toString(Qt::ISODate));

	return obj;
}

bool WorkflowStateMachine::deserializeWorkflowState(const QJsonObject& stateObj)
{
	if (!stateObj.contains(QStringLiteral("currentState"))) return false;

	if (stateObj.contains(QStringLiteral("inputFile"))) {
		m_inputFile = stateObj.value(QStringLiteral("inputFile")).toString();
	}

	if (stateObj.contains(QStringLiteral("isDerived"))) {
		m_isDerived = stateObj.value(QStringLiteral("isDerived")).toBool();
	}
	if (stateObj.contains(QStringLiteral("derivedFrom"))) {
		m_derivedFrom = stateObj.value(QStringLiteral("derivedFrom")).toString();
	}
	if (stateObj.contains(QStringLiteral("lastDerivedPath"))) {
		m_lastDerivedPath = stateObj.value(QStringLiteral("lastDerivedPath")).toString();
	}

	if (stateObj.contains(QStringLiteral("capabilities"))) {
		QJsonObject caps = stateObj.value(QStringLiteral("capabilities")).toObject();
		setCanDefineCrop(caps.value(QStringLiteral("canDefineCrop")).toBool());
		setCanSaveCrop(caps.value(QStringLiteral("canSaveCrop")).toBool());
		setCanPlaceLandmarks(caps.value(QStringLiteral("canPlaceLandmarks")).toBool());
	}

	int stateInt = stateObj.value(QStringLiteral("currentState")).toInt();
	State targetState = static_cast<State>(stateInt);

	QAbstractState* target = stateForEnum(targetState);
	if (target && m_workflowHistory) {
		m_workflowHistory->setDefaultState(target);
		qDebug() << "Workflow state restored to" << stateToString(targetState);
		return true;
	}

	return false;
}

bool WorkflowStateMachine::loadProjectSidecarFile(const QString& sidecarPath)
{
	if (sidecarPath.isEmpty()) return false;

	QFile f(sidecarPath);
	if (!f.open(QIODevice::ReadOnly)) {
		qWarning() << "WorkflowStateMachine::loadProjectSidecarFile: failed to open" << sidecarPath;
		return false;
	}

	const QByteArray data = f.readAll();
	f.close();

	const QJsonDocument doc = QJsonDocument::fromJson(data);
	if (!doc.isObject()) {
		qWarning() << "WorkflowStateMachine::loadProjectSidecarFile: invalid json in" << sidecarPath;
		return false;
	}

	const QJsonObject obj = doc.object();

	// Cache loaded sidecar; LoadingSidecar state will normalize/create missing keys as needed.
	m_sidecar = obj;

	// Require a canonical source path in the project.
	const QString src = obj.value(QStringLiteral("source")).toString();
	if (src.isEmpty() || !QFile::exists(src)) {
		qWarning() << "WorkflowStateMachine::loadProjectSidecarFile: missing or invalid 'source' in" << sidecarPath;
		return false;
	}

	// Reset runtime provenance: project resumes from the source image; LoadingSidecar decides next.
	m_inputFile = src;
	m_tempDir.clear();

	m_isDerived = false;
	m_derivedFrom.clear();
	m_lastDerivedPath.clear();

	// Notify consumers: project loaded (for recents/menu) and open the SOURCE image only.
	emit projectLoaded(sidecarPath);

	// IMPORTANT: do not suggest a UI state here; LoadingSidecar/ComputingThreshold/DefiningCrop will drive it.
	emit requestOpenImage(src);

	// We do not start/resume the internal machine here because MainWindow will call resume()
	// and the workflow history will choose the correct state. Keeping this function side-effect
	// free avoids race conditions (open image async vs. state transitions).
	return true;
}

bool WorkflowStateMachine::readSidecarForInput()
{
	// Reset previous sidecar-derived state first.
	m_sidecar = QJsonObject();
	m_isDerived = false;
	m_derivedFrom.clear();
	m_lastDerivedPath.clear();

	if (m_inputFile.isEmpty()) return false;

	// Use canonical sidecar location logic from JsonUtils (AppData/projects/<base>.json)
	QJsonObject side = JsonUtils::readJsonSidecar(m_inputFile);
	if (side.isEmpty()) {
		// No sidecar found for this image -> not derived
		m_isDerived = false;
		return false;
	}

	// Store sidecar in state machine
	m_sidecar = side;

	// Emit threshold presence/state for the newly-read sidecar
	auto [present, val] = parseThreshold(m_sidecar);
	emit thresholdChanged(present, val);

	// Two canonical cases:
	// 1) Sidecar that describes a derived image: JsonUtils::writeCropSidecar writes "source" at top-level.
	//    In this case the sidecar file is associated with the derived image and contains "source".
	// 2) Sidecar that is a project for a source image: no top-level "source" key (or it may carry operations).
	//
	// Use presence of "source" to decide derived vs non-derived.
	if (m_sidecar.contains(QStringLiteral("source")) && m_sidecar.value(QStringLiteral("source")).isString()) {
		m_isDerived = true;
		m_derivedFrom = m_sidecar.value(QStringLiteral("source")).toString();
		// record that this sidecar belongs to the current input file
		m_lastDerivedPath = m_inputFile;
	}
	else {
		// Not a derived-image sidecar; treat as project sidecar for the source image.
		m_isDerived = false;

		// Optionally record lastDerivedPath if project's operations reference derived outputs.
		// Look for operations[].derived equal to some known value (not required for derived detection).
		if (m_sidecar.contains(QStringLiteral("operations")) && m_sidecar.value(QStringLiteral("operations")).isArray()) {
			QJsonArray ops = m_sidecar.value(QStringLiteral("operations")).toArray();
			for (const QJsonValue& v : ops) {
				if (!v.isObject()) continue;
				QJsonObject op = v.toObject();
				if (op.contains(QStringLiteral("derived")) && op.value(QStringLiteral("derived")).isString()) {
					// remember last derived path referenced (useful bookkeeping)
					m_lastDerivedPath = op.value(QStringLiteral("derived")).toString();
					// do NOT mark m_isDerived true because the current input is the project/source image
					// (we only mark derived when the sidecar itself indicates "source").>
				}
			}
		}
	}

	// Optionally notify listeners that a project-sidecar is present for this image.
	// Keep decision conservative: emit projectLoaded only when this sidecar looks like a project (i.e., not "source")
	if (!m_isDerived) {
		// Emit projectLoaded with the canonical sidecar path so MainWindow can add to recents.
		const QString sidecarPath = JsonUtils::sidecarPathForImage(m_inputFile);
		emit projectLoaded(sidecarPath);
	}

	return true;
}

bool WorkflowStateMachine::appendHistoryToSidecar(const QString& imagePath, const QString& stepName, const QJsonObject& params)
{
	if (imagePath.isEmpty() || stepName.isEmpty()) return false;

	// Load existing sidecar (may return empty -> start a new one)
	QJsonObject meta = JsonUtils::readJsonSidecar(imagePath);

	// Ensure minimal meta exists
	if (meta.isEmpty()) {
		// create a minimal header if none exists
		meta = QJsonObject();
	}

	// If this operation is a computed primary threshold for a source image,
	// ensure the sidecar records the canonical source image so reopening the
	// project will load the correct primary image later.
	const QString canonicalSource = QFileInfo(imagePath).absoluteFilePath();
	if (stepName == QStringLiteral("compute_threshold")) {
		// Ensure top-level "source" is present and points to the primary image
		if (!meta.contains(QStringLiteral("source")) || meta.value(QStringLiteral("source")).toString() != canonicalSource) {
			meta.insert(QStringLiteral("source"), canonicalSource);
		}

		// Also append a descriptive "load_primary_image" operation if not already present
		// so the operations[] history documents that the project is associated with the given source.
		bool needAddLoadOp = true;
		if (meta.contains(QStringLiteral("operations")) && meta.value(QStringLiteral("operations")).isArray()) {
			QJsonArray existingOps = meta.value(QStringLiteral("operations")).toArray();
			if (!existingOps.isEmpty()) {
				QJsonObject last = existingOps.last().toObject();
				const QString lastName = last.value(QStringLiteral("name")).toString().toLower();
				if (lastName == QStringLiteral("load_primary_image")) {
					QJsonObject lastParams = last.value(QStringLiteral("parameters")).toObject();
					if (lastParams.value(QStringLiteral("source")).toString() == canonicalSource) {
						needAddLoadOp = false;
					}
				}
			}
		}

		if (needAddLoadOp) {
			QJsonObject loadOp;
			loadOp.insert(QStringLiteral("name"), QStringLiteral("load_primary_image"));
			loadOp.insert(QStringLiteral("status"), QStringLiteral("completed"));
			loadOp.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
			loadOp.insert(QStringLiteral("parameters"), QJsonObject({ { QStringLiteral("source"), canonicalSource } }));

			// Append into operations array (create if missing)
			QJsonArray ops;
			if (meta.contains(QStringLiteral("operations")) && meta.value(QStringLiteral("operations")).isArray()) {
				ops = meta.value(QStringLiteral("operations")).toArray();
			}
			// Avoid duplicating identical trailing op (extra guard)
			bool duplicate = false;
			if (!ops.isEmpty()) {
				QJsonObject last = ops.last().toObject();
				if (last.value(QStringLiteral("name")).toString() == loadOp.value(QStringLiteral("name")).toString()) {
					QJsonObject lastParams = last.value(QStringLiteral("parameters")).toObject();
					const QByteArray lastParamsJson = QJsonDocument(lastParams).toJson(QJsonDocument::Compact);
					const QByteArray newParamsJson = QJsonDocument(loadOp.value(QStringLiteral("parameters")).toObject()).toJson(QJsonDocument::Compact);
					if (lastParamsJson == newParamsJson) duplicate = true;
				}
			}
			if (!duplicate) {
				ops.append(loadOp);
				meta.insert(QStringLiteral("operations"), ops);
			}
		}
	}

	// Build new operation entry for the requested step (threshold or others)
	QJsonObject entry;
	entry.insert(QStringLiteral("name"), stepName);

	QString derived;
	if (params.contains(QStringLiteral("derived")) && params.value(QStringLiteral("derived")).isString())
		derived = params.value(QStringLiteral("derived")).toString();
	else if (params.contains(QStringLiteral("cropped_path")) && params.value(QStringLiteral("cropped_path")).isString())
		derived = params.value(QStringLiteral("cropped_path")).toString();
	else if (params.contains(QStringLiteral("output")) && params.value(QStringLiteral("output")).isString())
		derived = params.value(QStringLiteral("output")).toString();

	// Determine status:
	// - If operation produced a derived path, mark completed.
	// - Special-case: operations that record meaningful parameters (e.g., a computed threshold)
	//   should be marked completed even if they don't produce a derived file.
	QString status;
	if (!derived.isEmpty()) {
		status = QStringLiteral("completed");
	}
	else if (stepName == QStringLiteral("compute_threshold") || params.contains(QStringLiteral("threshold"))) {
		// Computed threshold is a finished operation even without a derived file.
		status = QStringLiteral("completed");
	}
	else {
		status = QStringLiteral("pending");
	}

	if (!derived.isEmpty()) {
		entry.insert(QStringLiteral("status"), status);
		entry.insert(QStringLiteral("derived"), derived);
	}
	else {
		entry.insert(QStringLiteral("status"), status);
	}

	entry.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
	entry.insert(QStringLiteral("parameters"), params);

	// Append to operations array, but avoid duplicating an identical last entry
	QJsonArray ops;
	if (meta.contains(QStringLiteral("operations")) && meta.value(QStringLiteral("operations")).isArray()) {
		ops = meta.value(QStringLiteral("operations")).toArray();
	}

	// Dedup check: if last op exists and appears identical (by name + derived + parameters JSON) then skip append
	bool isDuplicate = false;
	if (!ops.isEmpty()) {
		QJsonObject last = ops.last().toObject();
		const QString lastName = last.value(QStringLiteral("name")).toString();
		const QString lastDerived = last.value(QStringLiteral("derived")).toString();
		QJsonObject lastParams = last.value(QStringLiteral("parameters")).toObject();

		// Compare by name and derived path and parameters JSON text
		const bool sameName = (lastName == stepName);
		const bool sameDerived = (lastDerived == derived);

		const QByteArray lastParamsJson = QJsonDocument(lastParams).toJson(QJsonDocument::Compact);
		const QByteArray newParamsJson = QJsonDocument(params).toJson(QJsonDocument::Compact);
		const bool sameParams = (lastParamsJson == newParamsJson);

		if (sameName && sameDerived && sameParams) {
			isDuplicate = true;
		}
	}

	if (!isDuplicate) {
		ops.append(entry);
		meta.insert(QStringLiteral("operations"), ops);

		// Persist updated sidecar synchronously (JsonUtils encapsulates write semantics)
		const bool ok = JsonUtils::writeJsonSidecar(imagePath, meta);
		if (!ok) {
			emit sidecarWriteFailed(imagePath, QStringLiteral("Failed to append operation to sidecar"));
			return false;
		}
		// After successful write, emit threshold state derived from the new meta so UI can update.
		{
			auto [present2, val2] = parseThreshold(meta);
			emit thresholdChanged(present2, val2);
		}

		// Emit canonical sidecar path (not the image path)
		const QString sidecar = JsonUtils::sidecarPathForImage(imagePath);
		emit sidecarWritten(sidecar);
		return true;
	}

	// duplicate — treat as success but don't write again
	return true;
}

bool WorkflowStateMachine::writeCropSidecarForOutput(const QString& outPath, const QJsonObject& params)
{
	if (outPath.isEmpty()) return false;
	if (m_inputFile.isEmpty()) return false;

	QJsonObject side = JsonUtils::readJsonSidecar(m_inputFile);
	if (side.isEmpty()) return false;

	// Expect CropExporter params to contain crop_extents array
	const QJsonArray ext = params.value(QStringLiteral("crop_extents")).toArray();
	if (ext.size() != 6) {
		emit sidecarWriteFailed(outPath, QStringLiteral("Missing/invalid crop_extents"));
		return false;
	}

	QJsonObject crop;
	crop.insert(QStringLiteral("extents"), ext);
	crop.insert(QStringLiteral("outputPath"), outPath);
	crop.insert(QStringLiteral("completedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
	side.insert(QStringLiteral("crop"), crop);

	// workflow bookkeeping
	QJsonObject workflow = side.value(QStringLiteral("workflow")).toObject();
	QJsonObject completed = workflow.value(QStringLiteral("completed")).toObject();
	completed.insert(QStringLiteral("crop"), true);
	workflow.insert(QStringLiteral("completed"), completed);
	workflow.insert(QStringLiteral("currentStep"), QStringLiteral("landmarks"));
	side.insert(QStringLiteral("workflow"), workflow);

	side.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

	if (!JsonUtils::writeJsonSidecar(m_inputFile, side)) {
		emit sidecarWriteFailed(outPath, QStringLiteral("Failed to write crop info to sidecar"));
		return false;
	}

	// Update internal provenance
	m_lastDerivedPath = outPath;
	m_isDerived = true;
	m_derivedFrom = QFileInfo(m_inputFile).absoluteFilePath();

	m_sidecar = JsonUtils::readJsonSidecar(m_inputFile);

	emit sidecarWritten(JsonUtils::sidecarPathForImage(m_inputFile));
	return true;
}

std::pair<bool, double> WorkflowStateMachine::parseThreshold(const QJsonObject& side) const
{
	if (!side.contains(QStringLiteral("operations")) || !side.value(QStringLiteral("operations")).isArray())
		return { false, std::numeric_limits<double>::quiet_NaN() };
	QJsonArray ops = side.value(QStringLiteral("operations")).toArray();
	for (const QJsonValue& v : ops) {
		if (!v.isObject()) continue;
		QJsonObject op = v.toObject();
		QJsonObject params = op.value(QStringLiteral("parameters")).toObject();
		if (params.contains(QStringLiteral("threshold"))) {
			const QJsonValue tv = params.value(QStringLiteral("threshold"));
			if (tv.isDouble()) return { true, tv.toDouble() };
			if (tv.isString()) {
				bool ok = false;
				double val = tv.toString().toDouble(&ok);
				if (ok) return { true, val };
			}
		}
	}
	return { false, std::numeric_limits<double>::quiet_NaN() };
}

bool WorkflowStateMachine::sidecarHasThreshold() const
{
	// If no input file known, nothing to check
	if (m_inputFile.isEmpty()) return false;

	// Prefer cached in-memory sidecar if present, otherwise read the canonical sidecar on disk.
	QJsonObject side;
	if (!m_sidecar.isEmpty()) {
		side = m_sidecar;

	}
	else {
		side = JsonUtils::readJsonSidecar(m_inputFile);

	}

	if (side.isEmpty()) return false;

	if (!side.contains(QStringLiteral("operations")) || !side.value(QStringLiteral("operations")).isArray())
		return false;

	QJsonArray ops = side.value(QStringLiteral("operations")).toArray();
	for (const QJsonValue& v : ops) {
		if (!v.isObject()) continue;
		QJsonObject op = v.toObject();
		// Check explicit name + threshold param
		const QString name = op.value(QStringLiteral("name")).toString().toLower();
		const QJsonObject params = op.value(QStringLiteral("parameters")).toObject();
		if (name == QStringLiteral("compute_threshold") && params.contains(QStringLiteral("threshold")))
			return true;
		// Accept any op that records a 'threshold' parameter (conservative)
		if (params.contains(QStringLiteral("threshold")))
			return true;

	}
	return false;
}

bool WorkflowStateMachine::ensureSidecarForSource()
{
	if (m_inputFile.isEmpty()) return false;

	// Always treat m_inputFile as source image for this stage
	const QString sourceAbs = QFileInfo(m_inputFile).absoluteFilePath();

	QJsonObject side = JsonUtils::readJsonSidecar(m_inputFile);
	const bool exists = !side.isEmpty();

	if (!exists) {
		QJsonObject created;
		created.insert(QStringLiteral("source"), sourceAbs);
		created.insert(QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
		created.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

		QJsonObject workflow;
		workflow.insert(QStringLiteral("currentStep"), QStringLiteral("threshold"));
		workflow.insert(QStringLiteral("completed"), QJsonObject());
		created.insert(QStringLiteral("workflow"), workflow);

		if (!JsonUtils::writeJsonSidecar(m_inputFile, created)) {
			return false;
		}

		m_sidecar = JsonUtils::readJsonSidecar(m_inputFile);
		return !m_sidecar.isEmpty();
	}

	// If it exists, normalize a few required keys (non-legacy, just enforce minimal structure)
	bool changed = false;

	if (!side.contains(QStringLiteral("source"))) {
		side.insert(QStringLiteral("source"), sourceAbs);
		changed = true;
	}

	if (!side.contains(QStringLiteral("updated"))) {
		side.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
		changed = true;
	}

	if (!side.contains(QStringLiteral("workflow")) || !side.value(QStringLiteral("workflow")).isObject()) {
		QJsonObject workflow;
		workflow.insert(QStringLiteral("currentStep"), QStringLiteral("threshold"));
		workflow.insert(QStringLiteral("completed"), QJsonObject());
		side.insert(QStringLiteral("workflow"), workflow);
		changed = true;
	}

	if (changed) {
		if (!JsonUtils::writeJsonSidecar(m_inputFile, side)) return false;
		side = JsonUtils::readJsonSidecar(m_inputFile);
		if (side.isEmpty()) return false;
	}

	m_sidecar = side;
	return true;
}

bool WorkflowStateMachine::sidecarHasThresholdValue(double* outVal) const
{
	if (outVal) *outVal = 0.0;
	if (m_sidecar.isEmpty()) return false;

	// New canonical keys:
	// threshold: { value: <double>, method: "otsu"|"manual", completedAt: iso }
	const QJsonObject thr = m_sidecar.value(QStringLiteral("threshold")).toObject();
	if (thr.isEmpty()) return false;

	const QJsonValue v = thr.value(QStringLiteral("value"));
	if (!v.isDouble()) return false;

	if (outVal) *outVal = v.toDouble();
	return true;
}

bool WorkflowStateMachine::sidecarHasCropOutput(QString* outPath) const
{
	if (outPath) outPath->clear();
	if (m_sidecar.isEmpty()) return false;

	// crop: { extents: [..6..], outputPath: "C:/.../crop.nii", completedAt: iso }
	const QJsonObject crop = m_sidecar.value(QStringLiteral("crop")).toObject();
	if (crop.isEmpty()) return false;

	const QString p = crop.value(QStringLiteral("outputPath")).toString();
	if (p.isEmpty()) return false;

	if (outPath) *outPath = p;
	return true;
}

bool WorkflowStateMachine::writeThresholdToSidecar(double val, const QString& method)
{
	if (m_inputFile.isEmpty()) return false;

	QJsonObject side = JsonUtils::readJsonSidecar(m_inputFile);
	if (side.isEmpty()) return false;

	QJsonObject thr;
	thr.insert(QStringLiteral("value"), val);
	thr.insert(QStringLiteral("method"), method);
	thr.insert(QStringLiteral("completedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
	side.insert(QStringLiteral("threshold"), thr);

	// workflow bookkeeping
	QJsonObject workflow = side.value(QStringLiteral("workflow")).toObject();
	QJsonObject completed = workflow.value(QStringLiteral("completed")).toObject();
	completed.insert(QStringLiteral("threshold"), true);
	workflow.insert(QStringLiteral("completed"), completed);
	workflow.insert(QStringLiteral("currentStep"), QStringLiteral("crop"));
	side.insert(QStringLiteral("workflow"), workflow);

	side.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

	if (!JsonUtils::writeJsonSidecar(m_inputFile, side)) return false;

	m_sidecar = JsonUtils::readJsonSidecar(m_inputFile);
	return !m_sidecar.isEmpty();
}

void WorkflowStateMachine::onThresholdComputed(double threshold, const QString& method)
{
	if (!writeThresholdToSidecar(threshold, method)) {
		emit failed(QStringLiteral("Failed to persist threshold to sidecar"));
		return;
	}

	emit thresholdChanged(true, threshold);
	emit thresholdReady();
}

QString WorkflowStateMachine::croppedImagePathFromSidecar() const
{
	// Prefer the new canonical key first
	const QJsonObject cropObj = m_sidecar.value(QStringLiteral("crop")).toObject();
	const QString cropPath = cropObj.value(QStringLiteral("outputPath")).toString();
	if (!cropPath.isEmpty()) {
		return cropPath;
	}

	// Fallback to workflowState persistence (resume support)
	const QJsonObject wf = m_sidecar.value(QStringLiteral("workflowState")).toObject();
	const QString last = wf.value(QStringLiteral("lastDerivedPath")).toString();
	return last;
}

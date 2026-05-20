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
	m_replacingThreshold = new QState(m_machine);
	m_final = new QFinalState(m_machine);

	// Transitions (linearized + explicit gates)
	m_idle->addTransition(this, &WorkflowStateMachine::started, m_loading);

	m_loading->addTransition(this, &WorkflowStateMachine::imageLoaded, m_loadingSidecar);

	m_loadingSidecar->addTransition(this, &WorkflowStateMachine::sidecarReady, m_computingThreshold);

	m_computingThreshold->addTransition(this, &WorkflowStateMachine::thresholdReady, m_definingCrop);

	m_definingCrop->addTransition(this, &WorkflowStateMachine::cropApplied, m_loadingCropped);
	m_loadingCropped->addTransition(this, &WorkflowStateMachine::croppedLoaded, m_replacingThreshold);

	m_replacingThreshold->addTransition(this, &WorkflowStateMachine::thresholdReplaced, m_final);

	// crop reset self-loop (stay in DefiningCrop, but can be used by UI to show feedback)
	m_definingCrop->addTransition(this, &WorkflowStateMachine::cropReset, m_definingCrop);

	// Cancel transitions
	QList<QState*> cancellable = {
		m_loading,
		m_loadingSidecar,
		m_computingThreshold,
		m_definingCrop,
		m_loadingCropped,
		m_replacingThreshold
	};
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
		emit requestLoadImage();
	});

	connect(m_loadingSidecar, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering LoadingSidecar state";

		if (!ensureSidecarForSource()) {
			emit failed(QStringLiteral("Could not create/read sidecar"));
			emit canceled();
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

		emit requestComputeThreshold();
	});

	connect(m_definingCrop, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering DefiningCrop state";

		QString cropPath;
		if (sidecarHasCropOutput(&cropPath) && !cropPath.isEmpty() && QFile::exists(cropPath)) {
			m_lastDerivedPath = cropPath;
			m_isDerived = true;

			emit cropApplied();
			emit requestOpenImage(cropPath);
			return;
		}

		emit requestDefineCrop();
	});

	connect(m_loadingCropped, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering LoadingCropped state";

		if (!m_lastDerivedPath.isEmpty() && QFile::exists(m_lastDerivedPath)) {
			return;
		}

		emit requestLoadCropped();
	});

	connect(m_replacingThreshold, &QState::entered, this, [this]() {
		qDebug() << "WorkflowStateMachine: Entering ReplacingThreshold state";
		// Cropped image should already be loaded before entering this state.
		emit requestReplaceThreshold();
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
	m_idle->assignProperty(this, "canReplaceThreshold", false);

	m_loading->assignProperty(this, "canDefineCrop", false);
	m_loading->assignProperty(this, "canSaveCrop", false);
	m_loading->assignProperty(this, "canReplaceThreshold", false);

	m_loadingSidecar->assignProperty(this, "canDefineCrop", false);
	m_loadingSidecar->assignProperty(this, "canSaveCrop", false);
	m_loadingSidecar->assignProperty(this, "canReplaceThreshold", false);

	m_computingThreshold->assignProperty(this, "canDefineCrop", false);
	m_computingThreshold->assignProperty(this, "canSaveCrop", false);
	m_computingThreshold->assignProperty(this, "canReplaceThreshold", false);

	m_definingCrop->assignProperty(this, "canDefineCrop", true);
	m_definingCrop->assignProperty(this, "canSaveCrop", true);
	m_definingCrop->assignProperty(this, "canReplaceThreshold", false);

	m_loadingCropped->assignProperty(this, "canDefineCrop", false);
	m_loadingCropped->assignProperty(this, "canSaveCrop", false);
	m_loadingCropped->assignProperty(this, "canReplaceThreshold", false);

	m_replacingThreshold->assignProperty(this, "canDefineCrop", false);
	m_replacingThreshold->assignProperty(this, "canSaveCrop", false);
	m_replacingThreshold->assignProperty(this, "canReplaceThreshold", true);
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

void WorkflowStateMachine::setCanReplaceThreshold(bool can)
{
	if (m_canReplaceThreshold != can) {
		m_canReplaceThreshold = can;
		emit canReplaceThresholdChanged(can);
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
		case WorkflowStateMachine::ReplacingThreshold:
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
	if (config.contains(m_replacingThreshold)) return ReplacingThreshold;
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
	connectState(m_replacingThreshold, ReplacingThreshold);
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
	case ReplacingThreshold:   return m_replacingThreshold;
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
	case ReplacingThreshold: return QStringLiteral("ReplacingThreshold");
	case Completed: return QStringLiteral("Completed");
	case ErrorState: return QStringLiteral("ErrorState");
	default: return QStringLiteral("Unknown");
	}
}

void WorkflowStateMachine::start()
{
	// If the Qt state machine reached QFinalState, it stops running.
	// Restart engine first, then emit started so Idle->Loading transition is processed.
	if (m_machine && !m_machine->isRunning()) {
		m_machine->start();

		// Defer started emission to next turn so the machine has entered Idle.
		QMetaObject::invokeMethod(this, [this]() {
			emit started();
		}, Qt::QueuedConnection);
		return;
	}

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
	capabilities.insert(QStringLiteral("canReplaceThreshold"), m_canReplaceThreshold);
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
		setCanReplaceThreshold(caps.value(QStringLiteral("canReplaceThreshold")).toBool());
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

	// Notify consumers: project loaded (for recents/menu).
	emit projectLoaded(sidecarPath);

	// Choose the best initial image:
	// Prefer an existing crop output if present in the sidecar.
	// Otherwise open the canonical source image.
	QString initialPath = src;

	const QJsonObject cropObj = obj.value(QStringLiteral("crop")).toObject();
	const QString cropPath = cropObj.value(QStringLiteral("outputPath")).toString();
	if (!cropPath.isEmpty() && QFile::exists(cropPath)) {
		initialPath = cropPath;

		// Prime derived tracking so MainWindow treats this as "opening derived" and emits croppedLoaded().
		// This reduces unnecessary source loads while keeping the "landmarks must be on crop" invariant.
		m_lastDerivedPath = cropPath;
		m_isDerived = true;
		m_derivedFrom = src;
	}

	// IMPORTANT: do not suggest a UI state here; workflow logic will drive it.
	emit requestOpenImage(initialPath);

	// We do not start/resume the internal machine here because MainWindow will call resume()
	// and the workflow history will choose the correct state.
	return true;
}

bool WorkflowStateMachine::readSidecarForInput()
{
	// Reset previous sidecar-derived state first.
	m_sidecar = QJsonObject();
	m_isDerived = false;
	m_derivedFrom.clear();
	m_lastDerivedPath.clear();

	if (m_inputFile.isEmpty()) {
		return false;
	}

	QJsonObject side = JsonUtils::readJsonSidecar(m_inputFile);
	if (side.isEmpty()) {
		m_isDerived = false;
		return false;
	}

	m_sidecar = side;

	// Emit threshold presence/state from canonical+fallback parsing.
	{
		const auto [present, val] = parseThreshold(m_sidecar);
		emit thresholdChanged(present, val);
	}

	// Determine source and crop output from sidecar content.
	const QString sourcePath = m_sidecar.value(QStringLiteral("source")).toString();
	QString cropPath;
	if (sidecarHasCropOutput(&cropPath) && !cropPath.isEmpty()) {
		m_lastDerivedPath = cropPath;
	}

	// Infer derived status by comparing current input path to crop output path.
	if (!cropPath.isEmpty()) {
		const QString inputAbs = QFileInfo(m_inputFile).absoluteFilePath();
		const QString cropAbs = QFileInfo(cropPath).absoluteFilePath();
		if (!inputAbs.isEmpty() && inputAbs == cropAbs) {
			m_isDerived = true;
			m_derivedFrom = sourcePath;
		}
	}

	// Treat as project-sidecar when current input is not the derived crop output.
	if (!m_isDerived) {
		const QString sidecarPath = JsonUtils::sidecarPathForImage(m_inputFile);
		emit projectLoaded(sidecarPath);
	}

	return true;
}
bool WorkflowStateMachine::appendHistoryToSidecar(const QString& imagePath, const QString& stepName, const QJsonObject& params)
{
	if (imagePath.isEmpty() || stepName.isEmpty()) return false;

	// Load existing sidecar (may return empty; start a new one)
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
	// If operation produced a derived path, mark completed.
	// Special-case: operations that record meaningful parameters (e.g., a computed threshold)
	// should be marked completed even if they don't produce a derived file.
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

	// duplicate: treat as success but don't write again
	return true;
}

bool WorkflowStateMachine::writeCropSidecarForOutput(const QString& outPath, const QJsonObject& params)
{
	if (outPath.isEmpty()) {
		return false;
	}
	if (m_inputFile.isEmpty()) {
		return false;
	}

	QJsonObject side = JsonUtils::readJsonSidecar(m_inputFile);
	if (side.isEmpty()) {
		return false;
	}

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
	workflow.insert(QStringLiteral("currentStep"), QStringLiteral("replace_threshold"));
	side.insert(QStringLiteral("workflow"), workflow);

	side.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

	if (!JsonUtils::writeJsonSidecar(m_inputFile, side)) {
		emit sidecarWriteFailed(outPath, QStringLiteral("Failed to write crop info to sidecar"));
		return false;
	}

	m_lastDerivedPath = outPath;
	m_isDerived = true;
	m_derivedFrom = QFileInfo(m_inputFile).absoluteFilePath();
	m_sidecar = JsonUtils::readJsonSidecar(m_inputFile);

	emit sidecarWritten(JsonUtils::sidecarPathForImage(m_inputFile));
	return true;
}

std::pair<bool, double> WorkflowStateMachine::parseThreshold(const QJsonObject& side) const
{
	// 1) Canonical threshold object first.
	const QJsonObject thr = side.value(QStringLiteral("threshold")).toObject();
	if (!thr.isEmpty()) {
		const QJsonValue v = thr.value(QStringLiteral("value"));
		if (v.isDouble()) {
			return { true, v.toDouble() };
		}
		if (v.isString()) {
			bool ok = false;
			const double dv = v.toString().toDouble(&ok);
			if (ok) {
				return { true, dv };
			}
		}
	}

	// 2) Backward-compatible fallback to operations[].parameters.threshold
	if (!side.contains(QStringLiteral("operations")) || !side.value(QStringLiteral("operations")).isArray()) {
		return { false, std::numeric_limits<double>::quiet_NaN() };
	}

	const QJsonArray ops = side.value(QStringLiteral("operations")).toArray();
	for (const QJsonValue& v : ops) {
		if (!v.isObject()) {
			continue;
		}
		const QJsonObject op = v.toObject();
		const QJsonObject params = op.value(QStringLiteral("parameters")).toObject();
		if (!params.contains(QStringLiteral("threshold"))) {
			continue;
		}

		const QJsonValue tv = params.value(QStringLiteral("threshold"));
		if (tv.isDouble()) {
			return { true, tv.toDouble() };
		}
		if (tv.isString()) {
			bool ok = false;
			const double dv = tv.toString().toDouble(&ok);
			if (ok) {
				return { true, dv };
			}
		}
	}

	return { false, std::numeric_limits<double>::quiet_NaN() };
}

bool WorkflowStateMachine::sidecarHasThreshold() const
{
	if (m_inputFile.isEmpty()) {
		return false;
	}

	QJsonObject side;
	if (!m_sidecar.isEmpty()) {
		side = m_sidecar;
	}
	else {
		side = JsonUtils::readJsonSidecar(m_inputFile);
	}

	if (side.isEmpty()) {
		return false;
	}

	const auto [present, _] = parseThreshold(side);
	Q_UNUSED(_);
	return present;
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
	if (m_inputFile.isEmpty()) {
		return false;
	}

	QJsonObject side = JsonUtils::readJsonSidecar(m_inputFile);
	if (side.isEmpty()) {
		return false;
	}

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

	// Initial threshold compute => next stage is crop.
	// Threshold replacement on cropped image => workflow completed.
	const State s = currentState();
	if (s == WorkflowStateMachine::ReplacingThreshold) {
		workflow.insert(QStringLiteral("currentStep"), QStringLiteral("completed"));
	}
	else {
		workflow.insert(QStringLiteral("currentStep"), QStringLiteral("crop"));
	}

	side.insert(QStringLiteral("workflow"), workflow);
	side.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

	if (!JsonUtils::writeJsonSidecar(m_inputFile, side)) {
		return false;
	}

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

	const State s = currentState();
	if (s == WorkflowStateMachine::ComputingThreshold) {
		emit thresholdReady();
	}
	else if (s == WorkflowStateMachine::ReplacingThreshold) {
		emit thresholdReplaced();
	}
	else {
		// Fallback for out-of-band calls
		emit thresholdReady();
	}
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

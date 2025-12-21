#include "ImageProcessingStateMachine.h"
#include "JsonUtils.h"

#include <QStateMachine>
#include <QState>
#include <QFinalState>
#include <QJsonArray>
#include <QFuture>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <QSaveFile>
#include <QJsonDocument>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QJsonObject>

ImageProcessingStateMachine::ImageProcessingStateMachine(QObject* parent)
	: QObject(parent)
{
	m_machine = new QStateMachine(this);

	// create states
	m_idle = new QState(m_machine);
	m_loading = new QState(m_machine);
	m_definingCrop = new QState(m_machine);
	// m_applyingCrop removed
	m_loadingCropped = new QState(m_machine);
	m_placingFiducials = new QState(m_machine);
	m_interactiveRotation = new QState(m_machine);
	m_applyingRotation = new QState(m_machine);
	m_loadingRotated = new QState(m_machine);
	m_computingThreshold = new QState(m_machine);
	m_segmenting = new QState(m_machine);
	m_saving = new QState(m_machine);
	m_final = new QFinalState(m_machine);

	// transitions (use old SIGNAL macro for readability in QState)
	m_idle->addTransition(this, SIGNAL(started()), m_loading);
	m_loading->addTransition(this, SIGNAL(imageLoaded()), m_definingCrop);

	// Previously: defining->applying->loadingCropped
	// Apply step removed: transition directly from DefiningCrop -> LoadingCropped on cropApplied
	m_definingCrop->addTransition(this, SIGNAL(cropApplied()), m_loadingCropped);

	m_loadingCropped->addTransition(this, SIGNAL(croppedLoaded()), m_placingFiducials);
	m_placingFiducials->addTransition(this, SIGNAL(fiducialsPlaced()), m_interactiveRotation);
	m_interactiveRotation->addTransition(this, SIGNAL(interactiveRotationFinished()), m_applyingRotation);
	m_applyingRotation->addTransition(this, SIGNAL(rotationApplied()), m_loadingRotated);
	m_loadingRotated->addTransition(this, SIGNAL(rotatedLoaded()), m_computingThreshold);
	m_computingThreshold->addTransition(this, SIGNAL(thresholdComputed()), m_segmenting);
	m_segmenting->addTransition(this, SIGNAL(segmentationDone()), m_saving);
	m_saving->addTransition(this, SIGNAL(saved()), m_final);

	// cancel from most states -> idle
	QList<QState*> cancellable = { m_loading, m_definingCrop, m_loadingCropped,
								   m_placingFiducials, m_interactiveRotation, m_applyingRotation,
								   m_loadingRotated, m_computingThreshold, m_segmenting, m_saving };
	for (QState* s : cancellable) {
		QSignalTransition* t = new QSignalTransition(this, SIGNAL(canceled()));
		t->setTargetState(m_idle);
		s->addTransition(t);
	}

	// on entry of each state emit request signals so external logic starts work
	connect(m_loading, &QState::entered, this, [this]() {
		m_currentState = LoadingImage;
		m_active.store(true);
		emit stateChanged(m_currentState);
		emit requestLoadImage();
	});
	connect(m_definingCrop, &QState::entered, this, [this]() {
		m_currentState = DefiningCrop;
		emit stateChanged(m_currentState);
		emit requestDefineCrop();
	});
	// m_applyingCrop entry handler removed
	connect(m_loadingCropped, &QState::entered, this, [this]() {
		m_currentState = LoadingCropped;
		emit stateChanged(m_currentState);
		emit requestLoadCropped();
	});
	connect(m_placingFiducials, &QState::entered, this, [this]() {
		m_currentState = PlacingFiducials;
		emit stateChanged(m_currentState);
		emit requestPlaceFiducials();
	});
	connect(m_interactiveRotation, &QState::entered, this, [this]() {
		m_currentState = InteractiveRotation;
		emit stateChanged(m_currentState);
		emit requestStartInteractiveRotation();
	});
	connect(m_applyingRotation, &QState::entered, this, [this]() {
		m_currentState = ApplyingRotation;
		emit stateChanged(m_currentState);
		emit requestApplyRotation();
	});
	connect(m_loadingRotated, &QState::entered, this, [this]() {
		m_currentState = LoadingRotated;
		emit stateChanged(m_currentState);
		emit requestLoadRotated();
	});
	connect(m_computingThreshold, &QState::entered, this, [this]() {
		m_currentState = ComputingThreshold;
		emit stateChanged(m_currentState);
		emit requestComputeThreshold();
	});
	connect(m_segmenting, &QState::entered, this, [this]() {
		m_currentState = Segmenting;
		emit stateChanged(m_currentState);
		emit requestSegment();
	});
	connect(m_saving, &QState::entered, this, [this]() {
		m_currentState = SavingSegment;
		emit stateChanged(m_currentState);
		emit requestSaveSegment();
	});

	// idle entered
	connect(m_idle, &QState::entered, this, [this]() {
		m_currentState = Idle;
		m_active.store(false);
		emit stateChanged(m_currentState);
	});

	// final state -> finished; reset to idle automatically so machine is reusable
	connect(m_final, &QFinalState::entered, this, [this]() {
		m_currentState = Completed;
		m_active.store(false);
		emit stateChanged(m_currentState);
		emit finished();
		// reset to idle
		m_machine->stop();
		m_machine->setInitialState(m_idle);
		m_machine->start();
	});

	// failure handling: forward error and reset to idle
	connect(this, &ImageProcessingStateMachine::failed, this, [this](const QString& reason) {
		m_currentState = ErrorState;
		m_active.store(false);
		emit stateChanged(m_currentState);
		emit error(reason);
		// stop and restart in idle so the machine can accept a new start
		m_machine->stop();
		m_machine->setInitialState(m_idle);
		m_machine->start();
	});

	m_machine->setInitialState(m_idle);
	m_machine->start();

	// When UI/worker notifies that the crop was defined, ask the application to save it automatically.
	// The application (MainWindow) should perform the save and then call notifyCropApplied()/notifyCroppedLoaded().
	connect(this, &ImageProcessingStateMachine::cropDefined, this, [this]() {
		emit requestSaveCropped();
	});
}

void ImageProcessingStateMachine::start() { Q_EMIT started(); }
void ImageProcessingStateMachine::cancel() { Q_EMIT canceled(); }

bool ImageProcessingStateMachine::loadProjectSidecarFile(const QString& sidecarPath)
{
	if (sidecarPath.isEmpty()) return false;
	QFile f(sidecarPath);
	if (!f.open(QIODevice::ReadOnly)) {
		qWarning() << "ImageProcessingStateMachine::loadProjectSidecarFile: failed to open" << sidecarPath;
		return false;
	}
	const QByteArray data = f.readAll();
	f.close();
	const QJsonDocument doc = QJsonDocument::fromJson(data);
	if (!doc.isObject()) {
		qWarning() << "ImageProcessingStateMachine::loadProjectSidecarFile: invalid json in" << sidecarPath;
		return false;
	}
	QJsonObject obj = doc.object();

	// Persist parsed sidecar in-memory for later inspection by other helpers.
	m_sidecar = obj;

	// Determine the last produced derived output from the new "operations" model.
	QString lastOutputPath;
	QString lastStepName;
	if (obj.contains(QStringLiteral("operations")) && obj.value(QStringLiteral("operations")).isArray()) {
		QJsonArray ops = obj.value(QStringLiteral("operations")).toArray();
		for (int i = ops.size() - 1; i >= 0; --i) {
			QJsonValue v = ops.at(i);
			if (!v.isObject()) continue;
			QJsonObject entry = v.toObject();
			// Expect operation object to contain 'name', optional 'derived' and 'status'.
			if (entry.contains(QStringLiteral("derived")) && entry.value(QStringLiteral("derived")).isString()) {
				const QString cand = entry.value(QStringLiteral("derived")).toString();
				if (!cand.isEmpty() && QFile::exists(cand)) {
					lastOutputPath = cand;
					lastStepName = entry.value(QStringLiteral("name")).toString();
					break;
				}
			}
		}
	}

	// Determine source image (canonical)
	QString src;
	if (obj.contains(QStringLiteral("source")) && obj.value(QStringLiteral("source")).isString())
		src = obj.value(QStringLiteral("source")).toString();

	// Decide which image to open: prefer last processed output, then source
	QString toOpen;
	if (!lastOutputPath.isEmpty()) {
		toOpen = lastOutputPath;
		// record last derived path for provenance
		m_lastDerivedPath = lastOutputPath;
		m_isDerived = true;
	}
	else if (!src.isEmpty() && QFile::exists(src)) {
		toOpen = src;
		// set input file to source so subsequent readSidecarForInput / notifyImageLoaded work correctly
		m_inputFile = src;
		m_isDerived = false;
	}

	// Suggest UI state based on lastStepName / last operation (so MainWindow can pre-open the right panel)
	ImageProcessingStateMachine::State suggested = ImageProcessingStateMachine::Idle;
	const QString step = lastStepName.toLower();
	const QString op = obj.value(QStringLiteral("operation")).toString().toLower();
	if (step.contains(QStringLiteral("crop")) || op.contains(QStringLiteral("crop"))) {
		suggested = ImageProcessingStateMachine::PlacingFiducials;
	}
	else if (step.contains(QStringLiteral("place")) || step.contains(QStringLiteral("fiducial"))) {
		suggested = ImageProcessingStateMachine::PlacingFiducials;
	}
	else if (step.contains(QStringLiteral("rotate")) || op.contains(QStringLiteral("rotate"))) {
		suggested = ImageProcessingStateMachine::InteractiveRotation;
	}
	else if (step.contains(QStringLiteral("segment")) || step.contains(QStringLiteral("threshold")) || op.contains(QStringLiteral("segment"))) {
		suggested = ImageProcessingStateMachine::ComputingThreshold;
	}
	else {
		suggested = ImageProcessingStateMachine::Idle;
	}

	// Notify consumers: project loaded, ask UI to open appropriate image, and suggest UI state
	emit projectLoaded(sidecarPath);
	emit suggestedState(suggested);

	if (!toOpen.isEmpty()) {
		emit requestOpenImage(toOpen);
	}

	return true;
}

void ImageProcessingStateMachine::notifyImageLoaded()
{
	// Ensure we have sidecar data for the current input
	if (m_sidecar.isEmpty()) {
		readSidecarForInput();
	}

	// New: inspect 'operations' array and use last operation to decide if the loaded image is a derived output.
	if (m_sidecar.contains(QStringLiteral("operations")) && m_sidecar.value(QStringLiteral("operations")).isArray()) {
		QJsonArray ops = m_sidecar.value(QStringLiteral("operations")).toArray();
		if (!ops.isEmpty()) {
			QJsonObject last = ops.last().toObject();
			const QString name = last.value(QStringLiteral("name")).toString().toLower();
			const QString status = last.value(QStringLiteral("status")).toString().toLower();
			const QString derived = last.value(QStringLiteral("derived")).toString();

			// If last operation was a crop (or other operation producing a derived file) and the derived
			// path matches the currently-loaded input, treat it as croppedLoaded (i.e., derived image).
			if (!derived.isEmpty() && QFile::exists(derived) && (m_inputFile == derived || m_lastDerivedPath == derived)) {
				m_lastDerivedPath = derived;
				emit croppedLoaded();
				return;
			}

			// Optionally, handle other completed operation types that should map to specific transitions:
			// e.g., if name == "rotate" && status == "completed" -> emit rotatedLoaded(), etc.
			if (name == QStringLiteral("rotate") && status == QStringLiteral("completed") && !derived.isEmpty() && QFile::exists(derived)) {
				m_lastDerivedPath = derived;
				emit rotatedLoaded();
				return;
			}
			// add more mappings as needed
		}
	}

	// Fallback: if no operations array information indicates derived, emit normal imageLoaded
	emit imageLoaded();
}

void ImageProcessingStateMachine::notifyCropDefined() { Q_EMIT cropDefined(); }
void ImageProcessingStateMachine::notifyCropApplied() { Q_EMIT cropApplied(); }
void ImageProcessingStateMachine::notifyCroppedLoaded() { Q_EMIT croppedLoaded(); }
void ImageProcessingStateMachine::notifyFiducialsPlaced() { Q_EMIT fiducialsPlaced(); }
void ImageProcessingStateMachine::notifyInteractiveRotationFinished() { Q_EMIT interactiveRotationFinished(); }
void ImageProcessingStateMachine::notifyRotationApplied() { Q_EMIT rotationApplied(); }
void ImageProcessingStateMachine::notifyRotatedLoaded() { Q_EMIT rotatedLoaded(); }
void ImageProcessingStateMachine::notifyThresholdComputed() { Q_EMIT thresholdComputed(); }
void ImageProcessingStateMachine::notifySegmentationDone() { Q_EMIT segmentationDone(); }
void ImageProcessingStateMachine::notifySaved() { Q_EMIT saved(); }
void ImageProcessingStateMachine::notifyFailed(const QString& reason) { Q_EMIT failed(reason); }

QAbstractState* ImageProcessingStateMachine::stateForEnum(State s) const
{
	switch (s) {
		case Idle:                 return m_idle;
		case LoadingImage:         return m_loading;
		case DefiningCrop:         return m_definingCrop;
		case LoadingCropped:       return m_loadingCropped;
		case PlacingFiducials:     return m_placingFiducials;
		case InteractiveRotation:  return m_interactiveRotation;
		case ApplyingRotation:     return m_applyingRotation;
		case LoadingRotated:       return m_loadingRotated;
		case ComputingThreshold:   return m_computingThreshold;
		case Segmenting:           return m_segmenting;
		case SavingSegment:        return m_saving;
		case Completed:            return m_final;
		case ErrorState:           return m_idle; // map Error->idle for external-transition purposes
		default:                   return nullptr;
	}
}

// --- Sidecar / provenance helpers (implementations) ---

bool ImageProcessingStateMachine::readSidecarForInput()
{
	// No-op if input path not set
	if (m_inputFile.isEmpty()) return false;

	// Read canonical sidecar (JsonUtils only reads canonical path; no legacy checks)
	QJsonObject doc = JsonUtils::readJsonSidecar(m_inputFile);
	if (doc.isEmpty()) {
		m_sidecar = QJsonObject();
		m_isDerived = false;
		m_derivedFrom.clear();
		m_lastDerivedPath.clear();
		return false;
	}

	// Store parsed sidecar
	m_sidecar = doc;

	// Default flags
	m_isDerived = false;
	m_derivedFrom.clear();
	m_lastDerivedPath.clear();

	// If operations array present, inspect last operation for provenance
	if (doc.contains(QStringLiteral("operations")) && doc.value(QStringLiteral("operations")).isArray()) {
		QJsonArray ops = doc.value(QStringLiteral("operations")).toArray();
		if (!ops.isEmpty()) {
			QJsonObject lastOp = ops.last().toObject();
			const QString status = lastOp.value(QStringLiteral("status")).toString().toLower();
			const QString derived = lastOp.value(QStringLiteral("derived")).toString();
			if (!derived.isEmpty() && status == QStringLiteral("completed") && QFile::exists(derived)) {
				m_isDerived = true;
				m_lastDerivedPath = derived;
				if (doc.contains(QStringLiteral("source")) && doc.value(QStringLiteral("source")).isString()) {
					m_derivedFrom = doc.value(QStringLiteral("source")).toString();
				}
			}
		}
	}

	return true;
}

bool ImageProcessingStateMachine::appendHistoryToSidecar(const QString& imagePath, const QString& stepName, const QJsonObject& params)
{
	if (imagePath.isEmpty() || stepName.isEmpty()) return false;

	// Load existing sidecar (may return empty -> start a new one)
	QJsonObject meta = JsonUtils::readJsonSidecar(imagePath);

	// Ensure minimal meta exists
	if (meta.isEmpty()) {
		// create a minimal header if none exists
		meta = QJsonObject();
	}

	// Build new operation entry
	QJsonObject entry;
	entry.insert(QStringLiteral("name"), stepName);

	QString derived;
	if (params.contains(QStringLiteral("derived")) && params.value(QStringLiteral("derived")).isString())
		derived = params.value(QStringLiteral("derived")).toString();
	else if (params.contains(QStringLiteral("cropped_path")) && params.value(QStringLiteral("cropped_path")).isString())
		derived = params.value(QStringLiteral("cropped_path")).toString();
	else if (params.contains(QStringLiteral("output")) && params.value(QStringLiteral("output")).isString())
		derived = params.value(QStringLiteral("output")).toString();

	if (!derived.isEmpty()) {
		entry.insert(QStringLiteral("status"), QStringLiteral("completed"));
		entry.insert(QStringLiteral("derived"), derived);
	}
	else {
		entry.insert(QStringLiteral("status"), QStringLiteral("pending"));
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
		// Emit canonical sidecar path (not the image path)
		const QString sidecar = JsonUtils::sidecarPathForImage(imagePath);
		emit sidecarWritten(sidecar);
		return true;
	}

	// duplicate — treat as success but don't write again
	return true;
}

bool ImageProcessingStateMachine::writeCropSidecarForOutput(const QString& outPath, const QJsonObject& params)
{
	if (outPath.isEmpty()) return false;

	// Determine source: prefer the current input path, fall back to recorded derived-from
	QString source = m_inputFile;
	if (source.isEmpty()) source = m_derivedFrom;
	if (source.isEmpty()) {
		emit sidecarWriteFailed(outPath, QStringLiteral("No source image recorded for sidecar"));
		return false;
	}

	// Use JsonUtils to write a canonical crop sidecar for the output image
	const bool ok = JsonUtils::writeCropSidecar(outPath, source, params);
	if (!ok) {
		emit sidecarWriteFailed(outPath, QStringLiteral("Failed to write crop sidecar"));
		return false;
	}

	// Update internal provenance tracking
	m_lastDerivedPath = outPath;
	m_isDerived = true;
	m_derivedFrom = source;

	// NOTE: JsonUtils::writeCropSidecar already created a completed crop op in the canonical sidecar.
	// Do NOT call appendHistoryToSidecar() here because that would append a duplicate entry.

	// Emit the canonical sidecar path (JSON file), not the derived image path.
	const QString sidecar = JsonUtils::sidecarPathForImage(outPath);
	emit sidecarWritten(sidecar);
	return true;
}
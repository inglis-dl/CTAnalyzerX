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
#include <QJsonValue>
#include <limits>
#include <utility>

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

	// Inspect sidecar for an explicit primary threshold and notify listeners.
	auto [present, val] = parsePrimaryThreshold(m_sidecar);
	emit primaryThresholdChanged(present, val);

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
	auto [present, val] = parsePrimaryThreshold(m_sidecar);
	emit primaryThresholdChanged(present, val);

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
					// (we only mark derived when the sidecar itself indicates "source").
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

	// If this operation is a computed primary threshold for a source image,
	// ensure the sidecar records the canonical source image so reopening the
	// project will load the correct primary image later.
	const QString canonicalSource = QFileInfo(imagePath).absoluteFilePath();
	if (stepName == QStringLiteral("compute_primary_threshold")) {
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
	else if (stepName == QStringLiteral("compute_primary_threshold") || params.contains(QStringLiteral("threshold"))) {
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
			auto [present2, val2] = parsePrimaryThreshold(meta);
			emit primaryThresholdChanged(present2, val2);
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
	// Attempt to read the written canonical sidecar and notify listeners about threshold presence.
	{
		QJsonObject side = JsonUtils::readJsonSidecar(outPath);
		if (!side.isEmpty()) {
			auto [present, val] = parsePrimaryThreshold(side);
			emit primaryThresholdChanged(present, val);
		}
	}
	emit sidecarWritten(sidecar);
	return true;
}

std::pair<bool, double> ImageProcessingStateMachine::parsePrimaryThreshold(const QJsonObject& side) const
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

QString ImageProcessingStateMachine::stateToString(State s)
{
	switch (s) {
		case Idle: return QStringLiteral("Idle");
		case LoadingImage: return QStringLiteral("LoadingImage");
		case DefiningCrop: return QStringLiteral("DefiningCrop");
		case LoadingCropped: return QStringLiteral("LoadingCropped");
		case PlacingFiducials: return QStringLiteral("PlacingFiducials");
		case InteractiveRotation: return QStringLiteral("InteractiveRotation");
		case ApplyingRotation: return QStringLiteral("ApplyingRotation");
		case LoadingRotated: return QStringLiteral("LoadingRotated");
		case ComputingThreshold: return QStringLiteral("ComputingThreshold");
		case Segmenting: return QStringLiteral("Segmenting");
		case SavingSegment: return QStringLiteral("SavingSegment");
		case Completed: return QStringLiteral("Completed");
		case ErrorState: return QStringLiteral("ErrorState");
		default: return QStringLiteral("Unknown");

	}
}

bool ImageProcessingStateMachine::sidecarHasPrimaryThreshold() const
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
		if (name == QStringLiteral("compute_primary_threshold") && params.contains(QStringLiteral("threshold")))
			return true;
		// Accept any op that records a 'threshold' parameter (conservative)
		if (params.contains(QStringLiteral("threshold")))
			return true;

	}
	return false;
}
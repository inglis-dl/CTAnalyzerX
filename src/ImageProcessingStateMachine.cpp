#include "ImageProcessingStateMachine.h"
#include "JsonUtils.h"

#include <QSignalTransition>
#include <QList>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QDir>

QAbstractState* ImageProcessingStateMachine::stateForEnum(State s) const
{
	switch (s) {
		case Idle:               return m_idle;
		case LoadingImage:       return m_loading;
		case DefiningCrop:       return m_definingCrop;
		case LoadingCropped:     return m_loadingCropped; // ApplyingCrop removed
		case PlacingFiducials:   return m_placingFiducials;
		case InteractiveRotation:return m_interactiveRotation;
		case ApplyingRotation:   return m_applyingRotation;
		case LoadingRotated:     return m_loadingRotated;
		case ComputingThreshold: return m_computingThreshold;
		case Segmenting:         return m_segmenting;
		case SavingSegment:      return m_saving;
		case Completed:          return m_final; // QFinalState* -> QAbstractState* ok
		default:                 return nullptr;
	}
}

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

// New: read sidecar for current input file into m_sidecar and set derived flags
bool ImageProcessingStateMachine::readSidecarForInput()
{
	if (m_inputFile.isEmpty()) {
		m_sidecar = QJsonObject();
		m_isDerived = false;
		m_derivedFrom.clear();
		return false;
	}
	m_sidecar = JsonUtils::readJsonSidecar(m_inputFile);
	if (m_sidecar.isEmpty()) {
		m_isDerived = false;
		m_derivedFrom.clear();
		return false;
	}
	const QString op = m_sidecar.value(QStringLiteral("operation")).toString();
	m_isDerived = !op.isEmpty();
	m_derivedFrom = m_sidecar.value(QStringLiteral("derived_from")).toString();
	return true;
}

// Add near other helper implementations (e.g., notifyCropDefined...) — implement sidecar helpers.
bool ImageProcessingStateMachine::writeCropSidecarForOutput(const QString& outPath, const QJsonObject& params)
{
	// Write a sidecar JSON next to the produced output file describing the crop.
	// Also append an entry to the original input image's sidecar history (if available).
	if (outPath.isEmpty()) return false;

	// Prepare sidecar object for the derived output
	QJsonObject outSidecar = params; // copy provided params
	outSidecar.insert(QStringLiteral("derived_from"), m_inputFile);
	outSidecar.insert(QStringLiteral("writer_timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

	// Write output sidecar next to outPath (basename.json)
	QFileInfo outFi(outPath);
	const QString outSidecarPath = QDir(outFi.absolutePath()).filePath(outFi.completeBaseName() + QStringLiteral(".json"));

	QFile outFile(outSidecarPath);
	if (!outFile.open(QIODevice::WriteOnly)) {
		qWarning() << "ImageProcessingStateMachine::writeCropSidecarForOutput: failed to open" << outSidecarPath;
		return false;
	}
	QJsonDocument outDoc(outSidecar);
	outFile.write(outDoc.toJson(QJsonDocument::Indented));
	outFile.close();

	// Append a history entry into the original input's sidecar so provenance is preserved.
	if (!m_inputFile.isEmpty()) {
		QJsonObject histParams = params;
		histParams.insert(QStringLiteral("output"), outPath);
		bool ok = appendHistoryToSidecar(m_inputFile, QStringLiteral("crop"), histParams);
		if (!ok) {
			qWarning() << "ImageProcessingStateMachine::writeCropSidecarForOutput: failed to append history to input sidecar for" << m_inputFile;
			// still consider success for writing the output sidecar
		}
	}

	return true;
}

bool ImageProcessingStateMachine::appendHistoryToSidecar(const QString& imagePath, const QString& stepName, const QJsonObject& params)
{
	if (imagePath.isEmpty() || stepName.isEmpty()) return false;

	QFileInfo fi(imagePath);
	const QString sidecarPath = QDir(fi.absolutePath()).filePath(fi.completeBaseName() + QStringLiteral(".json"));

	// Read existing sidecar if present
	QJsonObject rootObj;
	if (QFile::exists(sidecarPath)) {
		QFile in(sidecarPath);
		if (in.open(QIODevice::ReadOnly)) {
			const QByteArray data = in.readAll();
			in.close();
			QJsonDocument doc = QJsonDocument::fromJson(data);
			if (doc.isObject()) rootObj = doc.object();
		}
	}

	// Prepare history entry
	QJsonObject entry;
	entry.insert(QStringLiteral("step"), stepName);
	entry.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
	entry.insert(QStringLiteral("params"), params);

	// Append to history array
	QJsonArray history = rootObj.value(QStringLiteral("history")).toArray();
	history.append(entry);
	rootObj.insert(QStringLiteral("history"), history);

	// Persist sidecar
	QFile out(sidecarPath);
	if (!out.open(QIODevice::WriteOnly)) {
		qWarning() << "ImageProcessingStateMachine::appendHistoryToSidecar: failed to open" << sidecarPath;
		return false;
	}
	QJsonDocument outDoc(rootObj);
	out.write(outDoc.toJson(QJsonDocument::Indented));
	out.close();
	return true;
}

// Public: add an external signal->state transition
bool ImageProcessingStateMachine::addExternalTransition(State from, State to, QObject* sender, const char* signal)
{
	if (!m_machine || !sender || !signal) return false;
	QAbstractState* sourceAbs = stateForEnum(from);
	QAbstractState* targetAbs = stateForEnum(to);
	if (!sourceAbs || !targetAbs) {
		qWarning() << "ImageProcessingStateMachine::addExternalTransition: invalid state mapping" << from << to;
		return false;
	}
	// QSignalTransition::setTargetState accepts QAbstractState*
	QSignalTransition* t = new QSignalTransition(sender, signal);
	t->setTargetState(targetAbs);
	// Only QState (not QFinalState) has addTransition(), so cast and add if possible.
	QState* sourceState = qobject_cast<QState*>(sourceAbs);
	if (!sourceState) {
		qWarning() << "ImageProcessingStateMachine::addExternalTransition: 'from' state is not a QState";
		delete t;
		return false;
	}
	sourceState->addTransition(t);
	return true;
}

// Notify image loaded; inspect sidecar to decide whether this is a derived/cropped image
void ImageProcessingStateMachine::notifyImageLoaded()
{
	// Ensure we have sidecar data for the current input
	if (m_sidecar.isEmpty()) {
		readSidecarForInput();
	}

	const QString op = m_sidecar.value(QStringLiteral("operation")).toString();
	if (!op.isEmpty() && op == QStringLiteral("crop")) {
		// This is a derived cropped volume (treat as cropped-loaded)
		m_lastDerivedPath = m_inputFile;
		emit croppedLoaded();
		return;
	}

	// normal full/original image
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
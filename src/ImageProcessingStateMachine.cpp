#include "ImageProcessingStateMachine.h"
#include <QSignalTransition>
#include <QList>

ImageProcessingStateMachine::ImageProcessingStateMachine(QObject* parent)
	: QObject(parent)
{
	m_machine = new QStateMachine(this);

	// create states
	m_idle = new QState(m_machine);
	m_loading = new QState(m_machine);
	m_definingCrop = new QState(m_machine);
	m_applyingCrop = new QState(m_machine);
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
	m_definingCrop->addTransition(this, SIGNAL(cropDefined()), m_applyingCrop);
	m_applyingCrop->addTransition(this, SIGNAL(cropApplied()), m_loadingCropped);
	m_loadingCropped->addTransition(this, SIGNAL(croppedLoaded()), m_placingFiducials);
	m_placingFiducials->addTransition(this, SIGNAL(fiducialsPlaced()), m_interactiveRotation);
	m_interactiveRotation->addTransition(this, SIGNAL(interactiveRotationFinished()), m_applyingRotation);
	m_applyingRotation->addTransition(this, SIGNAL(rotationApplied()), m_loadingRotated);
	m_loadingRotated->addTransition(this, SIGNAL(rotatedLoaded()), m_computingThreshold);
	m_computingThreshold->addTransition(this, SIGNAL(thresholdComputed()), m_segmenting);
	m_segmenting->addTransition(this, SIGNAL(segmentationDone()), m_saving);
	m_saving->addTransition(this, SIGNAL(saved()), m_final);

	// cancel from most states -> idle
	QList<QState*> cancellable = { m_loading, m_definingCrop, m_applyingCrop, m_loadingCropped,
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
	connect(m_applyingCrop, &QState::entered, this, [this]() {
		m_currentState = ApplyingCrop;
		emit stateChanged(m_currentState);
		emit requestApplyCrop();
	});
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
}

void ImageProcessingStateMachine::start() { Q_EMIT started(); }
void ImageProcessingStateMachine::cancel() { Q_EMIT canceled(); }

void ImageProcessingStateMachine::notifyImageLoaded() { Q_EMIT imageLoaded(); }
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
#pragma once

#include <utility>
#include <QObject>
#include <QAtomicInteger>
#include <QString>

class vtkImageData;

// Compute Otsu threshold from an already-loaded VTK image (produced by ImageLoader).
// The function will attempt to honor cooperative cancellation via the optional cancel flag.
// Returns {ok, threshold}. Safe to call from a worker thread.
std::pair<bool, double> computeOtsuThresholdFromVtk(vtkImageData* vtkImage, QAtomicInteger<int>* cancel = nullptr);

// QObject-based worker wrapper so the computation can be moved to a QThread and wired via signals/slots.
// - Emits signals on completion / error. Accepts an optional external QAtomicInteger<int>* cancellation flag.
// - The free function above is preserved for compatibility with existing synchronous callers.
class OtsuThresholdWorker : public QObject
{
	Q_OBJECT
public:
	explicit OtsuThresholdWorker(QObject* parent = nullptr);
	~OtsuThresholdWorker() override;

	// Convenience setter for an externally-managed cancel flag (not required).
	void setCancelFlag(QAtomicInteger<int>* cancel);

signals:
	// Emitted when a compute request starts (useful to show UI busy state).
	void computeStarted();

	// Emitted to report progress (0..100)
	void computeProgress(int percent);

	// Emitted when computation completes (ok == true if threshold valid).
	void computeFinished(bool ok, double threshold);

	// Emitted when computation cannot complete due to an error.
	void computeError(const QString& reason);

	// Emitted if computation was cancelled cooperatively (cancel flag observed).
	void computeCanceled();

public slots:
	// Starts computation on the provided vtk image. This is a slot so it can be invoked via queued connection.
	// If `cancel` is provided it will be used; otherwise the worker's internal cancel flag is used.
	void compute(vtkImageData* vtkImage, QAtomicInteger<int>* cancel = nullptr);

	// Request cooperative cancellation. This sets the worker's internal cancel flag.
	void requestCancel();

private:
	QAtomicInteger<int> m_internalCancel;
	QAtomicInteger<int>* m_externalCancel = nullptr;
};

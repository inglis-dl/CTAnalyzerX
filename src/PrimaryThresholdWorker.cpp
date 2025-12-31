#include "PrimaryThresholdWorker.h"

#include <vtkImageData.h>
#include <vtkType.h>

#include <QAtomicInteger>
#include <QThread>

#include "itkVTKImageToImageFilter.h"
#include <itkImage.h>
#include <itkImageToHistogramFilter.h>
#include <itkOtsuThresholdCalculator.h>

#include <stdexcept>
#include <functional> // for progress callback

namespace {

	// Templated processing function as requested.
	// Returns true + threshold in outThreshold on success, false otherwise.
	// Checks cancel flag before expensive Update() calls where practical.
	template <typename TPixel>
	bool ProcessImageTyped(vtkImageData* vtkImg, QAtomicInteger<int>* cancel, double& outThreshold,
						   const std::function<void(int)>& progress)
	{
		using ImageType = itk::Image<TPixel, 3>;
		using VTKToITKFilter = itk::VTKImageToImageFilter<ImageType>;

		if (cancel && cancel->loadAcquire()) {
			if (progress) progress(0);
			return false;
		}

		if (progress) progress(5);

		auto converter = VTKToITKFilter::New();
		converter->SetInput(vtkImg);

		// Check cancellation before heavy conversion
		if (cancel && cancel->loadAcquire()) {
			if (progress) progress(0);
			return false;
		}

		try {
			converter->Update();
		}
		catch (...) {
			if (progress) progress(0);
			return false;
		}

		if (progress) progress(30);

		if (cancel && cancel->loadAcquire()) {
			if (progress) progress(0);
			return false;
		}

		typename ImageType::Pointer itkImg = converter->GetOutput();
		if (!itkImg) {
			if (progress) progress(0);
			return false;
		}

		// Build histogram and compute Otsu threshold (mirrors repository test)
		using HistogramGeneratorType = itk::Statistics::ImageToHistogramFilter<ImageType>;
		using HistogramType = HistogramGeneratorType::HistogramType;
		using CalculatorType = itk::OtsuThresholdCalculator<HistogramType>;

		auto histGenerator = HistogramGeneratorType::New();
		histGenerator->SetInput(itkImg);
		HistogramGeneratorType::HistogramSizeType hsize(1);
		hsize[0] = 256;
		histGenerator->SetHistogramSize(hsize);
		histGenerator->SetAutoMinimumMaximum(true);

		if (cancel && cancel->loadAcquire()) {
			if (progress) progress(0);
			return false;
		}

		try {
			histGenerator->Update();
		}
		catch (...) {
			if (progress) progress(0);
			return false;
		}

		if (progress) progress(70);

		if (cancel && cancel->loadAcquire()) {
			if (progress) progress(0);
			return false;
		}

		auto calculator = CalculatorType::New();
		calculator->SetInput(histGenerator->GetOutput());

		try {
			calculator->Update();
		}
		catch (...) {
			if (progress) progress(0);
			return false;
		}

		if (progress) progress(95);

		outThreshold = calculator->GetThreshold();

		if (progress) progress(100);
		return true;
	}

	// Dispatcher that calls the right template based on the VTK scalar type.
	bool ProcessImageDispatcher(vtkImageData* vtkImg, QAtomicInteger<int>* cancel, double& outThreshold,
								const std::function<void(int)>& progress)
	{
		if (!vtkImg) {
			if (progress) progress(0);
			return false;
		}

		const int scalarType = vtkImg->GetScalarType();

		switch (scalarType)
		{
			case VTK_UNSIGNED_CHAR:
			return ProcessImageTyped<unsigned char>(vtkImg, cancel, outThreshold, progress);
			case VTK_CHAR:
			return ProcessImageTyped<char>(vtkImg, cancel, outThreshold, progress);
			case VTK_SHORT:
			return ProcessImageTyped<short>(vtkImg, cancel, outThreshold, progress);
			case VTK_UNSIGNED_SHORT:
			return ProcessImageTyped<unsigned short>(vtkImg, cancel, outThreshold, progress);
			case VTK_INT:
			return ProcessImageTyped<int>(vtkImg, cancel, outThreshold, progress);
			case VTK_UNSIGNED_INT:
			return ProcessImageTyped<unsigned int>(vtkImg, cancel, outThreshold, progress);
			case VTK_LONG:
			return ProcessImageTyped<long>(vtkImg, cancel, outThreshold, progress);
			case VTK_UNSIGNED_LONG:
			return ProcessImageTyped<unsigned long>(vtkImg, cancel, outThreshold, progress);
			case VTK_FLOAT:
			return ProcessImageTyped<float>(vtkImg, cancel, outThreshold, progress);
			case VTK_DOUBLE:
			return ProcessImageTyped<double>(vtkImg, cancel, outThreshold, progress);
			default:
			if (progress) progress(0);
			return false;
		}
	}

} // anonymous namespace

// Backwards-compatible synchronous helper retained.
std::pair<bool, double> computePrimaryOtsuThresholdFromVtk(vtkImageData* vtkImage, QAtomicInteger<int>* cancel)
{
	double threshold = 0.0;
	// No progress callback for synchronous API
	bool ok = ProcessImageDispatcher(vtkImage, cancel, threshold, std::function<void(int)>{});
	return { ok, threshold };
}

// -----------------------------
// PrimaryThresholdWorker implementation
// -----------------------------

PrimaryThresholdWorker::PrimaryThresholdWorker(QObject* parent)
	: QObject(parent),
	m_internalCancel(0),
	m_externalCancel(nullptr)
{
}

PrimaryThresholdWorker::~PrimaryThresholdWorker() = default;

void PrimaryThresholdWorker::setCancelFlag(QAtomicInteger<int>* cancel)
{
	m_externalCancel = cancel;
}

void PrimaryThresholdWorker::requestCancel()
{
	m_internalCancel.storeRelease(1);
}

void PrimaryThresholdWorker::compute(vtkImageData* vtkImage, QAtomicInteger<int>* cancel)
{
	// Use external cancel param if provided, else the worker's external flag if set,
	// otherwise fall back to internal cancel flag.
	QAtomicInteger<int>* cancelPtr = cancel ? cancel : (m_externalCancel ? m_externalCancel : &m_internalCancel);

	emit computeStarted();

	// progress callback emits computeProgress from the worker thread
	auto progressCb = [this](int pct) {
		// keep value in 0..100
		if (pct < 0) pct = 0;
		if (pct > 100) pct = 100;
		emit computeProgress(pct);
		};

	// Perform computation synchronously on the thread where this slot runs.
	// Caller is expected to invoke via queued connection when running in a dedicated worker thread.
	bool ok = false;
	double threshold = 0.0;

	try {
		ok = ProcessImageDispatcher(vtkImage, cancelPtr, threshold, progressCb);
	}
	catch (const std::exception& ex) {
		emit computeError(QString::fromUtf8(ex.what()));
		return;
	}
	catch (...) {
		emit computeError(QStringLiteral("Unknown error during threshold computation"));
		return;
	}

	// If cancel flag observed, signal canceled. Note: ProcessImageDispatcher returns false
	// if cancellation was observed; we attempt to differentiate cancellation vs other failures
	// by checking the cancel flag state here.
	if (cancelPtr && cancelPtr->loadAcquire()) {
		emit computeCanceled();
		return;
	}

	emit computeFinished(ok, threshold);
}
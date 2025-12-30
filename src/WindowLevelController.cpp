#include "WindowLevelController.h"

#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QSignalBlocker>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QPen>
#include <QMenu>
#include <QActionGroup>
#include <QAction>
#include <QSettings>

#include "JsonSettings.h"

#include <vtkImageData.h>
#include <vtkImageHistogram.h>
#include <vtkIdTypeArray.h>
#include <vtkSmartPointer.h>

WindowLevelController::WindowLevelController(QWidget* parent)
	: QWidget(parent)
{
	ui.setupUi(this);

	// Debounced interactive emission to reduce render flood
	m_debounce = new QTimer(this);
	m_debounce->setSingleShot(true);
	m_debounce->setInterval(60);

	auto maybeEmitInteractive = [this]() {
		m_debounce->start();
		};

	connect(m_debounce, &QTimer::timeout, this, [this]() {
		emit windowLevelChanged(ui.m_spinWindow->value(), ui.m_spinLevel->value());
	});

	connect(ui.m_spinWindow, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [maybeEmitInteractive](double) { maybeEmitInteractive(); });
	connect(ui.m_spinLevel, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [maybeEmitInteractive](double) { maybeEmitInteractive(); });

	connect(ui.m_spinWindow, &QDoubleSpinBox::editingFinished, this, [this]() {
		emit windowLevelCommitted(ui.m_spinWindow->value(), ui.m_spinLevel->value());
	});
	connect(ui.m_spinLevel, &QDoubleSpinBox::editingFinished, this, [this]() {
		emit windowLevelCommitted(ui.m_spinWindow->value(), ui.m_spinLevel->value());
	});

	// Reset button: notify listeners to reset window/level to baseline
	connect(ui.m_btnReset, &QPushButton::clicked, this, [this]() {
		emit requestResetWindowLevel();
	});

	m_histo = vtkSmartPointer<vtkImageHistogram>::New();

	// Prepare cached scene + painter path so redraws only update lines
	// Prepare context menu for the histogram view (ui.m_view)
	if (ui.m_view) {
		ui.m_view->setContextMenuPolicy(Qt::CustomContextMenu);
		m_viewMenu = new QMenu(this);
		m_viewMenuGroup = new QActionGroup(this);
		m_viewMenuGroup->setExclusive(true);

		QAction* aLinear = new QAction(tr("Linear"), m_viewMenu);
		aLinear->setCheckable(true);
		aLinear->setData(vtkImageHistogram::Linear);
		m_viewMenuGroup->addAction(aLinear);
		m_viewMenu->addAction(aLinear);

		QAction* aLog = new QAction(tr("Log"), m_viewMenu);
		aLog->setCheckable(true);
		aLog->setData(vtkImageHistogram::Log);
		m_viewMenuGroup->addAction(aLog);
		m_viewMenu->addAction(aLog);

		QAction* aSqrt = new QAction(tr("Sqrt"), m_viewMenu);
		aSqrt->setCheckable(true);
		aSqrt->setData(vtkImageHistogram::Sqrt);
		m_viewMenuGroup->addAction(aSqrt);
		m_viewMenu->addAction(aSqrt);

		// default selection
		aLinear->setChecked(true);
		m_histo->SetHistogramImageScale(vtkImageHistogram::Linear);

		connect(ui.m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& pt) {
			if (!m_viewMenu || !ui.m_view) return;
			// ensure QAction checked state matches current scale
			const int cur = m_histo ? m_histo->GetHistogramImageScale() : vtkImageHistogram::Linear;
			for (QAction* act : m_viewMenu->actions()) {
				if (act->data().isValid() && act->data().toInt() == cur) {
					act->setChecked(true);
					break;
				}
			}
			m_viewMenu->exec(ui.m_view->mapToGlobal(pt));
		});

		connect(m_viewMenuGroup, &QActionGroup::triggered, this, [this](QAction* act) {
			if (!act) return;
			const int val = act->data().toInt();
			setHistogramScale(val);
		});

		m_scene = new QGraphicsScene(this);
		ui.m_view->setScene(m_scene);

		// create an empty path item and keep pointer for fast updates (open polyline)
		m_pathItem = m_scene->addPath(m_path, QPen(Qt::black));
		m_pathItem->setBrush(Qt::NoBrush);
	}
}

void WindowLevelController::setWindow(double w)
{
	// Prevent emitting valueChanged while we programmatically set the spinbox
	if (!ui.m_spinWindow) return;
	QSignalBlocker b(ui.m_spinWindow);
	ui.m_spinWindow->setValue(w);
}

void WindowLevelController::setLevel(double l)
{
	if (!ui.m_spinLevel) return;
	QSignalBlocker b(ui.m_spinLevel);
	ui.m_spinLevel->setValue(l);
}

void WindowLevelController::setDebounceInterval(int ms)
{
	if (!m_debounce) return;
	m_debounce->setInterval(ms);
}

int WindowLevelController::histogramScale() const
{
	if (!m_histo) return vtkImageHistogram::Linear;
	return m_histo->GetHistogramImageScale();
}

void WindowLevelController::setHistogramScale(int s)
{
	if (!m_histo) return;
	if (s < vtkImageHistogram::Linear || s > vtkImageHistogram::Sqrt) s = vtkImageHistogram::Linear;
	if (m_histo->GetHistogramImageScale() == s) return;

	m_histo->SetHistogramImageScale(s);

	// Redraw histogram from whatever input is currently attached to m_histo.
	redrawHistogram();

	emit histogramScaleChanged(s);
}

void WindowLevelController::setImageData(vtkImageData* image)
{
	if (!image || !ui.m_view)  // adjust member name to your .ui
		return;

	// Do NOT rely on m_lastImage; set the input on m_histo and let redrawHistogram
	// obtain the input from the m_histo instance.
	m_histo->SetInputData(image);
	m_histo->AutomaticBinningOn();

	double* range = image->GetScalarRange();

	// UI-oriented cap; adjust as needed
	m_histo->SetMaximumNumberOfBins(static_cast<int>(range[1] - range[0]));
	m_histo->SetBinOrigin(range[0]);
	m_histo->GenerateHistogramImageOff();

	// Now compute and draw using the histogram object's input
	redrawHistogram();
}

void WindowLevelController::redrawHistogram()
{
	// Ensure we have a histogram filter with an attached concrete input vtkImageData
	if (!m_histo || !ui.m_view)
		return;

	// Try to obtain the input data object from the histogram algorithm.
	vtkDataObject* inObj = m_histo->GetInputDataObject(0, 0);
	vtkImageData* image = vtkImageData::SafeDownCast(inObj);
	if (!image) {
		// No concrete image attached to histogram - nothing to draw.
		return;
	}

	// Ensure histogram is up-to-date for the current input and histogram parameters.
	m_histo->Update();

	vtkIdTypeArray* hArr = m_histo->GetHistogram();
	if (!hArr || hArr->GetNumberOfTuples() <= 0)
		return;

	const int nBins = hArr->GetNumberOfTuples();

	// Generate scaled counts array according to histogram scale (linear/log/sqrt).
	// We compute scaledCounts separately and use it to find the peak for normalization.
	std::vector<double> scaledCounts;
	scaledCounts.resize(nBins);

	// Determine scale mode from vtkImageHistogram enum (client-side)
	int scaleMode = m_histo->GetHistogramImageScale(); // 0=Linear,1=Log,2=Sqrt

	for (int i = 0; i < nBins; ++i) {
		const vtkIdType rawCount = hArr->GetValue(i);
		double s = 0.0;
		switch (scaleMode) {
			case vtkImageHistogram::Log:
			// log(0) is undefined; map 0 -> 0, otherwise natural log
			s = (rawCount > 0) ? std::log(static_cast<double>(rawCount)) : 0.0;
			break;
			case vtkImageHistogram::Sqrt:
			s = std::sqrt(static_cast<double>(rawCount));
			break;
			case vtkImageHistogram::Linear:
			default:
			s = static_cast<double>(rawCount);
			break;
		}
		scaledCounts[i] = s;
	}

	// Find peak from scaled counts for normalization
	double maxScaled = 0.0;
	for (int i = 0; i < nBins; ++i) {
		if (scaledCounts[i] > maxScaled) maxScaled = scaledCounts[i];
	}
	if (maxScaled <= 0.0)
		return;

	// Use cached scene and polygon item if available; create lazily if missing.
	if (!m_scene) {
		m_scene = ui.m_view->scene();
		if (!m_scene) {
			m_scene = new QGraphicsScene(this);
			ui.m_view->setScene(m_scene);
		}
	}

	const QSizeF vpSize = ui.m_view->viewport()->size();
	const double w = vpSize.width() > 0 ? vpSize.width() : 256.0;
	const double h = vpSize.height() > 0 ? vpSize.height() : 128.0;

	// Build an open QPainterPath: moveTo(first) then lineTo(...) for each subsequent point.
	m_path.clear();
	if (nBins > 0) {
		const double x0 = (nBins > 1) ? (w * 0 / double(nBins - 1)) : 0.0;
		const double y0 = h * (1.0 - (scaledCounts[0] / maxScaled));
		m_path.moveTo(x0, y0);
		for (int i = 1; i < nBins; ++i) {
			const double x = (nBins > 1) ? (w * i / double(nBins - 1)) : 0.0;
			const double y = h * (1.0 - (scaledCounts[i] / maxScaled));
			m_path.lineTo(x, y);
		}
	}

	// Update path item (open polyline — QPainterPath is not closed unless closeSubpath() is called)
	m_pathItem->setPath(m_path);

	// Update the scene rect so the view scales correctly
	m_scene->setSceneRect(0, 0, w, h);
	// Optional: ensure view repaints
	ui.m_view->viewport()->update();
}

void WindowLevelController::writeSettings()
{
	QSettings settings(JsonSettings::defaultSettingsPath(), JsonSettings::JsonFormat);
	if (settings.status() != QSettings::NoError) return;

	settings.beginGroup("WindowLevelController");
	settings.setValue("histogramScale", histogramScale());
	settings.endGroup();
	settings.sync();
}

void WindowLevelController::readSettings()
{
	QSettings settings(JsonSettings::defaultSettingsPath(), JsonSettings::JsonFormat);
	if (settings.status() != QSettings::NoError) return;

	settings.beginGroup("WindowLevelController");
	const int s = settings.value("histogramScale", vtkImageHistogram::Linear).toInt();
	settings.endGroup();

	setHistogramScale(s);
}
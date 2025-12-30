#include "WindowLevelController.h"

#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QSignalBlocker>
#include <QMenu>
#include <QActionGroup>
#include <QAction>
#include <QSettings>
#include <QBrush>
#include <QColor>

#include "JsonSettings.h"

#include <vtkImageData.h>
#include <vtkImageHistogram.h>
#include <vtkIdTypeArray.h>
#include <vtkSmartPointer.h>
#include <vtkDataObject.h>

#include <QtCharts/QChartView>
#include <QtCharts/QChart>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QValueAxis>
#include <QSizePolicy>
#include <QEvent>
#include <QVBoxLayout>
#include "RangeSlider.h"

using namespace QtCharts;

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

	// Setup Qt Charts in the placeholder `ui.m_view` (works when `m_view` is a QWidget placeholder)
	if (ui.m_view) {

		// Series and axes
		m_barSet = new QBarSet(QString());
		m_barSet->setColor(QColor("#404040"));        // dark gray
		m_barSet->setBorderColor(Qt::transparent);    // no outline

		m_barSeries = new QBarSeries();
		m_barSeries->append(m_barSet);
		m_barSeries->setBarWidth(1.0); // full width bars, no gaps
		m_barSeries->setLabelsVisible(false);

		m_chart = new QChart();
		m_chart->legend()->hide();
		m_chart->addSeries(m_barSeries);
		m_chart->setBackgroundRoundness(0);
		m_chart->setBackgroundVisible(true);
		m_chart->setBackgroundBrush(QBrush(QColor("#e0e0e0")));   // light gray
		m_chart->setPlotAreaBackgroundVisible(true);
		m_chart->setPlotAreaBackgroundBrush(QBrush(QColor("#e0e0e0")));
		m_chart->setMargins(QMargins(0, 0, 0, 0));


		m_axisX = new QValueAxis();
		m_axisY = new QValueAxis();

		m_axisX->setLabelsVisible(false); // hide many labels if many bins
		m_axisX->setGridLineVisible(false);
		m_axisX->setLineVisible(false);
		m_axisX->setTickCount(0);      // hide ticks
		m_axisX->setMinorTickCount(0); // hide minor ticks

		m_axisY->setLabelsVisible(false); // hide many labels if many bins
		m_axisY->setGridLineVisible(false);
		m_axisY->setLineVisible(false);
		m_axisY->setTickCount(0);      // hide ticks
		m_axisY->setMinorTickCount(0); // hide minor ticks

		m_chart->addAxis(m_axisX, Qt::AlignBottom);
		m_chart->addAxis(m_axisY, Qt::AlignLeft);
		m_barSeries->attachAxis(m_axisX);
		m_barSeries->attachAxis(m_axisY);

		// Create view and place it into placeholder
		// Use a layout so the chart fills the placeholder and resizes correctly.
		m_chartView = new QChartView(m_chart);
		m_chartView->setRenderHint(QPainter::Antialiasing);
		m_chartView->setContentsMargins(0, 0, 0, 0);
		m_chartView->setStyleSheet("background: transparent; border: none; padding: 0px; margin: 0px;");


		m_chartView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		// Ensure the placeholder has a zero-margin layout and add the view.
		if (!ui.m_view->layout()) {
			// stack chart above slider
			auto layout = new QVBoxLayout(ui.m_view);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(0);
			layout->setSizeConstraint(QLayout::SetNoConstraint);
			// create slider below the chart
			m_slider = new RangeSlider(Qt::Horizontal, ui.m_view);
			m_slider->setRange(0, 0);
			m_slider->setValues(0, 0);
			m_slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			m_slider->setFixedHeight(18);

			layout->addWidget(m_chartView, 1);
			layout->addWidget(m_slider, 0);
		}
		else {
			// ensure zero margins / no spacing on the existing layout
			QLayout* lay = ui.m_view->layout();
			lay->setContentsMargins(0, 0, 0, 0);
			lay->setSpacing(0);
			lay->setSizeConstraint(QLayout::SetNoConstraint);

			// If the existing layout is a QBoxLayout (QHBoxLayout/QVBoxLayout),
			// we can specify a stretch factor to force the chart to fill.
			if (auto box = qobject_cast<QBoxLayout*>(lay)) {
				box->addWidget(m_chartView, /*stretch*/ 1);
			}
			else {
				lay->addWidget(m_chartView);
			}

			// add slider to existing layout as the next widget (attempt to place below if layout is vertical)
			m_slider = new RangeSlider(Qt::Horizontal, ui.m_view);
			m_slider->setRange(0, 0);
			m_slider->setValues(0, 0);
			m_slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			m_slider->setFixedHeight(18);
			if (auto box = qobject_cast<QBoxLayout*>(lay)) {
				box->addWidget(m_slider, /*stretch*/ 0);
			}
			else {
				lay->addWidget(m_slider);
			}
		}
		m_chartView->show();

		// wire slider -> x axis zooming
		if (m_slider) {
			connect(m_slider, &RangeSlider::valuesChanged, this, [this](int minPos, int maxPos) {
				if (m_axisX && m_chart) {
					m_axisX->setRange(double(minPos), double(maxPos));
					m_chart->update();
				}
			});
		}

		// arrange for the plot area to be resized to the entire view:
		// - install an event filter so we can react to view/placeholder resizes
		m_chartView->installEventFilter(this);
		ui.m_view->installEventFilter(this);
		// - and run once after construction to set initial plot area
		QTimer::singleShot(0, this, [this]() { adjustChartPlotArea(); });

		// Bar styling: dark gray
		QBrush barBrush(QColor(0x44, 0x44, 0x44));
		m_barSet->setBrush(barBrush);
	}

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

		// --- add separator + "Filter peak" toggle action ---
		m_viewMenu->addSeparator();
		m_actFilterPeak = new QAction(tr("Filter peak"), m_viewMenu);
		m_actFilterPeak->setCheckable(true);
		m_actFilterPeak->setChecked(m_filterPeak);
		m_viewMenu->addAction(m_actFilterPeak);

		// connect toggling the action to the property
		connect(m_actFilterPeak, &QAction::toggled, this, [this](bool checked) {
			setFilterPeak(checked);
		});

		connect(ui.m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint& pt) {
			if (!m_viewMenu || !ui.m_view) return;
			// ensure QAction checked state matches current scale
			const int cur = m_histo ? m_histo->GetHistogramImageScale() : vtkImageHistogram::Linear;
			for (QAction* act : m_viewMenu->actions()) {
				// actions used for histogram scale have valid data() integers
				if (act->data().isValid() && act->data().toInt() == cur) {
					act->setChecked(true);
					break;
				}
			}
			// ensure filter state is reflected
			if (m_actFilterPeak)
				m_actFilterPeak->setChecked(m_filterPeak);

			m_viewMenu->exec(ui.m_view->mapToGlobal(pt));
		});

		connect(m_viewMenuGroup, &QActionGroup::triggered, this, [this](QAction* act) {
			if (!act) return;
			const int val = act->data().toInt();
			setHistogramScale(val);
		});
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

// Filter peak property accessors
bool WindowLevelController::filterPeak() const
{
	return m_filterPeak;
}

void WindowLevelController::setFilterPeak(bool v)
{
	if (m_filterPeak == v) return;
	m_filterPeak = v;
	// update action checked state if present (avoid loops)
	if (m_actFilterPeak && m_actFilterPeak->isChecked() != v)
		m_actFilterPeak->setChecked(v);
	// re-render histogram with filter applied/removed
	redrawHistogram();
}

void WindowLevelController::setImageData(vtkImageData* image)
{
	if (!image || !ui.m_view)  // adjust member name to your .ui
		return;

	// Set the input on the histogram filter. redrawHistogram will query it.
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
	if (!m_histo || !ui.m_view || !m_chartView)
		return;

	vtkImageData* image =
		vtkImageData::SafeDownCast(m_histo->GetInputDataObject(0, 0));
	if (!image)
		return;

	m_histo->Update();

	vtkIdTypeArray* hArr = m_histo->GetHistogram();
	if (!hArr || hArr->GetNumberOfTuples() <= 0)
		return;

	const int nBins = hArr->GetNumberOfTuples();

	std::vector<double> scaledCounts(nBins);
	int scaleMode = m_histo->GetHistogramImageScale();

	for (int i = 0; i < nBins; ++i)
	{
		vtkIdType raw = hArr->GetValue(i);
		double s = 0.0;

		switch (scaleMode)
		{
			case vtkImageHistogram::Log:
			s = (raw > 0) ? std::log(double(raw)) : 0.0;
			break;
			case vtkImageHistogram::Sqrt:
			s = std::sqrt(double(raw));
			break;
			default:
			s = double(raw);
			break;
		}
		scaledCounts[i] = s;
	}

	double maxScaled = 0.0;
	for (double v : scaledCounts)
		if (v > maxScaled) maxScaled = v;

	if (maxScaled <= 0.0)
		return;

	const int maxDisplayBins = 2048;
	int displayBins = std::min(nBins, maxDisplayBins);

	std::vector<double> displayValues(displayBins, 0.0);

	if (displayBins == nBins)
	{
		displayValues = scaledCounts;
	}
	else
	{
		const double step = double(nBins) / double(displayBins);
		for (int b = 0; b < displayBins; ++b)
		{
			int start = int(std::floor(b * step));
			int end = int(std::floor((b + 1) * step));
			if (end <= start) end = start + 1;

			double peak = 0.0;
			for (int j = start; j < end && j < nBins; ++j)
				peak = std::max(peak, scaledCounts[j]);

			displayValues[b] = peak;
		}
	}

	// update slider range / selection to match current displayBins
	if (m_slider) {
		const int maxPos = (displayBins > 0) ? (displayBins - 1) : 0;
		// set slider overall range (inherited from QSlider) and RangeSlider values
		m_slider->setMinimum(0);
		m_slider->setMaximum(maxPos);
		// set full-range selection by default (user can then drag handles)
		m_slider->setValues(0, maxPos);
	}

	// If filterPeak is enabled, mask out the largest peak so it doesn't dominate the display
	if (m_filterPeak && !displayValues.empty()) {
		// find index of maximum
		std::size_t maxIdx = 0;
		for (std::size_t i = 1; i < displayValues.size(); ++i) {
			if (displayValues[i] > displayValues[maxIdx]) maxIdx = i;
		}
		// zero it out (remove its influence). This choice is simple and effective.
		displayValues[maxIdx] = 0.0;
	}

	double dispMax = 0.0;
	for (double v : displayValues)
		if (v > dispMax) dispMax = v;
	if (dispMax <= 0.0) dispMax = 1.0;

	if (!m_barSeries)
		return;

	if (m_barSeries->count() > 0)
		m_barSet->remove(0, m_barSet->count());

	QList<qreal> qvals;
	qvals.reserve(displayBins);
	for (double v : displayValues)
		qvals.append(qreal(v));

	m_barSet->append(qvals);

	// --- QValueAxis update ---
	if (m_axisX)
		m_axisX->setRange(0.0, double(displayBins));

	if (m_axisY)
		m_axisY->setRange(0.0, dispMax);

	// Bars fill each numeric bin
	m_barSeries->setBarWidth(1.0);

	m_chart->update();
	m_chartView->repaint();
}

void WindowLevelController::writeSettings()
{
	QSettings settings(JsonSettings::defaultSettingsPath(), JsonSettings::JsonFormat);
	if (settings.status() != QSettings::NoError) return;

	settings.beginGroup("WindowLevelController");
	settings.setValue("histogramScale", histogramScale());
	// persist filter preference optionally:
	settings.setValue("filterPeak", filterPeak());
	settings.endGroup();
	settings.sync();
}

void WindowLevelController::readSettings()
{
	QSettings settings(JsonSettings::defaultSettingsPath(), JsonSettings::JsonFormat);
	if (settings.status() != QSettings::NoError) return;

	settings.beginGroup("WindowLevelController");
	const int s = settings.value("histogramScale", vtkImageHistogram::Linear).toInt();
	const bool f = settings.value("filterPeak", false).toBool();
	settings.endGroup();

	setHistogramScale(s);
	setFilterPeak(f);
}

void WindowLevelController::adjustChartPlotArea()
{
	if (!m_chart || !m_chartView) return;
	// use the chart view's viewport size (pixels) and set plot area to fill it
	const QSize vsz = m_chartView->viewport() ? m_chartView->viewport()->size() : m_chartView->size();
	if (vsz.isEmpty()) return;
	m_chart->setPlotArea(QRectF(0.0, 0.0, qreal(vsz.width()), qreal(vsz.height())));
}

bool WindowLevelController::eventFilter(QObject* watched, QEvent* event)
{
	if (event->type() == QEvent::Resize) {
		if (watched == m_chartView || watched == ui.m_view) {
			// schedule update to allow layouts to settle
			QTimer::singleShot(0, this, [this]() { adjustChartPlotArea(); });
		}
	}
	// let base class handle other processing
	return QObject::eventFilter(watched, event);
}

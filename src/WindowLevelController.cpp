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
#include <QPolygonF>
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

	// Recompute/redraw histogram with same last image if available
	if (m_lastImage) {
		setImageData(m_lastImage);
	}

	emit histogramScaleChanged(s);
}

void WindowLevelController::setImageData(vtkImageData* image)
{
	if (!image || !ui.m_view)  // adjust member name to your .ui
		return;

	// keep a reference for future re-render when scale changes
	m_lastImage = image;

	// Compute histogram via vtkImageHistogram (non-interactive, once per image)
	m_histo->SetInputData(image);
	m_histo->AutomaticBinningOn();

	double* range = image->GetScalarRange();

	// UI-oriented cap; adjust as needed
	m_histo->SetMaximumNumberOfBins(static_cast<int>(range[1] - range[0]));
	m_histo->SetBinOrigin(range[0]);
	m_histo->GenerateHistogramImageOff();
	m_histo->Update();

	vtkIdTypeArray* hArr = m_histo->GetHistogram();
	if (!hArr || hArr->GetNumberOfTuples() <= 0)
		return;

	const int nBins = hArr->GetNumberOfTuples();

	// Find peak for normalization
	vtkIdType maxCount = 0;
	for (int i = 0; i < nBins; ++i) {
		vtkIdType c = hArr->GetValue(i);
		if (c > maxCount)
			maxCount = c;
	}
	if (maxCount <= 0)
		return;

	// Prepare scene
	QGraphicsScene* scene = ui.m_view->scene();
	if (!scene) {
		scene = new QGraphicsScene(ui.m_view);
		ui.m_view->setScene(scene);
	}
	scene->clear();

	const QSizeF vpSize = ui.m_view->viewport()->size();
	const double w = vpSize.width() > 0 ? vpSize.width() : 256.0;
	const double h = vpSize.height() > 0 ? vpSize.height() : 128.0;

	// Draw as simple polyline
	QPolygonF poly;
	poly.reserve(nBins);
	for (int i = 0; i < nBins; ++i) {
		const vtkIdType c = hArr->GetValue(i);
		const double x = (nBins > 1) ? (w * i / double(nBins - 1)) : 0.0;
		const double y = h * (1.0 - double(c) / double(maxCount)); // invert Y
		poly.append(QPointF(x, y));
	}

	QPen pen(Qt::black);
	pen.setWidthF(1.0);
	scene->addPolygon(poly, pen);
	scene->setSceneRect(0, 0, w, h);
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
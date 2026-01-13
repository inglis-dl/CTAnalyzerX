#include "WindowLevelController.h"
#include "JsonSettings.h"

#include "ScalarOpacityFunctionWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QPushButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <vtkImageData.h>
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

	// wire spinboxes to debounce
	connect(ui.m_spinWindow, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [maybeEmitInteractive](double) { maybeEmitInteractive(); });
	connect(ui.m_spinLevel, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [maybeEmitInteractive](double) { maybeEmitInteractive(); });

	// commit on editing finished
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

	// Integrate the ScalarOpacityFunctionWidget placed in the .ui (m_so_function)
	// The widget is responsible for displaying and editing a piecewise scalar-opacity function.
	// Connect its functionChanged to a hook so the controller can update window/level if desired.
	if (ui.m_so_function) {
		connect(ui.m_so_function, &ScalarOpacityFunctionWidget::functionChanged, this, [this]() {
			// Hook: derive window/level from the function nodes if desired in future.
			// For now, do nothing. Consumers can connect to functionChanged directly.
		});
	}

	// Defer any further initialization to let UI settle
	QTimer::singleShot(0, this, [this]() {
		// nothing for now
	});
}

void WindowLevelController::setWindow(double w)
{
	if (!ui.m_spinWindow) return;
	QSignalBlocker b(ui.m_spinWindow);
	ui.m_spinWindow->setValue(w);
	emit windowLevelChanged(ui.m_spinWindow->value(), ui.m_spinLevel->value());
}

void WindowLevelController::setLevel(double l)
{
	if (!ui.m_spinLevel) return;
	QSignalBlocker b(ui.m_spinLevel);
	ui.m_spinLevel->setValue(l);
	emit windowLevelChanged(ui.m_spinWindow->value(), ui.m_spinLevel->value());
}

ScalarOpacityFunctionWidget* WindowLevelController::scalarOpacityFunctionWidget() const
{
	if (ui.m_so_function) {
		return ui.m_so_function;
	}
	else {
		return nullptr;
	}
}

void WindowLevelController::setDebounceInterval(int ms)
{
	if (!m_debounce) return;
	m_debounce->setInterval(ms);
}

void WindowLevelController::setImageData(vtkImageData* image)
{
	// Store image and forward to scalar-opacity widget for histogram/chart rendering.
	if (!image) {
		m_image = nullptr;
		if (ui.m_so_function) ui.m_so_function->setImageData(nullptr);
		return;
	}
	m_image = vtkSmartPointer<vtkImageData>::New();
	m_image->ShallowCopy(image);

	// Push scalar range and image to the opacity widget so it can set scene domain and compute histogram.
	if (ui.m_so_function) {
		ui.m_so_function->setImageData(image);
	}
}

void WindowLevelController::writeSettings()
{
	QSettings settings(JsonSettings::defaultSettingsPath(), JsonSettings::JsonFormat);
	if (settings.status() != QSettings::NoError) return;

	settings.beginGroup("WindowLevelController");
	// no histogram/chart settings persisted at this time
	settings.endGroup();
	settings.sync();
}

void WindowLevelController::readSettings()
{
	QSettings settings(JsonSettings::defaultSettingsPath(), JsonSettings::JsonFormat);
	if (settings.status() != QSettings::NoError) return;

	settings.beginGroup("WindowLevelController");
	// no histogram/chart settings to restore
	settings.endGroup();
}

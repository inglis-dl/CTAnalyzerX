#include "WindowLevelController.h"

#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QSignalBlocker>

WindowLevelController::WindowLevelController(QWidget* parent)
	: QWidget(parent)
{
	ui.setupUi(this);

	// Debounced interactive emission to reduce render flood
	m_debounce = new QTimer(this);
	m_debounce->setSingleShot(true);
	m_debounce->setInterval(60);

	auto maybeEmitInteractive = [this]() {
		if (ui.m_chkInteractive && ui.m_chkInteractive->isChecked()) {
			m_debounce->start();
		}
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

	// Forward checkbox toggles to interested parties
	if (ui.m_chkInteractive) {
		connect(ui.m_chkInteractive, &QCheckBox::toggled, this, [this](bool on) {
			// If interactive was turned off, cancel any pending emission.
			if (!on && m_debounce->isActive()) {
				m_debounce->stop();
			}
			emit interactiveToggled(on);
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

void WindowLevelController::setInteractive(bool interactive)
{
	if (!ui.m_chkInteractive) return;
	// update checkbox programmatically (this will emit interactiveToggled via connected signal)
	ui.m_chkInteractive->setChecked(interactive);
}

void WindowLevelController::setDebounceInterval(int ms)
{
	if (!m_debounce) return;
	m_debounce->setInterval(ms);
}

bool WindowLevelController::interactive() const
{
	return ui.m_chkInteractive ? ui.m_chkInteractive->isChecked() : false;
}
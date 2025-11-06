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
	auto* hl = new QHBoxLayout(this);
	hl->setContentsMargins(6, 6, 6, 6);
	hl->setSpacing(8);

	hl->addWidget(new QLabel(QStringLiteral("Window:"), this));
	m_spinWindow = new QDoubleSpinBox(this);
	m_spinWindow->setRange(1e-6, 1e12);
	m_spinWindow->setDecimals(2);
	m_spinWindow->setSingleStep(1.0);
	hl->addWidget(m_spinWindow);

	hl->addWidget(new QLabel(QStringLiteral("Level:"), this));
	m_spinLevel = new QDoubleSpinBox(this);
	m_spinLevel->setRange(-1e12, 1e12);
	m_spinLevel->setDecimals(2);
	m_spinLevel->setSingleStep(1.0);
	hl->addWidget(m_spinLevel);

	m_chkInteractive = new QCheckBox(QStringLiteral("Interactive"), this);
	m_chkInteractive->setChecked(true);
	hl->addWidget(m_chkInteractive);

	m_btnVolumeProps = new QPushButton(QStringLiteral("Volume Properties…"), this);
	hl->addWidget(m_btnVolumeProps);

	// Debounced interactive emission to reduce render flood
	QTimer* debounce = new QTimer(this);
	debounce->setSingleShot(true);
	debounce->setInterval(60);

	auto maybeEmitInteractive = [this, debounce]() {
		if (m_chkInteractive->isChecked()) {
			debounce->start();
		}
		};

	connect(debounce, &QTimer::timeout, this, [this]() {
		emit windowLevelChanged(m_spinWindow->value(), m_spinLevel->value());
	});

	connect(m_spinWindow, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [maybeEmitInteractive](double) { maybeEmitInteractive(); });
	connect(m_spinLevel, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [maybeEmitInteractive](double) { maybeEmitInteractive(); });

	connect(m_spinWindow, &QDoubleSpinBox::editingFinished, this, [this]() {
		emit windowLevelCommitted(m_spinWindow->value(), m_spinLevel->value());
	});
	connect(m_spinLevel, &QDoubleSpinBox::editingFinished, this, [this]() {
		emit windowLevelCommitted(m_spinWindow->value(), m_spinLevel->value());
	});

	connect(m_btnVolumeProps, &QPushButton::clicked, this, &WindowLevelController::requestVolumePropertyEditor);
}

void WindowLevelController::setWindow(double w)
{
	// Prevent emitting valueChanged while we programmatically set the spinbox
	if (!m_spinWindow) return;
	QSignalBlocker b(m_spinWindow);
	m_spinWindow->setValue(w);
}

void WindowLevelController::setLevel(double l)
{
	if (!m_spinLevel) return;
	QSignalBlocker b(m_spinLevel);
	m_spinLevel->setValue(l);
}
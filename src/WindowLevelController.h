#pragma once
#include <QWidget>

#include "ui_WindowLevelController.h"

class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QTimer;

class WindowLevelController : public QWidget
{
	Q_OBJECT
public:
	explicit WindowLevelController(QWidget* parent = nullptr);

public Q_SLOTS:
	// Set UI values (can be connected to view signals)
	void setWindow(double w);
	void setLevel(double l);

	// Programmatically toggle interactive mode (updates checkbox)
	void setInteractive(bool interactive);
	// Adjust debounce interval used for interactive emissions (ms)
	void setDebounceInterval(int ms);

	// Query current interactive state
	bool interactive() const;

Q_SIGNALS:
	// interactive (fires while user adjusts when InteractiveApply is enabled)
	void windowLevelChanged(double window, double level);
	// committed (user finished editing / pressed Enter / clicked apply)
	void windowLevelCommitted(double window, double level);
	// request to reset window/level to baseline across views
	void requestResetWindowLevel();

	// notify listeners when interactive toggles
	void interactiveToggled(bool interactive);

private:
	Ui::WindowLevelController ui;
	QTimer* m_debounce = nullptr;
};

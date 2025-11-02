#pragma once

#include <QPushButton>
#include <QStringList>

class QStyleOptionButton;
class QMouseEvent;
class QPaintEvent;
class QAction;
class QMenu;

class MenuButtonPrivate;

class MenuButton : public QPushButton
{
	Q_OBJECT
		// Designer/bindings-friendly way to provide items
		Q_PROPERTY(QStringList menuItems READ menuItems WRITE setMenuItems NOTIFY menuItemsChanged)

public:
	explicit MenuButton(QWidget* parent = nullptr);
	explicit MenuButton(const QString& title, QWidget* parent = nullptr);
	~MenuButton() override;

	// Populate the popup menu with items. Emits itemSelected when an action is chosen.
	void setMenuItems(const QStringList& items);
	QStringList menuItems() const; // READ accessor for Q_PROPERTY

	// Size hints (+13 px for the indicator area when split-enabled)
	QSize minimumSizeHint() const override;
	QSize sizeHint() const override;

	// Expose initStyleOption with the correct signature (QStyleOptionButton)
	void initStyleOption(QStyleOptionButton* option) const;

	// Configuration: enable/disable indicator-only mode
	// When true: only the right-side drop-down indicator is shown; any click opens the menu.
	void setIndicatorOnly(bool enabled);
	bool indicatorOnly() const;

signals:
	void itemSelected(const QString& item);
	void menuItemsChanged(); // NOTIFY for Q_PROPERTY

protected:
	void paintEvent(QPaintEvent* _event) override;
	bool hitButton(const QPoint& _pos) const override;
	void mousePressEvent(QMouseEvent* e) override;
	bool event(QEvent* ev) override;

private:
	MenuButtonPrivate* d_ptr;
};

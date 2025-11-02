#include "SelectionFrameWidget.h"
#include "MenuButton.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QWidget>
#include <QMenu>
#include <QAction>
#include <QToolButton>
#include <QEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QShortcut>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QFocusEvent>
#include <QApplication> // added

// Removed global static selection tracker

SelectionFrameWidget::SelectionFrameWidget(QWidget* parent)
	: QFrame(parent)
	, m_headerContainer(new QWidget(this))
	, m_headerLayout(new QHBoxLayout)
	, m_mainLayout(new QVBoxLayout)
	, m_selectionMenuButton(new MenuButton(this))
	, m_titleLabel(new QLabel(this))
	, m_centralWidget(nullptr)
	, m_headerActionsContainer(new QWidget(this))
	, m_headerActionsLayout(new QHBoxLayout)
{
	// Object names for stylesheet targeting
	this->setObjectName(QStringLiteral("SelectionFrameWidget"));
	m_headerContainer->setObjectName(QStringLiteral("SelectionFrameHeader"));
	m_titleLabel->setObjectName(QStringLiteral("SelectionFrameTitleLabel"));
	m_selectionMenuButton->setObjectName(QStringLiteral("SelectionFrameMenuButton"));
	m_headerActionsContainer->setObjectName(QStringLiteral("SelectionFrameHeaderActions"));

	// Focus to handle keyboard shortcuts (Enter/Space/F2/Ctrl+W)
	this->setFocusPolicy(Qt::StrongFocus);

	// Title bar palette: darker gray when unselected, dark blue when selected
	const QPalette appPal = QApplication::palette();
	m_titleFg = Qt::black;
	m_titleBg = appPal.window().color().darker(125);          // darker gray to indicate inactive/unselected
	m_selectedTitleFg = Qt::white;
	m_selectedTitleBg = Qt::darkBlue;
	m_borderColor = appPal.mid().color();                     // subtle border
	m_borderSelectedColor = Qt::darkBlue;                     // match selected header

	// Header actions area (right-aligned)
	m_headerActionsLayout->setContentsMargins(0, 0, 0, 0);
	m_headerActionsLayout->setSpacing(4);
	m_headerActionsContainer->setLayout(m_headerActionsLayout);

	// Header: [MenuButton] [TitleLabel expanding] ...spacer... [actions]
	auto* headerBox = new QHBoxLayout(m_headerContainer);
	headerBox->setContentsMargins(0, 0, 0, 0);
	headerBox->setSpacing(4);

	m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	headerBox->addWidget(m_selectionMenuButton);
	headerBox->addWidget(m_titleLabel, /*stretch*/ 1);
	headerBox->addStretch(1);
	headerBox->addWidget(m_headerActionsContainer);

	// MenuButton appearance
	m_selectionMenuButton->setText(QString());
	m_selectionMenuButton->setPalette(appPal); // ensure standard control colors

	// Main layout
	m_mainLayout->setContentsMargins(0, 0, 0, 0);
	m_mainLayout->setSpacing(0);
	m_mainLayout->addWidget(m_headerContainer);
	this->setLayout(m_mainLayout);

	// Accessibility
	m_selectionMenuButton->setAccessibleName(QStringLiteral("Selection List"));
	m_titleLabel->setAccessibleName(QStringLiteral("Frame Title"));

	// Header interactions: select on click; emit double-click
	m_headerContainer->installEventFilter(this);
	m_titleLabel->installEventFilter(this);
	m_selectionMenuButton->installEventFilter(this);

	// When a menu item is selected, update title and propagate signals.
	connect(m_selectionMenuButton, &MenuButton::itemSelected, this,
			[this](const QString& item) {
				this->setTitle(item);
				emit this->selectionChanged(item);
				emit this->currentItemChanged(item);
				this->syncMenuCheckedFromTitle();
			});

	// Shortcuts
	auto* closeSc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_W), this);
	connect(closeSc, &QShortcut::activated, this, [this]() {
		if (m_allowClose) emit requestClose();
	});
	auto* editTitleSc = new QShortcut(QKeySequence(Qt::Key_F2), this);
	connect(editTitleSc, &QShortcut::activated, this, [this]() {
		if (m_allowChangeTitle) beginEditTitle();
	});

	updateVisuals();

	// Keep the MenuButton height aligned to the header; width remains free (not forced square).
	QTimer::singleShot(0, this, [this]() { syncMenuButtonSizeToHeader(); });
}

void SelectionFrameWidget::setTitle(const QString& title)
{
	const QString old = this->getTitle();
	if (old == title)
		return;

	m_titleLabel->setText(title);
	emit titleChanged(title);

	// Keep menu's checked action synchronized with the title
	syncMenuCheckedFromTitle();
}

QString SelectionFrameWidget::getTitle() const
{
	return m_titleLabel ? m_titleLabel->text() : QString();
}

void SelectionFrameWidget::setSelectionList(const QStringList& items)
{
	// Populate the menu; MenuButton handles separators and enabled state.
	if (m_selectionMenuButton)
		m_selectionMenuButton->setMenuItems(items);

	// Keep the button icon-only (avoid any text showing up)
	if (m_selectionMenuButton)
		m_selectionMenuButton->setText(QString());

	// Append auxiliary actions (Change Title / Close) if enabled.
	appendAuxMenuActions();

	// If no title set yet, default to the first selectable item.
	if (this->getTitle().isEmpty()) {
		for (const QString& it : items) {
			if (it != QStringLiteral("--")) {
				this->setTitle(it);
				break;
			}
		}
	}

	// Ensure checked state matches the (possibly new) title
	syncMenuCheckedFromTitle();
}

void SelectionFrameWidget::setCurrentItem(const QString& item)
{
	if (item.isEmpty())
		return;

	// Update title (emits titleChanged) and selection signals
	const bool changed = (getTitle() != item);
	setTitle(item);
	if (changed) {
		emit selectionChanged(item);
		emit currentItemChanged(item);
	}
	syncMenuCheckedFromTitle();
}

QString SelectionFrameWidget::currentItem() const
{
	if (!m_selectionMenuButton || !m_selectionMenuButton->menu())
		return QString();

	for (QAction* act : m_selectionMenuButton->menu()->actions()) {
		if (act->isSeparator()) continue;
		if (act->isCheckable() && act->isChecked())
			return act->text();
	}
	return QString();
}

void SelectionFrameWidget::setCentralWidget(QWidget* widget)
{
	if (m_centralWidget == widget)
		return;

	if (m_centralWidget) {
		// Remove previous widget from layout; do not delete (caller may own it).
		m_mainLayout->removeWidget(m_centralWidget);
		m_centralWidget->setParent(nullptr);
		m_centralWidget->hide();
	}

	m_centralWidget = widget;
	if (m_centralWidget) {
		m_centralWidget->setParent(this);
		m_mainLayout->addWidget(m_centralWidget, /*stretch*/ 1);
		m_centralWidget->show();
	}
}

MenuButton* SelectionFrameWidget::menuButton() const
{
	return m_selectionMenuButton;
}

QWidget* SelectionFrameWidget::centralWidget() const
{
	return m_centralWidget;
}

void SelectionFrameWidget::setSelected(bool selected)
{
	// No change
	if (m_selected == selected) {
		return;
	}

	m_selected = selected;
	updateVisuals();
	emit selectedChanged(m_selected);
}

void SelectionFrameWidget::setSelectionListVisible(bool visible)
{
	m_selectionListVisible = visible;
	if (m_selectionMenuButton)
		m_selectionMenuButton->setVisible(visible);
}

void SelectionFrameWidget::setTitleBarVisible(bool visible)
{
	m_titleBarVisible = visible;
	if (m_headerContainer)
		m_headerContainer->setVisible(visible);

	if (visible) {
		QTimer::singleShot(0, this, [this]() { syncMenuButtonSizeToHeader(); });
	}
}

void SelectionFrameWidget::setOuterBorderWidth(int px)
{
	if (m_outerBorderWidth == px) return;
	m_outerBorderWidth = px;
	updateVisuals();
}

void SelectionFrameWidget::setTitleColors(const QColor& foreground, const QColor& background)
{
	m_titleFg = foreground;
	m_titleBg = background;
	updateVisuals();
}

void SelectionFrameWidget::setSelectedTitleColors(const QColor& foreground, const QColor& background)
{
	m_selectedTitleFg = foreground;
	m_selectedTitleBg = background;
	updateVisuals();
}

void SelectionFrameWidget::setBorderColors(const QColor& normal, const QColor& selected)
{
	m_borderColor = normal;
	m_borderSelectedColor = selected;
	updateVisuals();
}

// Individual color property setters

void SelectionFrameWidget::setTitleForegroundColor(const QColor& c)
{
	if (m_titleFg == c) return;
	m_titleFg = c;
	updateVisuals();
}

void SelectionFrameWidget::setTitleBackgroundColor(const QColor& c)
{
	if (m_titleBg == c) return;
	m_titleBg = c;
	updateVisuals();
}

void SelectionFrameWidget::setSelectedTitleForegroundColor(const QColor& c)
{
	if (m_selectedTitleFg == c) return;
	m_selectedTitleFg = c;
	updateVisuals();
}

void SelectionFrameWidget::setSelectedTitleBackgroundColor(const QColor& c)
{
	if (m_selectedTitleBg == c) return;
	m_selectedTitleBg = c;
	updateVisuals();
}

void SelectionFrameWidget::setBorderColor(const QColor& c)
{
	if (m_borderColor == c) return;
	m_borderColor = c;
	updateVisuals();
}

void SelectionFrameWidget::setBorderSelectedColor(const QColor& c)
{
	if (m_borderSelectedColor == c) return;
	m_borderSelectedColor = c;
	updateVisuals();
}

void SelectionFrameWidget::setAllowChangeTitle(bool on)
{
	if (m_allowChangeTitle == on) return;
	m_allowChangeTitle = on;
	appendAuxMenuActions();
}

void SelectionFrameWidget::setAllowClose(bool on)
{
	if (m_allowClose == on) return;
	m_allowClose = on;
	appendAuxMenuActions();
}

QAction* SelectionFrameWidget::addHeaderAction(QAction* action)
{
	if (!action) return nullptr;
	auto* btn = new QToolButton(m_headerActionsContainer);
	btn->setDefaultAction(action);
	btn->setAutoRaise(true);
	m_headerActionsLayout->addWidget(btn);
	return action;
}

bool SelectionFrameWidget::eventFilter(QObject* watched, QEvent* event)
{
	if (watched == m_headerContainer || watched == m_titleLabel || watched == m_selectionMenuButton) {
		if ((watched == m_headerContainer || watched == m_titleLabel) &&
			(event->type() == QEvent::Resize ||
			event->type() == QEvent::LayoutRequest ||
			event->type() == QEvent::FontChange ||
			event->type() == QEvent::StyleChange ||
			event->type() == QEvent::PaletteChange ||
			event->type() == QEvent::Show))
		{
			syncMenuButtonSizeToHeader();
		}

		if (event->type() == QEvent::MouseButtonPress) {
			// Select and focus this view when its title bar or menu button is pressed
			setSelected(true);
			setFocus(Qt::MouseFocusReason);
			return false; // allow normal processing
		}
		if (event->type() == QEvent::MouseButtonDblClick) {
			setSelected(true);
			setFocus(Qt::MouseFocusReason);
			emit doubleClicked();
			return true; // consume double-click
		}
	}
	return QFrame::eventFilter(watched, event);
}

void SelectionFrameWidget::keyPressEvent(QKeyEvent* e)
{
	switch (e->key()) {
		case Qt::Key_Enter:
		case Qt::Key_Return:
		case Qt::Key_Space:
		setSelected(true);
		e->accept();
		return;
		case Qt::Key_F2:
		if (m_allowChangeTitle) { beginEditTitle(); e->accept(); return; }
		break;
		default:
		break;
	}

	if ((e->modifiers() & Qt::ControlModifier) && e->key() == Qt::Key_W) {
		if (m_allowClose) { emit requestClose(); e->accept(); return; }
	}

	QFrame::keyPressEvent(e);
}

void SelectionFrameWidget::updateVisuals()
{
	// Compute colors based on selection
	const QColor bg = m_selected ? m_selectedTitleBg : m_titleBg;
	const QColor fg = m_selected ? m_selectedTitleFg : m_titleFg;
	const QColor border = m_selected ? m_borderSelectedColor : m_borderColor;

	// Header background/foreground via stylesheet (scoped by objectName)
	const QString headerStyle = QStringLiteral(
		"#SelectionFrameHeader { background-color: %1; } "
		"#SelectionFrameTitleLabel { color: %2; }")
		.arg(bg.name(), fg.name());
	m_headerContainer->setStyleSheet(headerStyle);

	// Outer border on the frame itself
	const QString frameStyle = QStringLiteral(
		"#SelectionFrameWidget { border: %1px solid %2; }")
		.arg(QString::number(m_outerBorderWidth), border.name());
	this->setStyleSheet(frameStyle);
}

void SelectionFrameWidget::appendAuxMenuActions()
{
	if (!m_selectionMenuButton || !m_selectionMenuButton->menu())
		return;

	QMenu* m = m_selectionMenuButton->menu();

	// First remove any previous aux actions we might have added
	// (identified by their objectName)
	for (QAction* act : m->actions()) {
		if (act->objectName() == QLatin1String("SelectionFrame-ChangeTitle") ||
			act->objectName() == QLatin1String("SelectionFrame-Close")) {
			m->removeAction(act);
			delete act;
		}
	}
	// Remove trailing separator if it became dangling
	if (!m->actions().isEmpty() && m->actions().back()->isSeparator())
	{
		QAction* sep = m->actions().back();
		m->removeAction(sep);
		delete sep;
	}

	const bool needAux = (m_allowChangeTitle || m_allowClose);
	const bool hasItems = !m->actions().isEmpty();
	if (!needAux) return;

	// If menu already has items, add a separator before aux actions
	if (hasItems) m->addSeparator();

	if (m_allowChangeTitle) {
		QAction* change = m->addAction(tr("Change Title..."));
		change->setObjectName(QStringLiteral("SelectionFrame-ChangeTitle"));
		connect(change, &QAction::triggered, this, [this]() { beginEditTitle(); });
	}
	if (m_allowClose) {
		QAction* close = m->addAction(tr("Close"));
		close->setObjectName(QStringLiteral("SelectionFrame-Close"));
		connect(close, &QAction::triggered, this, [this]() { emit requestClose(); });
	}
}

void SelectionFrameWidget::syncMenuCheckedFromTitle()
{
	if (!m_selectionMenuButton || !m_selectionMenuButton->menu())
		return;

	const QString title = getTitle();
	QMenu* m = m_selectionMenuButton->menu();
	QAction* firstCheckable = nullptr;
	for (QAction* act : m->actions()) {
		if (act->isSeparator()) continue;
		if (!act->isCheckable()) continue; // skip aux actions
		if (!firstCheckable) firstCheckable = act;
		if (act->text() == title) {
			act->setChecked(true);
			return;
		}
	}
	// If no exact match found but we have a checkable item and no title, check first
	if (title.isEmpty() && firstCheckable) {
		firstCheckable->setChecked(true);
	}
}

void SelectionFrameWidget::beginEditTitle()
{
	const QString current = getTitle();
	bool ok = false;
	const QString newTitle = QInputDialog::getText(
		this, tr("Change Title"), tr("Title:"), QLineEdit::Normal, current, &ok);
	if (ok && !newTitle.isEmpty() && newTitle != current) {
		setTitle(newTitle);
		// Note: editing title does not change the current item unless it matches an item
		syncMenuCheckedFromTitle();
	}
}

void SelectionFrameWidget::syncMenuButtonSizeToHeader()
{
	if (!m_selectionMenuButton || !m_headerContainer) return;
	int h = m_headerContainer->height();
	if (h <= 0) h = m_headerContainer->sizeHint().height();
	if (h > 0) {
		// Keep height aligned to header, but do not force width (let MenuButton keep indicator-only width)
		m_selectionMenuButton->setFixedHeight(h);
		// Optional: keep a stable vertical policy; width is controlled by MenuButton
		m_selectionMenuButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
	}
}

void SelectionFrameWidget::resizeEvent(QResizeEvent* e)
{
	QFrame::resizeEvent(e);
	syncMenuButtonSizeToHeader();
}

// Ensure first-time sizing right after the widget is shown
void SelectionFrameWidget::showEvent(QShowEvent* e)
{
	QFrame::showEvent(e);
	// Header height is reliable now; enforce square size
	syncMenuButtonSizeToHeader();
	// Safety: run once more after pending layout passes
	QTimer::singleShot(0, this, [this]() { syncMenuButtonSizeToHeader(); });
}

// Make keyboard/ programmatic focus also highlight this title bar
void SelectionFrameWidget::focusInEvent(QFocusEvent* e)
{
	QFrame::focusInEvent(e);
	setSelected(true);
}

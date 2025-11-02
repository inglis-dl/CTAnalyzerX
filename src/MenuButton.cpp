/*=========================================================================

  Library:   CTK (adapted)

=========================================================================*/

#include "MenuButton.h"

#include <QApplication>
#include <QDebug>
#include <QDesktopWidget>
#include <QLayout>
#include <QMouseEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QAction>
#include <QActionGroup>
#include <QStringList>

namespace {
	// Compute the style-accurate indicator width and total control width
	static int indicatorPixelWidth(const QWidget* w)
	{
		QStyleOptionButton opt;
		opt.initFrom(w);
		int ind = w->style()->pixelMetric(QStyle::PM_MenuButtonIndicator, &opt, w);
		if (ind <= 0) ind = 14; // fallback
		return ind;
	}

	static int frameHMargin(const QWidget* w)
	{
		QStyleOptionButton opt;
		opt.initFrom(w);
		// Frame/border thickness on each side
		const int frame = w->style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &opt, w);
		return frame > 0 ? frame : 0;
	}

	static int indicatorOnlyTotalWidth(const QWidget* w)
	{
		const int ind = indicatorPixelWidth(w);
		const int frame = frameHMargin(w);
		// Total = indicator width + left/right frame
		return ind + 2 * frame;
	}
} // namespace

// Private implementation (kept local to cpp)
class MenuButtonPrivate
{
public:
	explicit MenuButtonPrivate(MenuButton& q) : q_ptr(&q), m_showMenu(false), m_indicatorOnly(true) {}

	QRect indicatorRect() const
	{
		QStyleOptionButton option;
		q_ptr->initStyleOption(&option);

		// Right-aligned indicator rect with style-accurate width
		const int indW = indicatorPixelWidth(q_ptr);
		QRect full = q_ptr->style()->visualRect(option.direction, option.rect, option.rect);
		QRect ind(full.right() - indW + 1, full.top(), indW, full.height());
		ind = q_ptr->style()->visualRect(option.direction, option.rect, ind);
		return ind;
	}

	void enforceIndicatorOnlyWidth()
	{
		if (!m_indicatorOnly) return;
		const int w = indicatorOnlyTotalWidth(q_ptr);
		// Hard lock so layout/style can't expand it
		q_ptr->setSizePolicy(QSizePolicy::Fixed, q_ptr->sizePolicy().verticalPolicy());
		q_ptr->setMinimumWidth(w);
		q_ptr->setMaximumWidth(w);
		q_ptr->resize(w, q_ptr->height());
	}

	void releaseWidthLock()
	{
		q_ptr->setMinimumWidth(0);
		q_ptr->setMaximumWidth(QWIDGETSIZE_MAX);
		// Caller should updateGeometry() after toggling
	}

	MenuButton* const q_ptr;
	bool m_showMenu;
	bool m_indicatorOnly;

	// Store items to support Q_PROPERTY READ accessor
	QStringList m_items;
};

// Constructors / dtor
MenuButton::MenuButton(QWidget* parent)
	: QPushButton(parent)
	, d_ptr(new MenuButtonPrivate(*this))
{
	// Provide a menu instance; behavior is managed in mousePressEvent().
	setMenu(new QMenu(this));

	// Default to indicator-only; lock width to style-accurate minimal width
	if (d_ptr->m_indicatorOnly) {
		d_ptr->enforceIndicatorOnlyWidth();
	}
}

MenuButton::MenuButton(const QString& title, QWidget* parent)
	: QPushButton(title, parent)
	, d_ptr(new MenuButtonPrivate(*this))
{
	setMenu(new QMenu(this));

	if (d_ptr->m_indicatorOnly) {
		d_ptr->enforceIndicatorOnlyWidth();
	}
}

MenuButton::~MenuButton()
{
	delete d_ptr;
}

// Minimum/size hints
QSize MenuButton::minimumSizeHint() const
{
	if (d_ptr->m_indicatorOnly) {
		// Tight minimal width matching style metrics; height from style
		QSize min = QPushButton::minimumSizeHint();
		return QSize(indicatorOnlyTotalWidth(this), min.height());
	}
	QSize min = QPushButton::minimumSizeHint();
	return QSize(min.width() + indicatorPixelWidth(this), min.height());
}

QSize MenuButton::sizeHint() const
{
	if (d_ptr->m_indicatorOnly) {
		QSize sh = QPushButton::sizeHint();
		return QSize(indicatorOnlyTotalWidth(this), sh.height());
	}
	QSize sh = QPushButton::sizeHint();
	return QSize(sh.width() + indicatorPixelWidth(this), sh.height());
}

// Paint the button with the indicator area
void MenuButton::paintEvent(QPaintEvent* _event)
{
	Q_UNUSED(_event);
	MenuButtonPrivate* d = d_ptr;
	QStylePainter painter(this);
	QStyleOptionButton option;
	initStyleOption(&option);

	const QRect downArrowRect = d->indicatorRect();

	// In indicator-only mode, paint only the indicator area in the (narrow) control
	if (d->m_indicatorOnly)
	{
		// Draw bevel for the whole (narrow) rect
		QStyleOptionButton clipped = option;
		clipped.rect = rect();
		clipped.features &= ~QStyleOptionButton::HasMenu;
		painter.drawControl(QStyle::CE_PushButtonBevel, clipped);

		// Draw the drop-down arrow within the indicator rect
		const int borderSize = 2;
		QStyleOption indicatorOpt;
		indicatorOpt.init(this);
		indicatorOpt.rect = downArrowRect.adjusted(borderSize, borderSize, -borderSize, -borderSize);
		painter.drawPrimitive(QStyle::PE_IndicatorArrowDown, indicatorOpt);
		return;
	}

	// Split mode (existing behavior)
	bool drawIndicatorBackground =
		(option.state & QStyle::State_Sunken) || (option.state & QStyle::State_On);

	// Draw button bevel across the full rect
	option.features &= ~QStyleOptionButton::HasMenu;
	if (this->menu() && (this->menu()->isVisible() || d->m_showMenu))
	{
		option.state &= ~QStyle::State_Sunken;
		option.state |= QStyle::State_Raised;
	}
	painter.drawControl(QStyle::CE_PushButtonBevel, option);

	if (drawIndicatorBackground)
	{
		QPixmap cache = QPixmap(option.rect.size());
		cache.fill(Qt::transparent);
		QPainter cachePainter(&cache);
		option.state &= ~QStyle::State_Sunken;
		option.state |= QStyle::State_Raised;
		option.state &= ~QStyle::State_On;
		option.state |= QStyle::State_Off;
		option.state &= ~QStyle::State_MouseOver;
		this->style()->drawControl(QStyle::CE_PushButtonBevel, &option, &cachePainter, this);
		painter.drawItemPixmap(downArrowRect, Qt::AlignLeft | Qt::AlignTop, cache.copy(downArrowRect));
	}

	// Separator lines
	{
		QColor buttonColor = this->palette().button().color();
		painter.setPen(buttonColor.darker(130));
		const int borderSize = 2;
		painter.drawLine(QPoint(downArrowRect.left() - 1, downArrowRect.top() + borderSize),
						 QPoint(downArrowRect.left() - 1, downArrowRect.bottom() - borderSize));
		painter.setPen(this->palette().light().color());
		painter.drawLine(QPoint(downArrowRect.left(), downArrowRect.top() + borderSize),
						 QPoint(downArrowRect.left(), downArrowRect.bottom() - borderSize));
	}

	// Arrow
	{
		const int borderSize = 2;
		QStyleOption indicatorOpt;
		indicatorOpt.init(this);
		indicatorOpt.rect = downArrowRect.adjusted(borderSize, borderSize, -borderSize, -borderSize);
		painter.drawPrimitive(QStyle::PE_IndicatorArrowDown, indicatorOpt);
	}

	// Icon & text to the left of the indicator
	QStyleOptionButton left = option;
	left.rect.setRight(downArrowRect.left());
	painter.drawControl(QStyle::CE_PushButtonLabel, left);
}

bool MenuButton::hitButton(const QPoint& _pos) const
{
	if (d_ptr->m_indicatorOnly)
	{
		// Only the indicator area is clickable in indicator-only mode
		return d_ptr->indicatorRect().contains(_pos);
	}
	MenuButtonPrivate* d = d_ptr;
	return !d->indicatorRect().contains(_pos) && this->QPushButton::hitButton(_pos);
}

void MenuButton::initStyleOption(QStyleOptionButton* option) const
{
	this->QPushButton::initStyleOption(option);
}

void MenuButton::mousePressEvent(QMouseEvent* e)
{
	MenuButtonPrivate* d = d_ptr;

	if (d->m_indicatorOnly)
	{
		// Only clicks in the indicator open the menu; otherwise let parent handle selection
		if (d->indicatorRect().contains(e->pos()))
		{
			d->m_showMenu = true;
			this->showMenu();
			d->m_showMenu = false;
			e->accept();
			return;
		}
		e->ignore();
		return;
	}

	// Split mode (unchanged)
	this->disconnect(this, SIGNAL(pressed()), this, SLOT(_q_popupPressed()));
	this->QPushButton::mousePressEvent(e);
	if (e->isAccepted())
	{
		return;
	}
	if (d->indicatorRect().contains(e->pos()))
	{
		d->m_showMenu = true;
		this->showMenu();
		d->m_showMenu = false;
		e->accept();
	}
}

// Populate menu and emit itemSelected when an action is chosen
void MenuButton::setMenuItems(const QStringList& items)
{
	bool changed = (d_ptr->m_items != items);
	d_ptr->m_items = items;

	if (!this->menu())
	{
		setMenu(new QMenu(this));
	}
	QMenu* m = this->menu();
	m->clear();

	QActionGroup* group = new QActionGroup(m);
	group->setExclusive(true);

	bool lastWasSeparator = true;
	int selectableCount = 0;

	for (const QString& it : items)
	{
		if (it == QStringLiteral("--"))
		{
			if (!lastWasSeparator && selectableCount > 0)
			{
				m->addSeparator();
				lastWasSeparator = true;
			}
			continue;
		}

		lastWasSeparator = false;

		QAction* act = m->addAction(it);
		act->setCheckable(true);
		act->setActionGroup(group);

		connect(act, &QAction::triggered, this, [this, act]() {
			emit itemSelected(act->text());
		});

		++selectableCount;
	}

	const auto actions = m->actions();
	if (!actions.isEmpty() && actions.back()->isSeparator())
	{
		QAction* sep = actions.back();
		m->removeAction(sep);
		delete sep;
	}

	this->setEnabled(selectableCount > 0);

	if (changed) {
		emit menuItemsChanged();
	}
}

QStringList MenuButton::menuItems() const
{
	return d_ptr->m_items;
}

void MenuButton::setIndicatorOnly(bool enabled)
{
	if (d_ptr->m_indicatorOnly == enabled)
		return;
	d_ptr->m_indicatorOnly = enabled;

	if (enabled) {
		d_ptr->enforceIndicatorOnlyWidth();
	}
	else {
		d_ptr->releaseWidthLock();
	}
	updateGeometry();
	update();
}

bool MenuButton::indicatorOnly() const
{
	return d_ptr->m_indicatorOnly;
}

// Keep the width correct if the style or font changes (theme switch, DPI, etc.)
bool eventIsStyleRelayout(QEvent::Type t)
{
	return t == QEvent::StyleChange || t == QEvent::Polish || t == QEvent::PolishRequest
		|| t == QEvent::FontChange || t == QEvent::LayoutRequest;
}

bool MenuButton::event(QEvent* ev)
{
	if (d_ptr->m_indicatorOnly && eventIsStyleRelayout(ev->type())) {
		d_ptr->enforceIndicatorOnlyWidth();
	}
	return QPushButton::event(ev);
}

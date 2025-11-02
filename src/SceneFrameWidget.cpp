#include "SceneFrameWidget.h"
#include "MenuButton.h"

#include <vtkRenderWindow.h>
#include <QAction>
#include <QMenu>
#include <QTimer>
#include <QShortcut>
#include <QKeySequence>
#include <QSettings>
#include <QCursor>
#include <QVBoxLayout>

SceneFrameWidget::SceneFrameWidget(QWidget* parent)
	: SelectionFrameWidget(parent)
{
	setTitleBarVisible(true);
	setSelectionListVisible(true);

	// React to orientation changes
	connect(this, &SceneFrameWidget::viewOrientationChanged, this, [this](SceneFrameWidget::ViewOrientation) {
		// Keep menu/radio in sync
		setCurrentItem(orientationLabel(m_viewOrientation));
		// Removed auto-orthogonalize
	});

	createDefaultMenuAndActions();
	wireShortcuts();

	// Defer action-state sync to avoid virtual dispatch during base construction
	QTimer::singleShot(0, this, [this]() { updateActionEnableStates(); });

	// Optional context menu (right-click anywhere on body)
	setUseContextMenu(true);
}

void SceneFrameWidget::render()
{
	if (auto* rw = getRenderWindow()) {
		rw->Render();
	}
}

void SceneFrameWidget::setViewOrientation(ViewOrientation orient)
{
	if (m_viewOrientation == orient)
		return;

	m_viewOrientation = orient;
}

void SceneFrameWidget::flipHorizontal()
{
	if (!m_allowHorizontalViewFlipping || !canFlipHorizontal()) return;
	// Subclasses override to implement
}

void SceneFrameWidget::flipVertical()
{
	if (!m_allowVerticalViewFlipping || !canFlipVertical()) return;
	// Subclasses override to implement
}

void SceneFrameWidget::flip(int on)
{
	Q_UNUSED(on);
	// Subclasses override to implement toggle-based flipping if desired
}

void SceneFrameWidget::rotateCamera(double degrees)
{
	if (!canRotate()) return;
	if (degrees > 0 && !m_allowClockwiseViewRotation) return;
	if (degrees < 0 && !m_allowCounterClockwiseViewRotation) return;
	// Subclasses override to implement
}

void SceneFrameWidget::orthogonalizeView()
{
	// Subclasses override to re-orthogonalize cameras for current orientation
}

void SceneFrameWidget::createDefaultMenuAndActions()
{
	// Orientation entries in the selection list
	setSelectionList({ QStringLiteral("XY"),
					   QStringLiteral("YZ"),
					   QStringLiteral("XZ") });

	// React to menu selection -> orientation
	connect(this, &SelectionFrameWidget::selectionChanged, this,
			[this](const QString& item) { handleOrientationSelected(item); });

	// Build extra actions in the same menu
	QMenu* menu = menuButton() ? menuButton()->menu() : nullptr;
	if (menu) {
		menu->addSeparator();
		m_actFlipH = menu->addAction(tr("Flip Horizontal"));
		m_actFlipV = menu->addAction(tr("Flip Vertical"));
		m_actRotCw = menu->addAction(tr("Rotate +90°"));
		m_actRotCcw = menu->addAction(tr("Rotate -90°"));
		m_actOrtho = menu->addAction(tr("Orthogonalize View"));
		m_actReset = menu->addAction(tr("Reset Camera"));

		connect(m_actFlipH, &QAction::triggered, this, [this]() { flipHorizontal(); });
		connect(m_actFlipV, &QAction::triggered, this, [this]() { flipVertical(); });
		connect(m_actRotCw, &QAction::triggered, this, [this]() { rotateCamera(+90.0); });
		connect(m_actRotCcw, &QAction::triggered, this, [this]() { rotateCamera(-90.0); });
		connect(m_actOrtho, &QAction::triggered, this, [this]() { orthogonalizeView(); });
		connect(m_actReset, &QAction::triggered, this, [this]() { resetCamera(); });
	}
}

void SceneFrameWidget::wireShortcuts()
{
	new QShortcut(QKeySequence(Qt::Key_1), this, [this]() { setViewOrientationToXY(); });
	new QShortcut(QKeySequence(Qt::Key_2), this, [this]() { setViewOrientationToYZ(); });
	new QShortcut(QKeySequence(Qt::Key_3), this, [this]() { setViewOrientationToXZ(); });

	new QShortcut(QKeySequence(Qt::Key_H), this, [this]() { flipHorizontal(); });
	new QShortcut(QKeySequence(Qt::Key_V), this, [this]() { flipVertical(); });

	new QShortcut(QKeySequence(Qt::Key_R), this, [this]() { resetCamera(); });
	new QShortcut(QKeySequence(Qt::Key_O), this, [this]() { orthogonalizeView(); });

	new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), this, [this]() { rotateCamera(+90.0); });
	new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), this, [this]() { rotateCamera(-90.0); });
}

void SceneFrameWidget::updateActionEnableStates()
{
	const bool haveRW = getRenderWindow() != nullptr;

	// Orientation
	const bool reorient2D = m_allowReOrientation2D && haveRW;
	// Flip/Rotate
	const bool flipH = m_allowHorizontalViewFlipping && haveRW && canFlipHorizontal();
	const bool flipV = m_allowVerticalViewFlipping && haveRW && canFlipVertical();
	const bool rotCW = m_allowClockwiseViewRotation && haveRW && canRotate();
	const bool rotCCW = m_allowCounterClockwiseViewRotation && haveRW && canRotate();

	if (m_actFlipH)  m_actFlipH->setEnabled(flipH);
	if (m_actFlipV)  m_actFlipV->setEnabled(flipV);
	if (m_actRotCw)  m_actRotCw->setEnabled(rotCW);
	if (m_actRotCcw) m_actRotCcw->setEnabled(rotCCW);
	if (m_actOrtho)  m_actOrtho->setEnabled(haveRW);
	if (m_actReset)  m_actReset->setEnabled(haveRW);

	Q_UNUSED(reorient2D);
}

void SceneFrameWidget::setUseContextMenu(bool v)
{
	m_useContextMenu = v;
	if (m_useContextMenu) {
		setContextMenuPolicy(Qt::CustomContextMenu);
		connect(this, &QWidget::customContextMenuRequested, this, [this]() { showContextMenuAtCursor(); });
	}
	else {
		setContextMenuPolicy(Qt::NoContextMenu);
		disconnect(this, &QWidget::customContextMenuRequested, nullptr, nullptr);
	}
}

void SceneFrameWidget::saveState(QSettings& s, const QString& keyPrefix) const
{
	s.beginGroup(keyPrefix);
	s.setValue(QStringLiteral("title"), getTitle());
	s.setValue(QStringLiteral("orientation"), static_cast<int>(m_viewOrientation));
	s.setValue(QStringLiteral("allowReOrientation2D"), m_allowReOrientation2D);
	s.setValue(QStringLiteral("allowReOrientation3D"), m_allowReOrientation3D);
	s.setValue(QStringLiteral("allowVerticalFlip"), m_allowVerticalViewFlipping);
	s.setValue(QStringLiteral("allowHorizontalFlip"), m_allowHorizontalViewFlipping);
	s.setValue(QStringLiteral("allowRotateCW"), m_allowClockwiseViewRotation);
	s.setValue(QStringLiteral("allowRotateCCW"), m_allowCounterClockwiseViewRotation);
	s.setValue(QStringLiteral("interactiveMode"), m_interactiveMode);
	s.setValue(QStringLiteral("useContextMenu"), m_useContextMenu);
	// removed autoOrthogonalize
	s.endGroup();
}

void SceneFrameWidget::restoreState(QSettings& s, const QString& keyPrefix)
{
	s.beginGroup(keyPrefix);
	setTitle(s.value(QStringLiteral("title"), getTitle()).toString());
	{
		const int raw = s.value(QStringLiteral("orientation"), static_cast<int>(m_viewOrientation)).toInt();
		switch (raw) {
			case VIEW_ORIENTATION_XY:
			case VIEW_ORIENTATION_YZ:
			case VIEW_ORIENTATION_XZ:
			setViewOrientation(static_cast<ViewOrientation>(raw));
			break;
			default:
			break; // ignore invalid
		}
	}
	setAllowReOrientation2D(s.value(QStringLiteral("allowReOrientation2D"), m_allowReOrientation2D).toBool());
	setAllowReOrientation3D(s.value(QStringLiteral("allowReOrientation3D"), m_allowReOrientation3D).toBool());
	setAllowVerticalViewFlipping(s.value(QStringLiteral("allowVerticalFlip"), m_allowVerticalViewFlipping).toBool());
	setAllowHorizontalViewFlipping(s.value(QStringLiteral("allowHorizontalFlip"), m_allowHorizontalViewFlipping).toBool());
	setAllowClockwiseViewRotation(s.value(QStringLiteral("allowRotateCW"), m_allowClockwiseViewRotation).toBool());
	setAllowCounterClockwiseViewRotation(s.value(QStringLiteral("allowRotateCCW"), m_allowCounterClockwiseViewRotation).toBool());
	setInteractiveMode(s.value(QStringLiteral("interactiveMode"), m_interactiveMode).toBool());
	setUseContextMenu(s.value(QStringLiteral("useContextMenu"), m_useContextMenu).toBool());
	// removed autoOrthogonalize
	s.endGroup();

	// Sync menu checkmark with restored orientation
	setCurrentItem(orientationLabel(m_viewOrientation));
	updateActionEnableStates();
}

void SceneFrameWidget::handleOrientationSelected(const QString& item)
{
	const ViewOrientation orient = labelToOrientation(item);
	setViewOrientation(orient);

	// When the menu item is an orientation, reflect it in the title bar label.
	if (item == QLatin1String("XY") || item == QLatin1String("XZ") || item == QLatin1String("YZ")) {
		setTitle(orientationLabel(orient));
	}
}

void SceneFrameWidget::showContextMenuAtCursor()
{
	if (auto* mb = menuButton()) {
		if (auto* m = mb->menu()) {
			m->exec(QCursor::pos());
		}
	}
}

QString SceneFrameWidget::orientationLabel(ViewOrientation orient) const
{
	switch (orient) {
		case VIEW_ORIENTATION_XY: return QStringLiteral("XY");
		case VIEW_ORIENTATION_YZ: return QStringLiteral("YZ");
		case VIEW_ORIENTATION_XZ: return QStringLiteral("XZ");
		default: return QString();
	}
}

SceneFrameWidget::ViewOrientation SceneFrameWidget::labelToOrientation(const QString& label) const
{
	if (label == QLatin1String("XY")) return VIEW_ORIENTATION_XY;
	if (label == QLatin1String("YZ")) return VIEW_ORIENTATION_YZ;
	if (label == QLatin1String("XZ")) return VIEW_ORIENTATION_XZ;
	return m_viewOrientation; // no change on unknown label
}

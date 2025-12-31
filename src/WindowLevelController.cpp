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
#include <QGraphicsEllipseItem>
#include <QGraphicsLineItem>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>

using namespace QtCharts;

// Small utility: NodeItem is a QGraphicsEllipseItem that knows its index and paints a
// number while pressed. It's implemented here to keep header clean.
namespace {
	class NodeItem : public QGraphicsEllipseItem
	{
	public:
		explicit NodeItem(int idx, QGraphicsItem* parent = nullptr)
			: QGraphicsEllipseItem(parent), m_index(idx), m_baseZ(1000.0)
		{
			setRect(-m_r, -m_r, 2 * m_r, 2 * m_r); // center-based
			setPen(QPen(Qt::black, 1));
			setBrush(QBrush(Qt::white));
			setFlag(QGraphicsItem::ItemIsMovable, true);
			setFlag(QGraphicsItem::ItemSendsScenePositionChanges, true);
			setAcceptHoverEvents(true);
			// initial z will be set via setBaseZ() by the controller
			setCursor(Qt::ArrowCursor);
		}

		void setPressed(bool p) { m_pressed = p; update(); }
		bool pressed() const { return m_pressed; }
		int index() const { return m_index; }

		void setBaseZ(qreal z) { m_baseZ = z; setZValue(m_baseZ); }
		qreal baseZ() const { return m_baseZ; }

	protected:
		void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override
		{
			Q_UNUSED(option); Q_UNUSED(widget);
			// fill color depends on pressed state
			if (m_pressed) painter->setBrush(QColor(173, 216, 230)); // light blue
			else painter->setBrush(QBrush(Qt::white));
			painter->setPen(pen());
			painter->setRenderHint(QPainter::Antialiasing, true);
			painter->drawEllipse(rect());

			// show index text while pressed
			if (m_pressed) {
				QFont f = painter->font();
				f.setBold(true);
				f.setPointSizeF(f.pointSizeF() * 0.9);
				painter->setFont(f);
				painter->setPen(Qt::black);
				painter->drawText(rect(), Qt::AlignCenter, QString::number(m_index + 1));
			}
		}

		void mousePressEvent(QGraphicsSceneMouseEvent* ev) override
		{
			// raise this node visually so it receives events during drag
			setZValue(m_baseZ + m_raiseDelta);
			m_pressed = true;
			update();
			QGraphicsEllipseItem::mousePressEvent(ev);
		}

		void mouseReleaseEvent(QGraphicsSceneMouseEvent* ev) override
		{
			// restore base z and visual state
			setZValue(m_baseZ);
			m_pressed = false;
			update();
			QGraphicsEllipseItem::mouseReleaseEvent(ev);
		}

		QVariant itemChange(GraphicsItemChange change, const QVariant& value) override
		{
			// default behaviour; actual position clamping handled by controller's constrainNodePosition
			return QGraphicsEllipseItem::itemChange(change, value);
		}

	private:
		const int m_index;
		bool m_pressed = false;
		qreal m_baseZ;
		const double m_r = 6.0;
		const qreal m_raiseDelta = 100.0; // amount to raise active node above others
	};
} // anonymous namespace

// initInteractiveLine: create nodes as top-level scene items (not children of m_chart)
void WindowLevelController::initInteractiveLine()
{
	if (!m_chart || m_interactiveInitialized) return;
	QRectF plot = m_chart->plotArea();
	if (plot.isEmpty() || !m_chart->scene()) return;

	// create or update plot-area outline (child of chart, below nodes)
	const qreal outlineZ = 900.0; // below nodes (nodes will be > outlineZ)
	if (!m_plotRect) {
		m_plotRect = new QGraphicsRectItem(plot, m_chart);
		m_plotRect->setPen(QPen(Qt::black, 1));
		m_plotRect->setBrush(Qt::NoBrush);
		m_plotRect->setZValue(outlineZ);
	}
	else {
		m_plotRect->setRect(plot);
		m_plotRect->setParentItem(m_chart);
		m_plotRect->setZValue(outlineZ);
	}

	// preferred base Z for nodes; choose values so center nodes (1 and 2 indexes 1/2) are above partners
	const qreal baseNodeZ = 1000.0;
	// ordering: node0 (left), node1 (center-left), node2 (center-right), node3 (right)
	// we want node1/node2 to take precedence over node0/node3 when overlapping:
	// give node1/node2 slightly higher base Z.
	for (int i = 0; i < 4; ++i) {
		NodeItem* node = new NodeItem(i /*index*/, /*parent*/ nullptr);
		// compute base Z: center nodes higher
		qreal z = baseNodeZ + ((i == 1 || i == 2) ? 2.0 : 0.0);
		node->setBaseZ(z);

		// compute desired chart-local position
		qreal x;
		if (i == 0) x = plot.left();                              // center on left edge
		else if (i == 3) x = plot.right();                        // center on right edge
		else x = plot.left() + (i * plot.width() / 3.0);
		qreal y = plot.bottom() - 10.0;
		QPointF chartLocalPos(x, y);
		// map to scene coordinates and add as scene item
		QPointF scenePos = m_chart->mapToScene(chartLocalPos);
		m_chart->scene()->addItem(node);
		node->setPos(scenePos);
		m_nodes[i] = node;
		// ensure node sends position changes
		m_nodes[i]->setFlag(QGraphicsItem::ItemSendsScenePositionChanges, true);
	}

	// store fixed Xs in chart-local coordinates (use chart-local values)
	m_fixedX[0] = m_nodes[0] ? m_chart->mapFromScene(m_nodes[0]->pos()).x() : 0.0;
	m_fixedX[3] = m_nodes[3] ? m_chart->mapFromScene(m_nodes[3]->pos()).x() : 0.0;

	// create three segment lines as children of chart (use chart-local coords)
	if (!m_segLeft) {
		m_segLeft = new QGraphicsLineItem(m_chart);
		m_segLeft->setPen(QPen(Qt::black, 2));
		m_segLeft->setZValue(950);
	}
	if (!m_segMid) {
		m_segMid = new QGraphicsLineItem(m_chart);
		m_segMid->setPen(QPen(Qt::black, 2));
		m_segMid->setZValue(950);
	}
	if (!m_segRight) {
		m_segRight = new QGraphicsLineItem(m_chart);
		m_segRight->setPen(QPen(Qt::black, 2));
		m_segRight->setZValue(950);
	}

	// initialize last-known node positions (chart-local)
	for (int i = 0; i < 4; ++i) {
		if (m_nodes[i]) m_lastNodePos[i] = m_chart->mapFromScene(m_nodes[i]->pos());
		else m_lastNodePos[i] = QPointF();
	}

	m_interactiveInitialized = true;
	updateInteractiveLine();
}

QPointF WindowLevelController::constrainNodePosition(int idx, const QPointF& desired)
{
	if (!m_chart || !m_nodes[0]) return desired;

	QRectF plot = m_chart->plotArea();
	QPointF p = desired;

	// helpers: chart-local (Qt) y increases downward; convert to y-up for spec logic
	const double chartTop = plot.top();
	const double chartBottom = plot.bottom();
	const double chartHeight = chartBottom - chartTop; // >= 0

	auto chartY_to_upY = [&](double yChart) -> double { return chartBottom - yChart; };
	auto upY_to_chartY = [&](double yUp) -> double { return chartBottom - yUp; };

	// Clamp center to chart edges (centers constrained; circles may overlap visually)
	const double minX = plot.left();
	const double maxX = plot.right();
	const double minChartY = chartTop;
	const double maxChartY = chartBottom;

	if (p.x() < minX) p.setX(minX);
	if (p.x() > maxX) p.setX(maxX);
	if (p.y() < minChartY) p.setY(minChartY);
	if (p.y() > maxChartY) p.setY(maxChartY);

	// convert to y-up
	double pUpY = chartY_to_upY(p.y());

	// current node positions (chart-local) and converted to up-y
	QPointF cur0 = m_chart->mapFromScene(m_nodes[0]->pos());
	QPointF cur1 = m_chart->mapFromScene(m_nodes[1]->pos());
	QPointF cur2 = m_chart->mapFromScene(m_nodes[2]->pos());
	QPointF cur3 = m_chart->mapFromScene(m_nodes[3]->pos());

	double cur0Up = chartY_to_upY(cur0.y());
	double cur1Up = chartY_to_upY(cur1.y());
	double cur2Up = chartY_to_upY(cur2.y());
	double cur3Up = chartY_to_upY(cur3.y());

	const double eps = 1.0; // separation tolerance in device units

	// Per-index simple (hard) constraints — do not perform cross-node pushing here.
	if (idx == 0) {
		// node1: x fixed to left edge, y within [0,chartHeight]
		p.setX(m_fixedX[0]);
		if (pUpY < 0.0) pUpY = 0.0;
		if (pUpY > chartHeight) pUpY = chartHeight;
	}
	else if (idx == 3) {
		// node4: x fixed to right edge, y within [0,chartHeight]
		p.setX(m_fixedX[3]);
		if (pUpY < 0.0) pUpY = 0.0;
		if (pUpY > chartHeight) pUpY = chartHeight;
	}
	else if (idx == 1) {
		// node2: x in [minX, maxX], y in [0,chartHeight], x must be < node3.x - eps
		if (p.x() < minX) p.setX(minX);
		if (p.x() > maxX) p.setX(maxX);
		if (pUpY < 0.0) pUpY = 0.0;
		if (pUpY > chartHeight) pUpY = chartHeight;
	}
	else if (idx == 2) {
		// node3: x in [minX, maxX], y in [0,chartHeight], x must be > node2.x + eps
		if (p.x() < minX) p.setX(minX);
		if (p.x() > maxX) p.setX(maxX);
		if (pUpY < 0.0) pUpY = 0.0;
		if (pUpY > chartHeight) pUpY = chartHeight;
	}

	// convert back to chart-local
	p.setY(upY_to_chartY(pUpY));
	return p;
}

void WindowLevelController::updateInteractiveLine()
{
	if (!m_interactiveInitialized || !m_chart) return;
	for (int i = 0; i < 4; ++i) if (!m_nodes[i]) return;
	m_interactiveUpdating = true;

	// read node positions in chart-local coordinates
	QPointF p0 = m_chart->mapFromScene(m_nodes[0]->pos());
	QPointF p1 = m_chart->mapFromScene(m_nodes[1]->pos());
	QPointF p2 = m_chart->mapFromScene(m_nodes[2]->pos());
	QPointF p3 = m_chart->mapFromScene(m_nodes[3]->pos());

	// enforce fixed X for node1 and node4
	p0.setX(m_fixedX[0]);
	p3.setX(m_fixedX[3]);

	// enforce always-true equalities from spec:
	// p2.y == p1.y  (node2.y == node1.y)
	p1.setY(p0.y());
	// p4.y == p3.y  (node4.y == node3.y)
	p3.setY(p2.y());

	// write back adjusted positions
	m_nodes[0]->setPos(m_chart->mapToScene(p0));
	m_nodes[1]->setPos(m_chart->mapToScene(p1));
	m_nodes[2]->setPos(m_chart->mapToScene(p2));
	m_nodes[3]->setPos(m_chart->mapToScene(p3));

	// update connector segments (chart-local coords)
	m_segLeft->setLine(p0.x(), p0.y(), p1.x(), p1.y());   // node1 -> node2
	m_segMid->setLine(p1.x(), p1.y(), p2.x(), p2.y());    // node2 -> node3
	m_segRight->setLine(p2.x(), p2.y(), p3.x(), p3.y());  // node3 -> node4

	// sync outline
	if (m_plotRect) {
		QRectF plotRect = m_chart->plotArea();
		if (!plotRect.isEmpty()) m_plotRect->setRect(plotRect);
	}

	// refresh last-known positions
	for (int i = 0; i < 4; ++i)
		m_lastNodePos[i] = m_chart->mapFromScene(m_nodes[i]->pos());

	m_interactiveUpdating = false;
}

void ensureInteractiveConnections(WindowLevelController* ctrl)
{
	if (!ctrl->chart() || !ctrl->chart()->scene()) return;

	QObject::connect(ctrl->chart()->scene(), &QGraphicsScene::changed, ctrl, [ctrl](const QList<QRectF>&) {
		if (!ctrl->m_interactiveInitialized) return;
		if (ctrl->m_interactiveUpdating) return;

		// read current chart-local centers
		QPointF curChart[4];
		for (int i = 0; i < 4; ++i) {
			if (!ctrl->m_nodes[i]) return;
			curChart[i] = ctrl->m_chart->mapFromScene(ctrl->m_nodes[i]->pos());
		}

		// helpers for y-up conversions
		QRectF plot = ctrl->m_chart->plotArea();
		const double chartTop = plot.top();
		const double chartBottom = plot.bottom();
		auto chartY_to_upY = [&](double yChart) -> double { return chartBottom - yChart; };
		auto upY_to_chartY = [&](double yUp) -> double { return chartBottom - yUp; };

		// detect active node (prefer mouseGrabberItem)
		int active = -1;
		if (QGraphicsScene* scene = ctrl->chart()->scene()) {
			QGraphicsItem* grabbed = scene->mouseGrabberItem();
			if (grabbed) {
				for (int i = 0; i < 4; ++i) {
					if (grabbed == ctrl->m_nodes[i]) { active = i; break; }
				}
			}
		}
		if (active == -1) {
			// fallback: detect which node moved compared to last-known
			const double tol = 0.5;
			for (int i = 0; i < 4; ++i) {
				QPointF last = ctrl->m_lastNodePos[i];
				if (std::hypot(curChart[i].x() - last.x(), curChart[i].y() - last.y()) > tol) {
					active = i;
					break;
				}
			}
		}

		// preserve previous active so we can detect drag end (for commit)
		int prevActive = ctrl->m_activeNode;
		ctrl->m_interactiveUpdating = true;
		ctrl->m_activeNode = active;

		// nothing changed: update last-known and return
		if (active < 0) {
			for (int i = 0; i < 4; ++i) ctrl->m_lastNodePos[i] = curChart[i];
			// if we just finished an interaction (prevActive >= 0 && now none), commit window/level
			if (prevActive >= 0) {
				// compute window/level from nodes 2 & 3 (data-space values)
				double v2 = ctrl->chartXToDataValue(ctrl->m_chart->mapFromScene(ctrl->m_nodes[1]->pos()).x());
				double v3 = ctrl->chartXToDataValue(ctrl->m_chart->mapFromScene(ctrl->m_nodes[2]->pos()).x());
				double dmin = std::min(v2, v3);
				double dmax = std::max(v2, v3);
				double W = dmax - dmin;
				double L = 0.5 * (dmin + dmax);
				// update spinboxes without emitting valueChanged
				if (ctrl->ui.m_spinWindow && ctrl->ui.m_spinLevel) {
					QSignalBlocker b1(ctrl->ui.m_spinWindow);
					QSignalBlocker b2(ctrl->ui.m_spinLevel);
					ctrl->ui.m_spinWindow->setValue(W);
					ctrl->ui.m_spinLevel->setValue(L);
				}
				// emit commit
				Q_EMIT ctrl->windowLevelCommitted(W, L);
			}
			ctrl->m_interactiveUpdating = false;
			return;
		}

		// prepare prev/current y-up values for direction detection
		double prevUp[4], curUp[4];
		for (int i = 0; i < 4; ++i) {
			prevUp[i] = chartY_to_upY(ctrl->m_lastNodePos[i].y());
			curUp[i] = chartY_to_upY(curChart[i].y());
		}

		double dx = curChart[active].x() - ctrl->m_lastNodePos[active].x();
		double dyUp = curUp[active] - prevUp[active]; // positive => moved up in user's y-up coords

		const double eps = 1.0;
		const double tol = 0.5;

		// helper to set a node position (constrained) and refresh curChart/curUp
		auto setChartPos = [&](int idx, const QPointF& desiredChart) {
			QPointF clamped = ctrl->constrainNodePosition(idx, desiredChart);
			ctrl->m_nodes[idx]->setPos(ctrl->m_chart->mapToScene(clamped));
			curChart[idx] = ctrl->m_chart->mapFromScene(ctrl->m_nodes[idx]->pos());
			curUp[idx] = chartY_to_upY(curChart[idx].y());
			};

		// useful bounds
		const double minX = plot.left();
		const double maxX = plot.right();

		// Implement the spec: active node dictates updates to others depending on movement direction
		const bool movedUp = (dyUp > tol);
		const bool movedDown = (dyUp < -tol);
		const bool movedLeft = (dx < -tol);
		const bool movedRight = (dx > tol);

		switch (active) {
			// ... other cases unchanged ...
			case 1: { // node2 active
				QPointF desired = ctrl->constrainNodePosition(1, curChart[1]);
				setChartPos(1, desired);

				if (movedUp) {
					setChartPos(0, QPointF(curChart[0].x(), desired.y()));
					if (chartY_to_upY(desired.y()) > curUp[2]) {
						setChartPos(2, QPointF(curChart[2].x(), desired.y()));
						setChartPos(3, QPointF(curChart[3].x(), desired.y()));
					}
				}
				else if (movedDown) {
					setChartPos(0, QPointF(curChart[0].x(), desired.y()));
				}

				// X movement: handle pushing to the right robustly
				if (movedRight) {
					// if p2 has crossed/passed p3 (or is too close), try to push p3 right
					if (curChart[1].x() >= (curChart[2].x() - eps)) {
						// desired new x for p3
						double targetP3x = std::min(maxX, curChart[1].x() + eps);
						// attempt to move p3 to target
						setChartPos(2, QPointF(targetP3x, curChart[2].y()));
						// after moving p3, ensure p2 is still strictly left of p3; if not, clamp p2 back

						if (curChart[1].x() >= curChart[2].x() - eps) {
							double clampedP2x = curChart[2].x() - eps;
							if (clampedP2x < minX) clampedP2x = minX;
							setChartPos(1, QPointF(clampedP2x, curChart[1].y()));
						}
					}
				}
				// movedLeft: constrain handled by constrainNodePosition
				break;
			}
			case 2: { // node3 active
				QPointF desired = ctrl->constrainNodePosition(2, curChart[2]);
				setChartPos(2, desired);

				// X movement left: if p3 moves left past/equal p2, push p2 left
				if (movedLeft) {
					if (curChart[2].x() <= (curChart[1].x() + eps)) {
						// desired new x for p2 so separation holds
						double targetP2x = std::max(minX, curChart[2].x() - eps);
						// attempt to set p2
						setChartPos(1, QPointF(targetP2x, curChart[1].y()));
						// after moving p2, ensure p3 still strictly right of p2; if not, nudge p3
						if (curChart[2].x() <= curChart[1].x() + eps) {
							double clampedP3x = curChart[1].x() + eps;
							if (clampedP3x > maxX) clampedP3x = maxX;
							setChartPos(2, QPointF(clampedP3x, curChart[2].y()));
						}
					}
				}

				// Y movement handled elsewhere (unchanged)
				if (movedUp) {
					setChartPos(3, QPointF(curChart[3].x(), desired.y()));
				}
				else if (movedDown) {
					setChartPos(3, QPointF(curChart[3].x(), desired.y()));
					if (curUp[1] > chartY_to_upY(desired.y())) {
						setChartPos(1, QPointF(curChart[1].x(), desired.y()));
						setChartPos(0, QPointF(curChart[0].x(), desired.y()));
					}
				}
				break;
			}
				  // ... other cases unchanged ...
			case 0: {
				// node1 behavior unchanged (kept for completeness)
				QPointF desired = ctrl->constrainNodePosition(0, curChart[0]);
				setChartPos(0, desired);
				setChartPos(1, QPointF(curChart[1].x(), desired.y()));
				if (movedUp) {
					if (chartY_to_upY(desired.y()) > curUp[2]) {
						setChartPos(2, QPointF(curChart[2].x(), desired.y()));
						setChartPos(3, QPointF(curChart[3].x(), desired.y()));
					}
				}
				break;
			}
			case 3: {
				// node4 behavior unchanged
				QPointF desired = ctrl->constrainNodePosition(3, curChart[3]);
				setChartPos(3, desired);
				setChartPos(2, QPointF(curChart[2].x(), desired.y()));
				if (movedDown) {
					if (curUp[1] > chartY_to_upY(desired.y())) {
						setChartPos(1, QPointF(curChart[1].x(), desired.y()));
						setChartPos(0, QPointF(curChart[0].x(), desired.y()));
					}
				}
				break;
			}
			default:
			break;
		}

		// Enforce invariant equalities and refresh geometry
		ctrl->updateInteractiveLine();

		// compute and publish window/level (data-space) from nodes 2 & 3
		if (ctrl->ui.m_spinWindow && ctrl->ui.m_spinLevel && ctrl->m_axisX) {
			double chartX2 = ctrl->m_chart->mapFromScene(ctrl->m_nodes[1]->pos()).x();
			double chartX3 = ctrl->m_chart->mapFromScene(ctrl->m_nodes[2]->pos()).x();
			double v2 = ctrl->chartXToDataValue(chartX2);
			double v3 = ctrl->chartXToDataValue(chartX3);
			double dmin = std::min(v2, v3);
			double dmax = std::max(v2, v3);
			double W = dmax - dmin;
			double L = 0.5 * (dmin + dmax);
			// update spinboxes without firing their change signals
			{
				QSignalBlocker b1(ctrl->ui.m_spinWindow);
				QSignalBlocker b2(ctrl->ui.m_spinLevel);
				ctrl->ui.m_spinWindow->setValue(W);
				ctrl->ui.m_spinLevel->setValue(L);
			}
			// start debounce to emit interactive change (same behavior as spinbox changes)
			if (ctrl->m_debounce) ctrl->m_debounce->start();
		}

		// Save last-known positions
		for (int i = 0; i < 4; ++i)
			ctrl->m_lastNodePos[i] = ctrl->m_chart->mapFromScene(ctrl->m_nodes[i]->pos());

		ctrl->m_interactiveUpdating = false;
	});
}

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
		QTimer::singleShot(0, this, [this]() {
			// ensure the chart plot area is sized to the view
			adjustChartPlotArea();
			// initialize interactive overlay now that the plot area has been set
			initInteractiveLine();
			// connect scene watcher (safe if chart/scene not yet available; helper will early-return)
			ensureInteractiveConnections(this);
		});

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
	// reflect into node positions immediately
	applyWindowLevelToNodes(ui.m_spinWindow->value(), ui.m_spinLevel->value());
}

void WindowLevelController::setLevel(double l)
{
	if (!ui.m_spinLevel) return;
	QSignalBlocker b(ui.m_spinLevel);
	ui.m_spinLevel->setValue(l);
	// reflect into node positions immediately
	applyWindowLevelToNodes(ui.m_spinWindow->value(), ui.m_spinLevel->value());
}

// Helper: convert chart-local X (plot coordinates) -> data value (axis units)
double WindowLevelController::chartXToDataValue(double chartX) const
{
	if (!m_chart || !m_axisX) return chartX;
	QRectF plot = m_chart->plotArea();
	const double plotLeft = plot.left();
	const double plotW = (plot.width() > 0.0) ? plot.width() : 1.0;
	const double axisMin = m_axisX->min();
	const double axisMax = m_axisX->max();
	const double t = (chartX - plotLeft) / plotW;
	return axisMin + t * (axisMax - axisMin);
}

// Helper inverse: data value (axis units) -> chart-local X
double WindowLevelController::dataValueToChartX(double dataVal) const
{
	if (!m_chart || !m_axisX) return dataVal;
	QRectF plot = m_chart->plotArea();
	const double plotLeft = plot.left();
	const double plotW = (plot.width() > 0.0) ? plot.width() : 1.0;
	const double axisMin = m_axisX->min();
	const double axisMax = m_axisX->max();
	const double t = (axisMax == axisMin) ? 0.0 : ((dataVal - axisMin) / (axisMax - axisMin));
	return plotLeft + t * plotW;
}

// Move nodes 2 & 3 to match given window/level (window = width, level = center)
void WindowLevelController::applyWindowLevelToNodes(double window, double level)
{
	if (!m_chart || !m_nodes[1] || !m_nodes[2] || !m_axisX) return;

	// compute data-space min/max
	const double half = 0.5 * window;
	const double dataMin = level - half;
	const double dataMax = level + half;

	// map to chart-local X
	const double x2 = dataValueToChartX(dataMin);
	const double x3 = dataValueToChartX(dataMax);

	// preserve current Y for nodes 2/3 (chart-local)
	QPointF cur1 = m_chart->mapFromScene(m_nodes[1]->pos());
	QPointF cur2 = m_chart->mapFromScene(m_nodes[2]->pos());

	// set positions (use scene coordinates). Use constrainNodePosition to clamp.
	QPointF new1 = constrainNodePosition(1, QPointF(x2, cur1.y()));
	QPointF new2 = constrainNodePosition(2, QPointF(x3, cur2.y()));

	m_nodes[1]->setPos(m_chart->mapToScene(new1));
	m_nodes[2]->setPos(m_chart->mapToScene(new2));

	// Ensure geometry & equality constraints are enforced
	updateInteractiveLine();
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
			QTimer::singleShot(0, this, [this]() {
				adjustChartPlotArea();
				if (!m_interactiveInitialized) {
					initInteractiveLine();
					ensureInteractiveConnections(this);
				}
			});
		}
	}
	// let base class handle other processing
	return QObject::eventFilter(watched, event);
}

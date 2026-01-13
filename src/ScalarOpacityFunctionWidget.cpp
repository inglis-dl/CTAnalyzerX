#include "ScalarOpacityFunctionWidget.h"
#include "RangeSlider.h"

#include <QDebug>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QTimer>

#include <vtkPiecewiseFunction.h>

#include <algorithm>
#include <functional>
#include <limits>

// Private implementation (PIMPL)
class ScalarOpacityFunctionWidgetPrivate
{
	Q_DECLARE_PUBLIC(ScalarOpacityFunctionWidget)
protected:
	ScalarOpacityFunctionWidget* const q_ptr;

public:
	explicit ScalarOpacityFunctionWidgetPrivate(ScalarOpacityFunctionWidget& q)
		: q_ptr(&q)
		, m_scene(nullptr)
		, m_path(nullptr)
		, m_viewMin(0)
		, m_viewMax(255)
		, m_domainMin(-1.0)
		, m_domainMax(1.0)
		, m_master(nullptr)
	{
		for (int i = 0; i < 4; ++i) m_nodes[i] = nullptr;
	}

	~ScalarOpacityFunctionWidgetPrivate()
	{
		// Qt will delete scene children automatically with the scene, but ensure no dangling pointers
		if (m_scene) {
			for (int i = 0; i < 4; ++i) {
				if (m_nodes[i]) {
					m_scene->removeItem(m_nodes[i]);
					delete m_nodes[i];
					m_nodes[i] = nullptr;
				}
			}
			if (m_path) {
				m_scene->removeItem(m_path);
				delete m_path;
				m_path = nullptr;
			}
		}
	}

	// Scene and items (owned by Qt parent/scene)
	QGraphicsScene* m_scene;
	QGraphicsPathItem* m_path;
	QGraphicsEllipseItem* m_nodes[4];

	// cached domain for current function (function node bounds)
	double m_domainMin;
	double m_domainMax;

	// slider positions (either percent [0..100] or scalar units depending on slider range)
	// prefer double for domain math
	double m_viewMin;
	double m_viewMax;

	// Store master using vtkSmartPointer to be safer about lifetime.
	vtkSmartPointer<vtkPiecewiseFunction> m_master;

	// Helper inner NodeItem (unchanged)
	class NodeItem : public QGraphicsEllipseItem
	{
	public:
		NodeItem(int idx, QGraphicsItem* parent = nullptr)
			: QGraphicsEllipseItem(parent), m_index(idx), m_pressed(false)
		{
			const double r = 6.0;
			setRect(-r, -r, 2 * r, 2 * r);
			QPen nodePen(Qt::black);
			nodePen.setCosmetic(true); // keep 1px outline regardless of view transform/zoom
			nodePen.setWidthF(1.0);
			setPen(nodePen);
			// use a slightly contrasting fill so the outline is always visible on light/dark themes
			QWidget* widgetParent = nullptr;
			QGraphicsItem* p = parent;
			while (p) {
				// Try to get the widget from the scene/view chain
				if (auto view = dynamic_cast<QGraphicsView*>(p->scene() ? p->scene()->views().value(0, nullptr) : nullptr)) {
					widgetParent = view->parentWidget();
					break;
				}
				p = p->parentItem();
			}
			if (!widgetParent) {
				// fallback: use QApplication palette
				setBrush(QApplication::palette().color(QPalette::Window));
			}
			else {
				setBrush(widgetParent->palette().color(QPalette::Window));
			}
			// Keep nodes at constant pixel size when the view is zoomed via fitInView.
			setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
			setFlag(QGraphicsItem::ItemIsMovable, true);
			setFlag(QGraphicsItem::ItemSendsScenePositionChanges, true);
			setAcceptHoverEvents(true);
			setCursor(Qt::ArrowCursor);
			setZValue(1000.0);
		}

	protected:
		void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override
		{
			Q_UNUSED(option); Q_UNUSED(widget);
			painter->setRenderHint(QPainter::Antialiasing, true);
			painter->setPen(pen());
			painter->setBrush(m_pressed ? QColor(173, 216, 230) : Qt::white);
			painter->drawEllipse(rect());
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
			setZValue(1100.0);
			m_pressed = true;
			update();
			QGraphicsEllipseItem::mousePressEvent(ev);
		}

		void mouseReleaseEvent(QGraphicsSceneMouseEvent* ev) override
		{
			setZValue(1000.0);
			m_pressed = false;
			update();
			QGraphicsEllipseItem::mouseReleaseEvent(ev);
		}

		QVariant itemChange(GraphicsItemChange change, const QVariant& value) override
		{
			// no-op here; owner will query positions from scene items when needed and commit into the slave when appropriate
			return QGraphicsEllipseItem::itemChange(change, value);
		}

	public:
		int m_index;
		bool m_pressed;
	};

	void initializeGraphics()
	{
		Q_Q(ScalarOpacityFunctionWidget);

		// create scene and attach
		m_scene = new QGraphicsScene(q);

		// Ensure scene/view background matches widget palette (avoid default black background)
		m_scene->setBackgroundBrush(q->palette().color(QPalette::Base));

		// create path item
		m_path = new QGraphicsPathItem();
		// Use a contrasting pen color from the widget palette (works for light & dark themes)
		QPen pen = m_path->pen();
		pen.setColor(q->palette().color(QPalette::WindowText));
		pen.setWidthF(2.0);
		pen.setCosmetic(true);
		m_path->setPen(pen);
		m_path->setBrush(Qt::NoBrush);
		m_path->setZValue(500);
		m_scene->addItem(m_path);

		for (int i = 0; i < 4; ++i) {
			if (!m_nodes[i]) {
				m_nodes[i] = new NodeItem(i);
				// add to scene root (clipping is handled by the view)
				m_scene->addItem(m_nodes[i]);

				// Make sure outline is visible and cosmetic so it remains 1px on-screen.
				QPen outline(q_ptr->palette().color(QPalette::WindowText));
				outline.setCosmetic(true);
				outline.setWidthF(1.0);
				m_nodes[i]->setPen(outline);
				// Fill chosen to be Window so the node appears as a white circle with dark outline on light themes,
				// and still visible on dark themes because outline contrasts.
				m_nodes[i]->setBrush(q_ptr->palette().color(QPalette::Window));
			}
		}
	}

	// Copy master nodes into the provided slave (outer q->m_function). Clears existing points.
	void ScalarOpacityFunctionWidgetPrivate::copyMasterToSlave()
	{
		Q_Q(ScalarOpacityFunctionWidget);

		if (!m_master) return;

		// If master already provides 4+ nodes, just deep-copy it.
		const int nMaster = m_master->GetSize();
		if (nMaster >= 4) {
			q->m_function->DeepCopy(m_master);
			q->m_function->Modified();
			return;
		}

		// Require a valid domain to synthesize meaningful endpoints.
		if (!(m_domainMin < m_domainMax)) {
			return;
		}

		using Node = std::pair<double, double>;   // (x, y)
		std::vector<Node> nodes;
		nodes.reserve(std::max(0, nMaster));
		double lowVal = 1.0;
		double highVal = 0.0;
		for (int i = 0; i < nMaster; ++i) {
			double node[4] = { 0.0, 0.0, 0.0, 0.0 };
			m_master->GetNodeValue(i, node);
			nodes.emplace_back(std::make_pair(node[0], node[1]));
			lowVal = std::min(lowVal, node[1]);
			highVal = std::max(highVal, node[1]);
		}

		// Prepare 4 synthesized (x,y) pairs.
		std::vector<Node> out(4);
		out[0] = { m_domainMin, lowVal };
		out[1] = { m_domainMin, lowVal };
		out[2] = { m_domainMax, highVal };
		out[3] = { m_domainMax, highVal };

		if (nMaster <= 2) {
			// simple linear ramp between two nodes
			out[0] = { m_domainMin,  lowVal };
			out[1] = nodes[0];
			out[2] = nodes[1];
			out[3] = { m_domainMax,  highVal };
		}
		else if (nMaster == 3) {
			const Node& n0 = nodes[0];
			const Node& n1 = nodes[1];
			const Node& n2 = nodes[2];

			if (n0.second < n1.second && n2.second == n1.second) {
				// case 1: left plateau collapsed
				out[1] = n0;
				out[2] = n1;
				out[3] = n2;
			}
			else if (n0.second == n1.second && n1.second < n2.second) {
				// case 2: rigt plateau collapsed
				out[0] = n0;
				out[1] = n1;
				out[2] = n2;
			}
			else if (n0.second == n1.second && n1.second == n2.second) {
				// case 3: ramp collapsed
				double val = n0.second;
				out[0] = { m_domainMin, val };
				std::sort(nodes.begin(), nodes.end());
				out[1] = { nodes.front().first, val };
				out[2] = { nodes.back().first,  val };
				out[3] = { m_domainMax, val };
			}
			else {
				// fallback
				out[0] = { m_domainMin,  lowVal };
				out[1] = { n0.first, lowVal };
				out[2] = { n2.first, highVal };
				out[3] = { m_domainMax,  highVal };
			}
		}

		// Commit synthesized points into the slave function.
		q->m_function->RemoveAllPoints();
		for (int i = 0; i < 4; ++i) {
			q->m_function->AddPoint(out[i].first, out[i].second);
		}
		q->m_function->Modified();
	}

	// Map function domain -> scene coordinates (scene uses world coords equal to function domain in X and [0..1] in Y)
	// Uses the slave (q->m_function) for display.
	void layoutItems()
	{
		Q_Q(ScalarOpacityFunctionWidget);

		if (!q->m_function) return;

		// query function nodes (we expect max 4)
		const int n = q->m_function->GetSize();

		// Build simple 3-segment QPainterPath between the 4 points (no interpolation)
		QPainterPath path;
		QPointF pts[4];
		for (int i = 0; i < n; ++i) {

			double node[4] = { 0.0, 0.0, 0.0, 0.0 };
			q->m_function->GetNodeValue(i, node);

			// scene Y: invert opacity so 1.0 -> top (0.0). We'll map yScene = 1 - opacity
			double sx = node[0];
			double sy = 1.0 - node[1];

			pts[i] = QPointF(sx, sy);

			//if (sx < m_domainMin) qDebug() << "Warning: function node " << i << " x value " << sx << " below domain min " << m_domainMin;
			//if (sx > m_domainMax) qDebug() << "Warning: function node " << i << " x value " << sx << " above domain max " << m_domainMax;

			m_nodes[i]->setPos(pts[i]);

			if (i == 0) path.moveTo(pts[i]);
			else path.lineTo(pts[i]);
		}
		if (m_path) {
			// update only the path geometry (avoid re-parenting / heavy scene ops)
			m_path->setPath(path);
		}

		// keep view bounds in double, clamp to domain
		if (m_viewMin < m_domainMin) m_viewMin = int(std::ceil(m_domainMin));
		if (m_viewMax > m_domainMax) m_viewMax = int(std::floor(m_domainMax));

		// compute visible rectangle from slider (slider values are integer but stored as doubles)
		double vminX = double(m_viewMin);
		double vmaxX = double(m_viewMax);
		if (vmaxX <= vminX) vmaxX = vminX + (m_domainMax - m_domainMin) * 0.001; // avoid zero width

		QRectF visibleRect(vminX, 0.0, vmaxX - vminX, 1.0);

		// Arrange view to show visibleRect: use fitInView to map visible world -> view area.
		// Use IgnoreAspectRatio so Y maps to full height.
		QGraphicsView* v = q->ui.m_view;
		if (v) {
			v->fitInView(visibleRect, Qt::IgnoreAspectRatio);
			// Force a repaint so the view doesn't leave a 1-pixel artifact when zoom is extreme.
			if (v->viewport()) v->viewport()->update();
		}
	}

}; // end PIMPL

// ---------- ScalarOpacityFunctionWidget implementation ----------

ScalarOpacityFunctionWidget::ScalarOpacityFunctionWidget(QWidget* parent)
	: QWidget(parent)
	, d_ptr(new ScalarOpacityFunctionWidgetPrivate(*this))
{
	Q_D(ScalarOpacityFunctionWidget);
	ui.setupUi(this);

	d->initializeGraphics();

	ui.m_view->setBackgroundBrush(d->m_scene->backgroundBrush());

	ui.m_view->setScene(d->m_scene);
	ui.m_view->setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
	ui.m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	ui.m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

	m_function = vtkSmartPointer<vtkPiecewiseFunction>::New();
	m_function->AllowDuplicateScalarsOn();

	// create a default linear function with 4 nodes
	// Window/level boundaries when scalar range is unsignd char 0 - 255
	const double window = 255.0;
	const double half = window * 0.5;
	const double lower = std::max(0.0, half - std::fabs(half));
	const double upper = std::min(255.0, half + std::fabs(half));

	// Opacity values based on window sign
	const double lowVal = (window < 0.0) ? 1.0 : 0.0;
	const double highVal = 1.0 - lowVal;

	d->m_domainMin = 0;
	d->m_domainMax = 255;

	QRectF worldRect(d->m_domainMin, 0.0, d->m_domainMax - d->m_domainMin, 1.0);
	d->m_scene->setSceneRect(worldRect);

	// edge order: min -> lower -> upper -> max
	m_function->RemoveAllPoints();
	m_function->AddPoint(d->m_domainMin, lowVal);
	m_function->AddPoint(lower, lowVal);
	m_function->AddPoint(upper, highVal);
	m_function->AddPoint(d->m_domainMax, highVal);

	ui.m_slider->setRange(d->m_domainMin, d->m_domainMax);
	ui.m_slider->setValues(d->m_domainMin, d->m_domainMax);
	connect(ui.m_slider, &RangeSlider::valuesChanged, this, [this](int minv, int maxv) {
		Q_D(ScalarOpacityFunctionWidget);
		int smin = ui.m_slider->minimum();
		int smax = ui.m_slider->maximum();
		d->m_viewMin = std::clamp(minv, smin, smax);
		d->m_viewMax = std::clamp(maxv, smin, smax);
		// defer update to avoid re-entrancy while dragging
		QTimer::singleShot(0, this, SLOT(updateFunction()));
	});

	// Defer initial layout once widget has size
	QTimer::singleShot(0, this, SLOT(updateFunction()));
}

ScalarOpacityFunctionWidget::~ScalarOpacityFunctionWidget()
{
	// PIMPL destructor will clean scene items
}

void ScalarOpacityFunctionWidget::setFunction(vtkPiecewiseFunction* func)
{
	Q_D(ScalarOpacityFunctionWidget);

	// store raw master pointer (not owned)
	d->m_master = func;
	d->copyMasterToSlave();
	d->layoutItems();
}

void ScalarOpacityFunctionWidget::updateFunction()
{
	Q_D(ScalarOpacityFunctionWidget);

	d->copyMasterToSlave();
	d->layoutItems();
}

void ScalarOpacityFunctionWidget::setSceneXRange(double xmin, double xmax)
{
	Q_D(ScalarOpacityFunctionWidget);
	if (!d->m_scene) return;

	// Ensure proper ordering and positive width.
	double x0 = xmin;
	double x1 = xmax;
	if (x1 < x0) {
		std::swap(x0, x1);
	}

	d->m_domainMin = x0;
	d->m_domainMax = x1;

	// prepare scene full-world rect: X in [domainMin,domainMax], Y in [0,1] where 0=top,1=bottom
	QRectF worldRect(d->m_domainMin, 0.0, d->m_domainMax - d->m_domainMin, 1.0);
	d->m_scene->setSceneRect(worldRect);

	// Also synchronize slider range to the scene/domain when slider exists.

	QSignalBlocker b(ui.m_slider);
	// Slider works with integer positions; map domain to integer bounds conservatively.
	const int smin = static_cast<int>(std::ceil(x0));
	const int smax = static_cast<int>(std::floor(x1));
	// Only change if sensible
	if (smin < smax) {
		ui.m_slider->setRange(smin, smax);
		ui.m_slider->setValues(smin, smax);
		d->m_viewMin = ui.m_slider->minimum();
		d->m_viewMax = ui.m_slider->maximum();
	}

	updateFunction();
}

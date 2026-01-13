#include "ScalarOpacityFunctionWidget.h"
#include "RangeSlider.h"

#include <QActionGroup>
#include <QDebug>
#include <QGraphicsEllipseItem>
#include <QGraphicsView>
#include <QMenu>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarSet>
#include <QtCharts/QValueAxis>
#include <QtCharts/QLineSeries>

using namespace QtCharts;

#include <vtkIdTypeArray.h>
#include <vtkImageData.h>
#include <vtkImageHistogram.h>
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
		, m_plotRect(nullptr)
		, m_viewMin(0)
		, m_viewMax(255)
		, m_domainMin(-1.0)
		, m_domainMax(1.0)
		, m_master(nullptr)
		, m_histo(nullptr)
		, m_chart(nullptr)
		, m_chartView(nullptr)
		, m_barSeries(nullptr)
		, m_barSet(nullptr)
		, m_axisX(nullptr)
		, m_axisY(nullptr)
		, m_barAxisX(nullptr)
		, m_barAxisY(nullptr)
		, m_mapSeries(nullptr)
		, m_filterPeak(false)
	{
		for (int i = 0; i < 4; ++i) m_nodes[i] = nullptr;
	}

	~ScalarOpacityFunctionWidgetPrivate()
	{
		// histogram visual items (if any) may have been created directly in a scene previously;
		// ensure they are removed if present.
		for (auto* bar : m_histBars) {
			if (bar) {
				if (bar->scene()) bar->scene()->removeItem(bar);
				delete bar;
			}
		}
		m_histBars.clear();

		// Nodes are parented to the chart and will be deleted by Qt parent-child cleanup.
	}

	// Chart scene clipping rect (child of QChart)
	QGraphicsRectItem* m_plotRect = nullptr;

	// Node handles created as children of the chart (chart's scene).
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

	// Histogram data / VTK helper
	vtkSmartPointer<vtkImageHistogram> m_histo;

	// Chart pieces (QtCharts)
	QChart* m_chart;
	QChartView* m_chartView;
	QBarSeries* m_barSeries;
	QBarSet* m_barSet;

	// Axis used for mapping the overlay (scalar domain units). These are the axes used by m_mapSeries.
	QValueAxis* m_axisX;
	QValueAxis* m_axisY;

	// Separate axes used by the bar (histogram) rendering. Keep them distinct so bar-series
	// ranges (0..displayBins) do not interfere with overlay mapping.
	QValueAxis* m_barAxisX;
	QValueAxis* m_barAxisY;

	// Hidden (or visible) XY series used to render the overlay and for coordinate mapping.
	QLineSeries* m_mapSeries;

	// Histogram visual items (when drawing directly into scene)
	QVector<QGraphicsRectItem*> m_histBars;

	// Context menu + actions for histogram controls
	QMenu* m_viewMenu = nullptr;
	QActionGroup* m_viewMenuGroup = nullptr;
	QAction* m_actLinear = nullptr;
	QAction* m_actLog = nullptr;
	QAction* m_actSqrt = nullptr;
	QAction* m_actFilterPeak = nullptr;

	// Optional filter peak
	bool m_filterPeak;

	// Helper inner NodeItem (unchanged appearance/behavior)
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
		// keep this minimal: node creation is deferred until the chart + plot area exist.
		// (initializeChart will create m_plotRect and m_nodes as children of the chart)
	}

	// Build QLineSeries points from the internal slave function (q->m_function).
	void updateOverlaySeries()
	{
		Q_Q(ScalarOpacityFunctionWidget);
		if (!m_mapSeries || !q->m_function) return;

		const int n = q->m_function->GetSize();
		QVector<QPointF> pts;
		pts.reserve(std::max(0, n));

		double axisYmin = m_axisY ? m_axisY->min() : 0.0;
		double axisYmax = m_axisY ? m_axisY->max() : 1.0;

		for (int i = 0; i < n; ++i) {
			double node[4] = { 0.0, 0.0, 0.0, 0.0 };
			q->m_function->GetNodeValue(i, node);
			double x = node[0];
			double y = node[1];
			if (std::isnan(y)) y = 0.0;
			y = std::min(1.0, std::max(0.0, y));
			double yWorld = axisYmin + y * (axisYmax - axisYmin);
			pts.append(QPointF(qreal(x), qreal(yWorld)));
		}

		// Replace series data in one operation
		m_mapSeries->replace(pts);
	}

	// Copy master nodes into the provided slave (outer q->m_function). Clears existing points.
	void copyMasterToSlave()
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

	// Map function domain -> parent-local coordinates (parent is chart).
	// Uses the slave (q->m_function) for display and positions QGraphics node handles.
	void layoutItems()
	{
		Q_Q(ScalarOpacityFunctionWidget);

		if (!q->m_function) return;
		if (!m_chart) return;

		// Pixel plot rectangle must exist (used for positioning/optional clipping)
		QRectF plot = m_chart->plotArea();
		if (plot.isEmpty()) return;

		// Use the visible axis ranges (chart's axis reflects range slider / mapping axis)
		double axisXmin = m_axisX ? m_axisX->min() : m_domainMin;
		double axisXmax = m_axisX ? m_axisX->max() : m_domainMax;
		double axisYmin = m_axisY ? m_axisY->min() : 0.0;
		double axisYmax = m_axisY ? m_axisY->max() : 1.0;

		const int n = q->m_function->GetSize();
		if (n <= 0) {
			if (m_mapSeries) m_mapSeries->replace(QVector<QPointF>()); // clear overlay
			return;
		}

		// Ensure overlay series contains current polyline in axis units
		updateOverlaySeries();

		// Position node handles in pixel coords using the mapping series
		for (int i = 0; i < 4; ++i) {
			if (!m_nodes[i]) continue;

			if (i >= n) {
				m_nodes[i]->setVisible(false);
				continue;
			}

			double node[4] = { 0.0, 0.0, 0.0, 0.0 };
			q->m_function->GetNodeValue(i, node);
			double xWorld = node[0];
			double yVal = node[1];
			if (std::isnan(yVal)) yVal = 0.0;
			yVal = std::min(1.0, std::max(0.0, yVal));
			double yWorld = axisYmin + yVal * (axisYmax - axisYmin);

			QPointF mapped = m_chart->mapToPosition(QPointF(xWorld, yWorld), m_mapSeries);

			m_nodes[i]->setParentItem(m_chart);
			m_nodes[i]->setPos(mapped);
			m_nodes[i]->setVisible(true);
			m_nodes[i]->setTransform(QTransform()); // clear transformations so position is literal
		}
	}

	void createPlotChildren()
	{
		Q_Q(ScalarOpacityFunctionWidget);

		// recompute plot area in case layout changed
		this->adjustChartPlotArea();
		QRectF plot = this->m_chart->plotArea();
		if (plot.isEmpty())
			return;

		// keep a plot rect for clipping/backdrop if desired
		if (!m_plotRect) {
			m_plotRect = new QGraphicsRectItem(plot, m_chart);
			m_plotRect->setPen(QPen(Qt::black, 1));
			m_plotRect->setBrush(Qt::NoBrush);
			m_plotRect->setZValue(900.0);
			// This ensures children clipped to the plot rect if you prefer explicit clipping.
			m_plotRect->setFlag(QGraphicsItem::ItemClipsChildrenToShape, true);
		}
		else {
			m_plotRect->setRect(plot);
			m_plotRect->setParentItem(m_chart);
			m_plotRect->setZValue(900.0);
		}

		const double nodeRadius = 6.0;
		for (int i = 0; i < 4; ++i) {
			if (!m_nodes[i]) {
				// create nodes as children of the chart so they can participate in pixel positioning
				m_nodes[i] = new NodeItem(i, m_chart);
				// ensure outline is cosmetic
				QPen nodePen(q_ptr->palette().color(QPalette::WindowText));
				nodePen.setCosmetic(true);
				nodePen.setWidthF(1.0);
				m_nodes[i]->setPen(nodePen);
				m_nodes[i]->setBrush(q_ptr->palette().color(QPalette::Window));
				m_nodes[i]->setZValue(1000.0 + ((i == 1 || i == 2) ? 2.0 : 0.0));
				// NodeItem ctor sets ItemIgnoresTransformations=true for constant pixel size — clear it
				m_nodes[i]->setFlag(QGraphicsItem::ItemIgnoresTransformations, false);
			}
			else {
				// ensure parented to chart
				m_nodes[i]->setParentItem(m_chart);
			}
		}

		// perform initial layout now that children exist
		q->updateFunction();
	}

	// Chart initialization
	void initializeChart()
	{
		Q_Q(ScalarOpacityFunctionWidget);

		if (m_chart) return;
		// bar set + series
		m_barSet = new QBarSet(QString());
		m_barSet->setColor(QColor("#404040"));
		m_barSet->setBorderColor(Qt::transparent);
		m_barSet->setBrush(QBrush(QColor(0x44, 0x44, 0x44)));

		m_barSeries = new QBarSeries();
		m_barSeries->append(m_barSet);
		m_barSeries->setBarWidth(1.0);
		m_barSeries->setLabelsVisible(false);

		m_chart = new QChart();
		m_chart->legend()->hide();
		m_chart->addSeries(m_barSeries);
		m_chart->setBackgroundRoundness(0);
		m_chart->setBackgroundVisible(false);
		m_chart->setMargins(QMargins(0, 0, 0, 0));
		m_chart->setPlotAreaBackgroundVisible(false);

		// Create mapping axes (used for overlay mapping in scalar units).
		m_axisX = new QValueAxis();
		m_axisY = new QValueAxis();
		m_axisX->setLabelsVisible(false);
		m_axisX->setGridLineVisible(false);
		m_axisX->setLineVisible(false);
		m_axisX->setTickCount(0);
		m_axisY->setLabelsVisible(false);
		m_axisY->setGridLineVisible(false);
		m_axisY->setLineVisible(false);
		m_axisY->setTickCount(0);

		// Create separate axes for bar/histogram rendering (bar series uses these).
		m_barAxisX = new QValueAxis();
		m_barAxisY = new QValueAxis();
		m_barAxisX->setLabelsVisible(false);
		m_barAxisX->setGridLineVisible(false);
		m_barAxisX->setLineVisible(false);
		m_barAxisX->setTickCount(0);
		m_barAxisY->setLabelsVisible(false);
		m_barAxisY->setGridLineVisible(false);
		m_barAxisY->setLineVisible(false);
		m_barAxisY->setTickCount(0);

		// Add mapping axes to chart and attach (these axes tell the map series how to map scalar coords)
		m_chart->addAxis(m_axisX, Qt::AlignBottom);
		m_chart->addAxis(m_axisY, Qt::AlignLeft);

		// Add bar axes to chart and attach them to the bar series
		m_chart->addAxis(m_barAxisX, Qt::AlignBottom);
		m_chart->addAxis(m_barAxisY, Qt::AlignLeft);
		m_barSeries->attachAxis(m_barAxisX);
		m_barSeries->attachAxis(m_barAxisY);

		// Create mapping series used to draw overlay and for coordinate mapping
		m_mapSeries = new QLineSeries();
		m_mapSeries->setName(QString());
		// show the series so chart draws the overlay
		m_mapSeries->setVisible(true);
		// hide point markers - we use QGraphicsEllipseItems for interactive nodes
		m_mapSeries->setPointsVisible(false);
		// style overlay
		QPen overlayPen(q_ptr->palette().color(QPalette::WindowText));
		overlayPen.setWidth(2);
		m_mapSeries->setPen(overlayPen);

		m_chart->addSeries(m_mapSeries);
		// Attach mapping series to the mapping axes
		m_mapSeries->attachAxis(m_axisX);
		m_mapSeries->attachAxis(m_axisY);

		// Ensure mapping axes have sensible defaults (scalar domain and y [0..1])
		m_axisX->setRange(m_domainMin, m_domainMax);
		m_axisY->setRange(0.0, 1.0);

		// Give bar axes sensible defaults as well (will be updated by redrawHistogram)
		m_barAxisX->setRange(0.0, 1.0);
		m_barAxisY->setRange(0.0, 1.0);

		m_chartView = new QChartView(m_chart);
		m_chartView->setRenderHint(QPainter::Antialiasing);
		m_chartView->setContentsMargins(0, 0, 0, 0);
		m_chartView->setStyleSheet("background: transparent; border: none; padding: 0px; margin: 0px;");

		// ensure the chart view expands to fill the container/layout
		m_chartView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

		// If the UI has a container widget `m_view`, parent the chart into its viewport (if QGraphicsView)
		// or directly into the container widget. Use a consistent, simple parenting strategy so the chart
		// always becomes a child of the UI placeholder (or its viewport) and is visible.
		QWidget* container = q->ui.m_view; // may be QGraphicsView or QWidget
		if (container) {
			QWidget* parentForChart = nullptr;
			if (auto gview = qobject_cast<QGraphicsView*>(container))
				parentForChart = gview->viewport();
			else
				parentForChart = container;

			// Ensure the parent has a layout. If none exists, create one so Qt layouts manage the chartView sizing.
			QLayout* parentLayout = parentForChart->layout();
			if (!parentLayout) {
				qDebug() << "[ScalarOpacity] initializeChart: no layout on parentForChart - creating QVBoxLayout";
				auto layout = new QVBoxLayout(parentForChart);
				layout->setContentsMargins(0, 0, 0, 0);
				layout->setSpacing(0);
				layout->setSizeConstraint(QLayout::SetNoConstraint);
				layout->addWidget(m_chartView, 1);
				// setLayout attaches the layout to the widget; from now on the layout will manage m_chartView geometry
				parentForChart->setLayout(layout);
				parentLayout = layout;
			}
			else {
				parentLayout->setContentsMargins(0, 0, 0, 0);
				parentLayout->setSpacing(0);
				if (auto box = qobject_cast<QBoxLayout*>(parentLayout))
					box->addWidget(m_chartView, 1);
				else
					parentLayout->addWidget(m_chartView);
			}

			// Ensure widget/layout update so viewport geometry becomes available immediately
			m_chartView->setParent(parentForChart);
			m_chartView->show();
			parentForChart->update();
			parentForChart->layout()->activate();
			parentForChart->installEventFilter(q);
		}

		// Ensure the chart's pixel plot area matches the chartView viewport now
		adjustChartPlotArea();

		// Helper that creates the clipping rect and nodes when a valid plot area exists.
		// Create children now if plot area is valid, otherwise defer to allow layouts to run.
		QRectF initialPlot = m_chart->plotArea();
		if (initialPlot.isEmpty()) {
			// schedule using the outer widget as context; bind the PIMPL member function
			// so we don't capture the PIMPL pointer in a lambda.
			QTimer::singleShot(0, q, std::bind(&ScalarOpacityFunctionWidgetPrivate::createPlotChildren, this));
		}
		else {
			createPlotChildren();
		}
	}

	// Recompute & update chart from the current VTK histogram input
	void redrawHistogram()
	{
		if (!m_histo || !m_chart || !m_barSet || !m_barAxisX || !m_barAxisY) return;

		m_histo->Update();
		vtkIdTypeArray* hArr = m_histo->GetHistogram();
		if (!hArr || hArr->GetNumberOfTuples() <= 0) return;

		const int nBins = hArr->GetNumberOfTuples();
		const int scaleMode = m_histo->GetHistogramImageScale();

		// compute scaled counts
		std::vector<double> scaled(nBins, 0.0);
		double maxScaled = 0.0;
		for (int i = 0; i < nBins; ++i) {
			vtkIdType raw = hArr->GetValue(i);
			double s = 0.0;
			if (scaleMode == vtkImageHistogram::Log) s = (raw > 0) ? std::log(static_cast<double>(raw)) : 0.0;
			else if (scaleMode == vtkImageHistogram::Sqrt) s = static_cast<double>(std::sqrt(static_cast<double>(raw)));
			else s = static_cast<double>(raw);
			scaled[i] = s;
			if (s > maxScaled) maxScaled = s;
		}
		if (maxScaled <= 0.0) maxScaled = 1.0;

		// downsample to display-friendly size
		const int maxDisplayBins = 2048;
		int displayBins = std::min(nBins, maxDisplayBins);
		std::vector<double> display(displayBins, 0.0);
		if (displayBins == nBins) display = scaled;
		else {
			double step = static_cast<double>(nBins) / static_cast<double>(displayBins);
			for (int b = 0; b < displayBins; ++b) {
				int start = static_cast<int>(std::floor(b * step));
				int end = static_cast<int>(std::floor((b + 1) * step));
				if (end <= start) end = start + 1;
				double peak = 0.0;
				for (int j = start; j < end && j < nBins; ++j) peak = std::max(peak, scaled[j]);
				display[b] = peak;
			}
		}

		// optional filter peak
		if (m_filterPeak && !display.empty()) {
			auto it = std::max_element(display.begin(), display.end());
			if (it != display.end()) *it = 0.0;
		}

		double dispMax = *std::max_element(display.begin(), display.end());
		if (dispMax <= 0.0) dispMax = 1.0;

		// update chart data (bar series)
		if (m_barSet->count() > 0) m_barSet->remove(0, m_barSet->count());
		QList<qreal> qvals;
		qvals.reserve(displayBins);
		for (double v : display) qvals.append(qreal(v));
		m_barSet->append(qvals);

		// --- Compute mapping from scalar domain (m_axisX) to histogram display index range ---
		// We attempt to obtain bin origin and bin width from vtkImageHistogram, then map the visible
		// axis range [axisMin, axisMax) to bin indices and map those to display indices.
		double axisMin = 0.0;
		double axisMax = 0.0;
		if (m_axisX) {
			axisMin = m_axisX->min();
			axisMax = m_axisX->max();
		}

		// Default origin/width; override if histogram provides them.
		double binOrigin = 0.0;
		double binWidth = 1.0;

		// These getters are available on vtkImageHistogram (bin origin / bin width).
		// If for some reason your VTK version differs, adapt accordingly.
		// Protect calls by checking m_histo is valid (already tested above).
		binOrigin = m_histo->GetBinOrigin();
		double histoMin = m_histo->GetBinOrigin();
		double histoMax = histoMin;
		if (nBins > 0) {
			// Try to get the scalar range from the image if available
			if (vtkImageData* img = vtkImageData::SafeDownCast(m_histo->GetInput())) {
				double range[2] = { 0, 0 };
				img->GetScalarRange(range);
				histoMax = range[1];
				histoMin = range[0];
			}
			else {
				// Fallback: estimate max from bins
				histoMax = histoMin + nBins;
			}
		}
		binWidth = (nBins > 0) ? (histoMax - histoMin) / nBins : 1.0;

		// Map axis scalar range -> original bin indices [binStart, binEnd)
		int binStart = static_cast<int>(std::floor((axisMin - binOrigin) / binWidth));
		int binEnd = static_cast<int>(std::ceil((axisMax - binOrigin) / binWidth));
		// clamp to histogram bins
		if (binStart < 0) binStart = 0;
		if (binStart > nBins) binStart = nBins;
		if (binEnd < 0) binEnd = 0;
		if (binEnd > nBins) binEnd = nBins;

		// Map original bin indices to display indices (account for downsampling)
		if (displayBins >= 1) {
			if (displayBins == nBins) {
				// no downsampling
				m_barAxisX->setRange(double(binStart), double(binEnd));
			}
			else {
				double step = static_cast<double>(nBins) / static_cast<double>(displayBins);
				int dStart = static_cast<int>(std::floor(binStart / step));
				int dEnd = static_cast<int>(std::ceil(binEnd / step));
				// clamp
				if (dStart < 0) dStart = 0;
				if (dStart > displayBins) dStart = displayBins;
				if (dEnd < 0) dEnd = 0;
				if (dEnd > displayBins) dEnd = displayBins;
				m_barAxisX->setRange(double(dStart), double(dEnd));
			}
		}
		else {
			// fallback: show all
			m_barAxisX->setRange(0.0, double(displayBins));
		}

		// Update bar Y range
		m_barAxisY->setRange(0.0, dispMax);
		m_barSeries->setBarWidth(1.0);

		// Ensure overlay series is drawn above histogram: remove and re-add it so it's last in chart series list
		if (m_mapSeries) {

			// removeSeries does not delete the series instance.
			m_chart->removeSeries(m_mapSeries);
			m_chart->addSeries(m_mapSeries);
			// reattach mapping axes (remove/add may detach attachments)
			m_mapSeries->attachAxis(m_axisX);
			m_mapSeries->attachAxis(m_axisY);
		}

		// after histogram changes, ensure overlay series reflects current function
		if (m_mapSeries) updateOverlaySeries();

		if (m_chartView) {
			m_chartView->update();
			m_chartView->repaint();
		}
	}

	// Recompute chart plot area to match the chart view viewport (pixel coordinates).
	// Keeps the chart's internal plot area in sync with the widget overlay.
	void adjustChartPlotArea()
	{
		if (!m_chart || !m_chartView)
			return;

		QWidget* vp = m_chartView->viewport();
		QSize vsz = vp ? vp->size() : m_chartView->size();
		if (vsz.isEmpty())
			return;

		// Use pixel coordinates for plot area so bars/axis fill the full viewport.
		m_chart->setPlotArea(QRectF(0.0, 0.0, qreal(vsz.width()), qreal(vsz.height())));

		// If plot rect exists, update it to match the new plot area.
		QRectF plot = m_chart->plotArea();
		if (m_plotRect) {
			m_plotRect->setRect(plot);
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

	// initialize placeholder graphics (actual nodes created when chart exists)
	d->initializeGraphics();

	// Note: we no longer create and attach a dedicated QGraphicsScene to ui.m_view.
	// Chart overlay (QChartView) will be parented into ui.m_view or its viewport.
	// configure the display function container and slider
	m_function = vtkSmartPointer<vtkPiecewiseFunction>::New();
	m_function->AllowDuplicateScalarsOn();

	// default scene/domain values
	d->m_domainMin = 0.0;
	d->m_domainMax = 255.0;
	d->m_viewMin = d->m_domainMin;
	d->m_viewMax = d->m_domainMax;
	ui.m_slider->setRange(int(d->m_domainMin), int(d->m_domainMax));
	ui.m_slider->setValues(int(d->m_domainMin), int(d->m_domainMax));

	// Ensure the internal (slave) function has sensible default points so
	// the widget displays nodes/curve even if no master function is attached.
	// This matches the synthesized 4-point layout used when a master is present.
	m_function->RemoveAllPoints();
	m_function->AddPoint(d->m_domainMin, 0.0);
	m_function->AddPoint(d->m_domainMin, 0.0);
	m_function->AddPoint(d->m_domainMax, 1.0);
	m_function->AddPoint(d->m_domainMax, 1.0);

	// Defer initial layout once widget has size
	QTimer::singleShot(0, this, SLOT(updateFunction()));

	// prepare histogram helper and chart view
	d->m_histo = vtkSmartPointer<vtkImageHistogram>::New();
	d->m_histo->AutomaticBinningOn();
	d->m_histo->GenerateHistogramImageOff();
	d->m_histo->SetHistogramImageScale(vtkImageHistogram::Linear);
	d->initializeChart();

	// setup persistent context menu and wire actions to widget slots
	d->m_viewMenu = new QMenu(this);
	d->m_viewMenuGroup = new QActionGroup(d->m_viewMenu);
	d->m_viewMenuGroup->setExclusive(true);

	// create scale actions and add to group
	d->m_actLinear = d->m_viewMenu->addAction(tr("Linear"));
	d->m_actLinear->setCheckable(true);
	d->m_actLinear->setData(vtkImageHistogram::Linear);
	d->m_viewMenuGroup->addAction(d->m_actLinear);

	d->m_actLog = d->m_viewMenu->addAction(tr("Log"));
	d->m_actLog->setCheckable(true);
	d->m_actLog->setData(vtkImageHistogram::Log);
	d->m_viewMenuGroup->addAction(d->m_actLog);

	d->m_actSqrt = d->m_viewMenu->addAction(tr("Sqrt"));
	d->m_actSqrt->setCheckable(true);
	d->m_actSqrt->setData(vtkImageHistogram::Sqrt);
	d->m_viewMenuGroup->addAction(d->m_actSqrt);

	// filter peak action
	d->m_viewMenu->addSeparator();
	d->m_actFilterPeak = d->m_viewMenu->addAction(tr("Filter peak"));
	d->m_actFilterPeak->setCheckable(true);
	d->m_actFilterPeak->setChecked(d->m_filterPeak);

	// connect group -> change scale (delegates to widget public API)
	connect(d->m_viewMenuGroup, &QActionGroup::triggered, this, [this](QAction* act) {
		if (!act) return;
		bool ok = act->data().isValid();
		if (!ok) return;
		int s = act->data().toInt();
		this->setHistogramScale(s); // will call d->redrawHistogram()
	});

	// connect filter toggle
	connect(d->m_actFilterPeak, &QAction::toggled, this, [this](bool checked) {
		this->setFilterPeak(checked); // will call d->redrawHistogram()
	});

	// Show menu on right click, ensure checks reflect current state

	ui.m_view->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(ui.m_view, &QWidget::customContextMenuRequested, this, [this, d](const QPoint& pt) {
		// sync checked state
		if (d->m_histo) {
			int cur = d->m_histo->GetHistogramImageScale();
			switch (cur) {
				case vtkImageHistogram::Log: d->m_actLog->setChecked(true); break;
				case vtkImageHistogram::Sqrt: d->m_actSqrt->setChecked(true); break;
				default: d->m_actLinear->setChecked(true); break;
			}
		}
		d->m_actFilterPeak->setChecked(d->m_filterPeak);

		// exec menu
		if (d->m_viewMenu) {
			d->m_viewMenu->exec(ui.m_view->mapToGlobal(pt));
		}
	});

	// wire slider to axis zoom (if chart present) - slider controls the overlay/mapping X axis
	if (ui.m_slider && d->m_axisX) {
		connect(ui.m_slider, &RangeSlider::valuesChanged, this, [d](int minPos, int maxPos) {
			if (!d->m_axisX) return;
			// inclusive range -> set axis to [min, max+1)
			d->m_axisX->setRange(double(minPos), double(maxPos + 1));
		});
	}

	// keep the widget updated when the chart axes range changes (e.g. range slider)
	// Update both overlay (nodes/curve) and histogram when the mapping axis range changes.
	connect(d->m_axisX, &QValueAxis::rangeChanged, this, [this, d]() {
		this->updateFunction();
		// redrawHistogram lives in the private implementation; use d pointer to call it
		d->redrawHistogram();
	});
	// always 0 to 1
	//connect(d->m_axisY, &QValueAxis::rangeChanged, this, [this]() { this->updateFunction(); });
}

ScalarOpacityFunctionWidget::~ScalarOpacityFunctionWidget()
{
	// PIMPL destructor will clean scene items and chart children
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
	// Ensure proper ordering and positive width.
	double x0 = xmin;
	double x1 = xmax;
	if (x1 < x0) std::swap(x0, x1);

	d->m_domainMin = x0;
	d->m_domainMax = x1;

	// Update slider integer bounds conservatively (if it exists).
	QSignalBlocker b(ui.m_slider);
	const int smin = static_cast<int>(std::ceil(x0));
	const int smax = static_cast<int>(std::floor(x1));
	if (smin < smax) {
		ui.m_slider->setRange(smin, smax);
		ui.m_slider->setValues(smin, smax);
		d->m_viewMin = ui.m_slider->minimum();
		d->m_viewMax = ui.m_slider->maximum();
	}

	// Update the mapping axes so overlay mapping uses the image scalar domain.
	if (d->m_axisX) d->m_axisX->setRange(x0, x1);
	if (d->m_axisY) d->m_axisY->setRange(0.0, 1.0);

	// If the chart/plot rect exist, update the clipping rect and re-layout nodes
	if (d->m_chart) {
		d->adjustChartPlotArea();
		QRectF plot = d->m_chart->plotArea();
		if (!plot.isEmpty()) {
			if (!d->m_plotRect) {
				d->m_plotRect = new QGraphicsRectItem(plot, d->m_chart);
				d->m_plotRect->setFlag(QGraphicsItem::ItemClipsChildrenToShape, true);
				d->m_plotRect->setZValue(900.0);
			}
			else {
				d->m_plotRect->setRect(plot);
				d->m_plotRect->setParentItem(d->m_chart);
				d->m_plotRect->setZValue(900.0);
			}
		}
	}

	// Recompute visual layout from the current slave function and domain.
	updateFunction();
}

void ScalarOpacityFunctionWidget::setImageData(vtkImageData* image)
{
	Q_D(ScalarOpacityFunctionWidget);

	if (!d->m_histo) {
		d->m_histo = vtkSmartPointer<vtkImageHistogram>::New();
		d->m_histo->AutomaticBinningOn();
		d->m_histo->GenerateHistogramImageOff();
		d->m_histo->SetHistogramImageScale(vtkImageHistogram::Linear);
	}

	if (!image) {
		if (d->m_barSet) d->m_barSet->remove(0, d->m_barSet->count());
		if (d->m_barAxisX) d->m_barAxisX->setRange(0, 1);
		if (d->m_barAxisY) d->m_barAxisY->setRange(0, 1);
		// Keep mapping axis defaults
		if (d->m_axisX) d->m_axisX->setRange(d->m_domainMin, d->m_domainMax);
		if (d->m_axisY) d->m_axisY->setRange(0.0, 1.0);
		return;
	}

	// Configure histogram for the image
	d->m_histo->SetInputData(image);
	d->m_histo->AutomaticBinningOn();
	d->m_histo->GenerateHistogramImageOff();

	// Derive domain from the image scalar range and ensure the scene & slider follow the image domain.
	double range[2];
	image->GetScalarRange(range);
	// Only set scene range when scalar range is sensible
	if (range[0] < range[1]) {
		// Ensure widget-level setter is used, which synchronizes slider & scene and mapping axes
		setSceneXRange(range[0], range[1]);
		// Configure histogram bins/origin like WindowLevelController does so vtkImageHistogram
		// produces expected (non-empty) output. Use a reasonable integer bin count derived
		// from the scalar range width.
		int maxBins = std::max(1, static_cast<int>(std::ceil(range[1] - range[0])));
		d->m_histo->SetMaximumNumberOfBins(maxBins);
		d->m_histo->SetBinOrigin(range[0]);
	}

	// now redraw histogram (updates bar axes ranges and ensures overlay is on top)
	d->redrawHistogram();
}

int ScalarOpacityFunctionWidget::histogramScale() const
{
	Q_D(const ScalarOpacityFunctionWidget);
	return d->m_histo ? d->m_histo->GetHistogramImageScale() : vtkImageHistogram::Linear;
}

void ScalarOpacityFunctionWidget::setHistogramScale(int s)
{
	Q_D(ScalarOpacityFunctionWidget);
	if (!d->m_histo) return;
	d->m_histo->SetHistogramImageScale(s);
	d->redrawHistogram();
}

bool ScalarOpacityFunctionWidget::filterPeak() const
{
	Q_D(const ScalarOpacityFunctionWidget);
	return d->m_filterPeak;
}

void ScalarOpacityFunctionWidget::setFilterPeak(bool v)
{
	Q_D(ScalarOpacityFunctionWidget);
	if (d->m_filterPeak == v) return;
	d->m_filterPeak = v;
	d->redrawHistogram();
}

bool ScalarOpacityFunctionWidget::eventFilter(QObject* watched, QEvent* event)
{
	Q_D(ScalarOpacityFunctionWidget);
	// Keep chart view sized to the graphics view viewport or container.
	if (d->m_chartView && ui.m_view) {
		if (auto gview = qobject_cast<QGraphicsView*>(ui.m_view)) {
			// watched may be the graphics view viewport
			if (gview->viewport() && watched == gview->viewport()) {
				if (event->type() == QEvent::Resize) {
					QWidget* vp = gview->viewport();
					d->m_chartView->setGeometry(vp->rect());
					// update the chart's internal pixel plot area so bars fill the whole viewport
					d->adjustChartPlotArea();
					// if plotRect exists, update layout
					d->layoutItems();
					return false; // allow normal processing too
				}
			}
		}
		else {
			// container widget case
			if (watched == ui.m_view && event->type() == QEvent::Resize) {
				QWidget* container = ui.m_view;
				d->m_chartView->setGeometry(container->rect());
				d->adjustChartPlotArea();
				d->layoutItems();
				return false;
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}
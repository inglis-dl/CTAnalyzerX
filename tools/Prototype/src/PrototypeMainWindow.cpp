#include "PrototypeMainWindow.h"
#include "PrototypeHelpers.h"
#include "ui_MainWindow.h"

#include "VolumeView.h"
#include "SliceView.h"
#include "ImageLoader.h"
#include "JsonUtils.h"

// Add with the other VTK includes at the top
#include <vtkBillboardTextActor3D.h>
#include <vtkBoundingBox.h>
#include <vtkCamera.h>
#include <vtkColorTransferFunction.h>
#include <vtkDataArray.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkExtractVOI.h>
#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkNIFTIImageWriter.h>
#include <vtkPointData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkScalarBarActor.h>
#include <vtkSmartPointer.h>
#include <vtkTextProperty.h>

#include <QAction>
#include <QButtonGroup>
#include <QChart>
#include <QChartView>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineSeries>
#include <QMessageBox>
#include <QPushButton>
#include <QScatterSeries>
#include <QSet>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QThread>
#include <QValueAxis>
#include <QVBoxLayout>

#include <functional>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

QT_CHARTS_USE_NAMESPACE   // expands to: using namespace QtCharts; (Qt5 only)

namespace {

	// ---------------------------------------------------------------------------
	// IterationProgressDialog
	//
	// Non-modal dialog combining setup controls and iteration history.
	//
	// LEFT  : baseline stats, iterations spin, multiplier spin, live step label,
	//         Run, Reset, and Apply Selection buttons.
	// RIGHT : iteration history table (top) and threshold-vs-volume chart (bottom).
	//
	// The iteration history table accumulates one row per completed iteration.
	// Each row shows the iteration index, threshold, total region volume ×10³,
	// and a mutually exclusive checkbox so the user can nominate one iteration
	// for final application.
	//
	// Apply Selection re-runs segmentation at the chosen threshold and updates
	// the 3D view, letting the user lock in whichever iteration gave the best
	// result without re-entering the setup parameters.
	//
	// Callbacks supplied by the caller:
	//   setIterateCallback — given a threshold, segments and updates the 3D view.
	//   setResetCallback   — restores the 3D view to the baseline segmentation.
	// ---------------------------------------------------------------------------
	class IterationProgressDialog : public QDialog
	{
	public:
		using IterateFunc = std::function<
			std::vector<PrototypeHelpers::BoneIsland>(double threshold)
		>;
		using ResetFunc = std::function<void()>;

		explicit IterationProgressDialog(
			double baselineThreshold,
			double regionMean,
			double regionStdDev,
			double regionVolumeMm3,
			const double voxelSpacing[3],
			QWidget* parent = nullptr)
			: QDialog(parent)
			, m_baseThreshold(baselineThreshold)
			, m_baseStdDev(regionStdDev)
			, m_voxelVolMm3(voxelSpacing[0] * voxelSpacing[1] * voxelSpacing[2])
		{
			setWindowTitle(tr("Region Grow - Iteration"));
			setMinimumSize(1100, 640);

			// ── Left panel: setup ────────────────────────────────────────────
			auto* setupForm = new QFormLayout;
			setupForm->addRow(tr("Baseline threshold:"),
				new QLabel(QString::number(baselineThreshold, 'f', 2), this));
			setupForm->addRow(tr("Region mean:"),
				new QLabel(QString::number(regionMean, 'f', 2), this));
			setupForm->addRow(tr("Region std deviation:"),
				new QLabel(QString::number(regionStdDev, 'f', 2), this));
			setupForm->addRow(tr("Region volume (mm\u00B3):"),
				new QLabel(QString::number(regionVolumeMm3, 'f', 1), this));

			m_spinIterations = new QSpinBox(this);
			m_spinIterations->setRange(1, 100);
			m_spinIterations->setValue(10);
			setupForm->addRow(tr("Iterations:"), m_spinIterations);

			m_spinMultiplier = new QDoubleSpinBox(this);
			m_spinMultiplier->setRange(0.01, 10.0);
			m_spinMultiplier->setSingleStep(0.1);
			m_spinMultiplier->setDecimals(3);
			m_spinMultiplier->setValue(0.1);
			setupForm->addRow(tr("Std-dev multiplier:"), m_spinMultiplier);

			// Read-only label: multiplier × stdDev updated live as the spin changes.
			m_labelStepSize = new QLabel(this);
			m_labelStepSize->setTextInteractionFlags(Qt::NoTextInteraction);
			updateStepSizeLabel(m_spinMultiplier->value());
			setupForm->addRow(tr("Threshold step:"), m_labelStepSize);

			connect(m_spinMultiplier,
				QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this,
				[this](double v) { updateStepSizeLabel(v); });

			auto* setupBox = new QGroupBox(tr("Setup"), this);
			setupBox->setLayout(setupForm);

			m_btnRun = new QPushButton(tr("Run"), this);
			connect(m_btnRun, &QPushButton::clicked, this,
				[this] { runIterations(); });

			m_btnReset = new QPushButton(tr("Reset"), this);
			connect(m_btnReset, &QPushButton::clicked, this,
				[this] { resetState(); });

			// Apply Selection is enabled only when a row checkbox is checked.
			m_btnApply = new QPushButton(tr("Apply Selection"), this);
			m_btnApply->setEnabled(false);
			connect(m_btnApply, &QPushButton::clicked, this,
				[this] { applySelection(); });

			auto* leftLayout = new QVBoxLayout;
			leftLayout->addWidget(setupBox);
			leftLayout->addSpacing(8);
			leftLayout->addWidget(m_btnRun);
			leftLayout->addWidget(m_btnReset);
			leftLayout->addWidget(m_btnApply);
			leftLayout->addStretch();

			// ── Right panel: iteration history table (top) ───────────────────
			// One row per completed iteration; the Select column is mutually
			// exclusive so exactly one iteration can be nominated for Apply.
			m_iterationTable = new QTableWidget(0, 5, this);
			m_iterationTable->setHorizontalHeaderLabels(
				{ tr("#"), tr("Threshold"), tr("Volume (\u00D710\u00B3 mm\u00B3)"), tr("Islands"), tr("Select") });
			m_iterationTable->setSelectionMode(QAbstractItemView::NoSelection);
			m_iterationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
			m_iterationTable->verticalHeader()->setVisible(false);
			m_iterationTable->horizontalHeader()->setSectionResizeMode(
				2, QHeaderView::Stretch);
			m_iterationTable->horizontalHeader()->setStretchLastSection(false);

			// QButtonGroup enforces mutual exclusivity across all row checkboxes.
			m_selectionGroup = new QButtonGroup(this);
			m_selectionGroup->setExclusive(true);

			connect(m_selectionGroup,
				QOverload<int>::of(&QButtonGroup::buttonClicked),
				this,
				[this](int) {
					m_btnApply->setEnabled(
						m_selectionGroup->checkedButton() != nullptr);
				});

			auto* historyBox = new QGroupBox(tr("Iteration History"), this);
			auto* historyBoxLayout = new QVBoxLayout(historyBox);
			historyBoxLayout->addWidget(m_iterationTable);

			// ── Right panel: chart (bottom) ───────────────────────────────────
			m_series = new QLineSeries(this);
			m_series->setName(tr("Volume"));

			m_currentPoint = new QScatterSeries(this);
			m_currentPoint->setName(tr("Current"));
			m_currentPoint->setMarkerSize(10.0);
			m_currentPoint->setColor(Qt::red);

			m_chart = new QChart;
			m_chart->addSeries(m_series);
			m_chart->addSeries(m_currentPoint);
			m_chart->setTitle(tr("Threshold vs. Region Volume"));
			m_chart->legend()->setVisible(true);
			m_chart->createDefaultAxes();
			rebuildAxes();

			auto* chartView = new QChartView(m_chart, this);
			chartView->setRenderHint(QPainter::Antialiasing);

			// Right side: table and chart stacked vertically
			auto* rightLayout = new QVBoxLayout;
			rightLayout->addWidget(historyBox, 1);
			rightLayout->addWidget(chartView, 2);

			// ── Root layout: left | right ─────────────────────────────────────
			auto* root = new QHBoxLayout(this);
			root->addLayout(leftLayout);
			root->addLayout(rightLayout, 1);
		}

		void setIterateCallback(IterateFunc fn) { m_iterateFunc = std::move(fn); }
		void setResetCallback(ResetFunc fn) { m_resetFunc = std::move(fn); }

	private:
		void updateStepSizeLabel(double multiplier)
		{
			m_labelStepSize->setText(
				QString::number(multiplier * m_baseStdDev, 'f', 3));
		}

		void runIterations()
		{
			if (!m_iterateFunc)
				return;

			m_btnRun->setEnabled(false);
			m_btnReset->setEnabled(false);
			m_btnApply->setEnabled(false);
			m_spinIterations->setEnabled(false);
			m_spinMultiplier->setEnabled(false);

			const int    maxIter = m_spinIterations->value();
			const double multiplier = m_spinMultiplier->value();

			for (int iter = 1; iter <= maxIter; ++iter)
			{
				const double threshold =
					m_baseThreshold
					+ static_cast<double>(iter) * multiplier * m_baseStdDev;

				const auto islands = m_iterateFunc(threshold);

				if (islands.empty())
				{
					qDebug("IterationProgressDialog: iter=%d — no islands; stopping.", iter);
					break;
				}

				// Total segmented volume from island voxel counts.
				double totalVoxels = 0.0;
				for (const auto& isl : islands)
					totalVoxels += static_cast<double>(isl.voxelCount);
				const double volumeMm3 = totalVoxels * m_voxelVolMm3;
				const double volumeMm3x1k = volumeMm3 * 1000.0;

				// Chart plots the same (threshold, volume×10³) pair shown in the table.
				m_series->append(threshold, volumeMm3x1k);
				m_currentPoint->clear();
				m_currentPoint->append(threshold, volumeMm3x1k);

				// Qt5: remove-and-re-add forces axis range recalculation.
				// Use removeSeries() (not removeAllSeries()) — the latter deletes the series!
				m_chart->removeSeries(m_series);
				m_chart->removeSeries(m_currentPoint);
				m_chart->addSeries(m_series);
				m_chart->addSeries(m_currentPoint);
				m_chart->createDefaultAxes();
				rebuildAxes();

				// Append one row to the iteration history table.
				appendIterationRow(iter, threshold, volumeMm3x1k,
					static_cast<int>(islands.size()));

				QCoreApplication::processEvents();
			}

			m_btnRun->setEnabled(true);
			m_btnReset->setEnabled(true);
			m_btnApply->setEnabled(
				m_selectionGroup->checkedButton() != nullptr);
			m_spinIterations->setEnabled(true);
			m_spinMultiplier->setEnabled(true);
		}

		// Appends one row to the iteration history table and registers its
		// checkbox with the exclusive selection group.
		// volumeMm3x1k is already scaled ×10³ to match the chart and table header.
		void appendIterationRow(int iter, double threshold, double volumeMm3x1k,
			int islandCount)
		{
			const int row = m_iterationTable->rowCount();
			m_iterationTable->insertRow(row);

			// Store the threshold so Apply Selection can retrieve it by row id.
			m_iterationThresholds.push_back(threshold);

			// Volume ×10³ to 3 significant figures — matches the chart Y axis exactly.
			const QString volStr = QString::number(volumeMm3x1k, 'g', 3);

			auto makeItem = [](const QString& text) -> QTableWidgetItem*
				{
					auto* item = new QTableWidgetItem(text);
					item->setTextAlignment(Qt::AlignCenter);
					return item;
				};

			m_iterationTable->setItem(row, 0, makeItem(QString::number(iter)));
			m_iterationTable->setItem(row, 1,
				makeItem(QString::number(threshold, 'f', 2)));
			m_iterationTable->setItem(row, 2, makeItem(volStr));
			m_iterationTable->setItem(row, 3,
				makeItem(QString::number(islandCount)));

			// Centred checkbox in col 4 registered with the exclusive group.
			auto* chkWidget = new QWidget(m_iterationTable);
			auto* chk = new QCheckBox(chkWidget);

			// Use row as the button id so Apply Selection can map id → threshold.
			m_selectionGroup->addButton(chk, row);

			auto* chkLayout = new QHBoxLayout(chkWidget);
			chkLayout->addWidget(chk);
			chkLayout->setAlignment(Qt::AlignCenter);
			chkLayout->setContentsMargins(0, 0, 0, 0);

			m_iterationTable->setCellWidget(row, 4, chkWidget);
			m_iterationTable->scrollToBottom();
		}

		// Clears all accumulated iteration data and restores the baseline view.
		void resetState()
		{
			// Remove all buttons from the exclusive group before clearing the table
			// so QButtonGroup does not hold dangling pointers to deleted widgets.
			const auto buttons = m_selectionGroup->buttons();
			for (auto* btn : buttons)
				m_selectionGroup->removeButton(btn);

			m_iterationThresholds.clear();
			m_iterationTable->setRowCount(0);
			m_btnApply->setEnabled(false);

			// Clear chart data and reset axis ranges.
			m_series->clear();
			m_currentPoint->clear();
			m_chart->removeSeries(m_series);
			m_chart->removeSeries(m_currentPoint);
			m_chart->addSeries(m_series);
			m_chart->addSeries(m_currentPoint);
			m_chart->createDefaultAxes();
			rebuildAxes();

			if (m_resetFunc)
				m_resetFunc();

			qDebug("IterationProgressDialog: state reset to baseline.");
		}

		// Re-runs segmentation at the selected iteration's threshold and
		// updates the 3D view through the iterate callback.
		void applySelection()
		{
			if (!m_iterateFunc)
				return;

			QAbstractButton* checked = m_selectionGroup->checkedButton();
			if (!checked)
				return;

			const int id = m_selectionGroup->id(checked);
			if (id < 0 || id >= static_cast<int>(m_iterationThresholds.size()))
				return;

			const double threshold =
				m_iterationThresholds[static_cast<std::size_t>(id)];

			qDebug("IterationProgressDialog: applying selection — "
				   "row=%d  threshold=%.4f", id, threshold);

			m_btnRun->setEnabled(false);
			m_btnReset->setEnabled(false);
			m_btnApply->setEnabled(false);

			m_iterateFunc(threshold);

			m_btnRun->setEnabled(true);
			m_btnReset->setEnabled(true);
			m_btnApply->setEnabled(true);
		}

		void rebuildAxes()
		{
			if (auto* ax = qobject_cast<QValueAxis*>(
				m_chart->axes(Qt::Horizontal).value(0)))
				ax->setTitleText(tr("Threshold"));
			if (auto* ay = qobject_cast<QValueAxis*>(
				m_chart->axes(Qt::Vertical).value(0)))
				ay->setTitleText(tr("Volume (\u00D710\u00B3 mm\u00B3)"));
		}

		double          m_baseThreshold;
		double          m_baseStdDev;
		double          m_voxelVolMm3;

		QSpinBox* m_spinIterations = nullptr;
		QDoubleSpinBox* m_spinMultiplier = nullptr;
		QLabel* m_labelStepSize = nullptr;
		QPushButton* m_btnRun = nullptr;
		QPushButton* m_btnReset = nullptr;
		QPushButton* m_btnApply = nullptr;

		QTableWidget* m_iterationTable = nullptr;
		QButtonGroup* m_selectionGroup = nullptr;
		QLineSeries* m_series = nullptr;
		QScatterSeries* m_currentPoint = nullptr;
		QChart* m_chart = nullptr;

		IterateFunc              m_iterateFunc;
		ResetFunc                m_resetFunc;
		std::vector<double>      m_iterationThresholds;
	};

} // anonymous namespace


// ---------------------------------------------------------------------------
// PrototypeMainWindow
// ---------------------------------------------------------------------------

PrototypeMainWindow::PrototypeMainWindow(QWidget* parent)
	: QMainWindow(parent)
	, ui(new Ui::MainWindow)
{
	ui->setupUi(this);

	// Progress bar (permanent widget on status bar, hidden until loading starts)
	m_progressBar = new QProgressBar(this);
	m_progressBar->setRange(0, 100);
	m_progressBar->setValue(0);
	m_progressBar->setVisible(false);
	statusBar()->addPermanentWidget(m_progressBar);

	// ------------------------------------------------------------------
	// Toolbar button order:
	//   File | Reslice | Landmark | Regions | Regions Alt |
	//   Graph Cut | Clean | Outline | Restart
	// ------------------------------------------------------------------

	// "File" toolbar button - always enabled.
	// Opens a QFileDialog to select a project sidecar JSON.
	// This is the primary entry point when no path is given on the command line.
	m_actFile = new QAction(tr("File"), this);
	m_actFile->setToolTip(tr("Open a project sidecar JSON file"));
	ui->toolBar->addAction(m_actFile);
	connect(m_actFile, &QAction::triggered, this, &PrototypeMainWindow::onFileOpen);

	// "Reslice" toolbar button: reslice the volume aligned to the PCA axes
	m_actReslice = new QAction(tr("Reslice"), this);
	m_actReslice->setToolTip(tr("Reslice the volume aligned to the PCA principal axes"));
	ui->toolBar->addAction(m_actReslice);
	connect(m_actReslice, &QAction::triggered, this, &PrototypeMainWindow::onReslice);

	// "Landmark" toolbar button: searches along each PCA axis for surface transitions
	m_actLandmark = new QAction(tr("Landmark"), this);
	m_actLandmark->setToolTip(tr("Find surface landmark points along the PCA axes"));
	ui->toolBar->addAction(m_actLandmark);
	connect(m_actLandmark, &QAction::triggered, this, &PrototypeMainWindow::onLandmark);

	// "Regions" toolbar button: threshold + seeded BFS island segmentation
	m_actRegions = new QAction(tr("Regions"), this);
	m_actRegions->setToolTip(tr("Segment bone islands from the resliced volume using landmark seeds"));
	ui->toolBar->addAction(m_actRegions);
	connect(m_actRegions, &QAction::triggered, this, &PrototypeMainWindow::onRegions);

	// "Regions Alt" toolbar button: morphological pipeline (smooth -> erode -> dilate -> connectivity)
	m_actRegionsAlt = new QAction(tr("Regions Alt"), this);
	m_actRegionsAlt->setToolTip(tr("Segment bone islands using the morphological pipeline "
		"(Gaussian smooth -> erode -> dilate -> seeded connectivity)"));
	ui->toolBar->addAction(m_actRegionsAlt);
	connect(m_actRegionsAlt, &QAction::triggered, this, &PrototypeMainWindow::onRegionsAlt);

	// "Graph Cut" toolbar button: ITK ImageGridCutFilter (multi-threaded GridCut solver)
	m_actRegionsGraphCut = new QAction(tr("Graph Cut"), this);
	m_actRegionsGraphCut->setToolTip(tr(
		"Segment bone islands using ITK graph cut (ImageGridCutFilter, multi-threaded GridCut solver)"));
	ui->toolBar->addAction(m_actRegionsGraphCut);
	connect(m_actRegionsGraphCut, &QAction::triggered,
			this, &PrototypeMainWindow::onRegionsGraphCut);

	// "Clean" toolbar button: post-segmentation clean step.
	// Enabled only after segmentation completes AND at least 8 Reslice operations
	// have been performed in the current session (m_resliceCount >= 8).
	m_actClean = new QAction(tr("Clean"), this);
	m_actClean->setToolTip(tr("Run the post-segmentation clean step "
		"(available after 8 or more Reslice operations)"));
	ui->toolBar->addAction(m_actClean);
	connect(m_actClean, &QAction::triggered, this, &PrototypeMainWindow::onClean);

	// "Export Reslice" toolbar button: reslice -> landmark -> threshold crop -> NIfTI export.
	m_actExportReslice = new QAction(tr("Export Reslice"), this);
	m_actExportReslice->setToolTip(tr(
		"Reslice, landmark, threshold-crop the volume and export as a NIfTI file"));
	ui->toolBar->addAction(m_actExportReslice);
	connect(m_actExportReslice, &QAction::triggered,
		this, &PrototypeMainWindow::onExportReslice);

	// "Restart" toolbar button: revert to the original image and reset the workflow.
	// Always enabled - Restart can be applied at any workflow step.
	m_actRestart = new QAction(tr("Restart"), this);
	m_actRestart->setToolTip(tr("Revert to the original image and reset the workflow to the start"));
	ui->toolBar->addAction(m_actRestart);
	connect(m_actRestart, &QAction::triggered, this, &PrototypeMainWindow::onRestart);

	// Apply the initial workflow step state: only Reslice enabled at startup.
	// Restart is always enabled regardless of step; set it explicitly here.
	m_actRestart->setEnabled(true);
	setWorkflowStep(WorkflowStep::Idle);

	// ImageLoader + VTK event wiring
	m_imageLoader = vtkSmartPointer<ImageLoader>::New();
	m_vtkConnections = vtkSmartPointer<vtkEventQtSlotConnect>::New();

	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::StartEvent,
		this, SLOT(onVtkStartEvent()));

	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::EndEvent,
		this, SLOT(onVtkEndEvent()));

	m_vtkConnections->Connect(
		m_imageLoader, vtkCommand::ProgressEvent,
		this, SLOT(onVtkProgressEvent()));

	ui->sliceView->setInterpolationToNearest();
	ui->sliceView->setOrthoPlanes(ui->volumeView->orthoPlanes());
}

PrototypeMainWindow::~PrototypeMainWindow()
{
	delete ui;
}

// ---------------------------------------------------------------------------
// Window close - flush the JSON cache to the prototype sidecar
// ---------------------------------------------------------------------------

void PrototypeMainWindow::closeEvent(QCloseEvent* event)
{
	writePrototypeSidecar();
	QMainWindow::closeEvent(event);
}

// ---------------------------------------------------------------------------
// Workflow step state machine
// ---------------------------------------------------------------------------

void PrototypeMainWindow::setWorkflowStep(WorkflowStep step)
{
	m_workflowStep = step;

	// Determine enabled state for each step button based on the current step.
	// Restart is always enabled and managed independently.
	//
	// Workflow routes:
	//   A) Idle -> Resliced -> Landmarked -> Segmented  (via Regions)
	//   B) Idle -> Resliced -> Landmarked -> Segmented  (via Regions Alt)
	//   C) Idle -> Resliced -> Landmarked -> Segmented  (via Graph Cut)
	//
	// At each step exactly one (or three, at Landmarked) button is enabled:
	//   Idle       : Reslice=on,  Landmark=off, Regions=off, RegionsAlt=off, GraphCut=off
	//   Resliced   : Reslice=off, Landmark=on,  Regions=off, RegionsAlt=off, GraphCut=off
	//   Landmarked : Reslice=off, Landmark=off, Regions=on,  RegionsAlt=on,  GraphCut=on
	//   Segmented  : Reslice=off, Landmark=off, Regions=off, RegionsAlt=off, GraphCut=off

	const bool atIdle      = (step == WorkflowStep::Idle);
	const bool atResliced  = (step == WorkflowStep::Resliced);
	const bool atLandmarked = (step == WorkflowStep::Landmarked);

	// File is always available - the user can open a new sidecar at any time.
	m_actFile->setEnabled(true);

	m_actReslice->setEnabled(atIdle);
	m_actLandmark->setEnabled(atResliced);
	m_actRegions->setEnabled(atLandmarked);
	m_actRegionsAlt->setEnabled(atLandmarked);
	m_actRegionsGraphCut->setEnabled(atLandmarked);

	// Export Reslice is available whenever a valid image and finite threshold exist.
	// It runs its own internal reslice + landmark pipeline so it does not require
	// a specific prior workflow step.
	m_actExportReslice->setEnabled(m_image != nullptr && std::isfinite(m_threshold));

	// Restart is always enabled - it can be applied at any time.
	m_actRestart->setEnabled(true);
}

// ---------------------------------------------------------------------------
// Progress slots
// ---------------------------------------------------------------------------

void PrototypeMainWindow::showProgressStart()
{
	m_progressBar->setValue(0);
	m_progressBar->setVisible(true);
	m_progressBar->setEnabled(true);
}

void PrototypeMainWindow::showProgressValue(int percent)
{
	m_progressBar->setValue(percent);
	m_progressBar->setVisible(true);
}

void PrototypeMainWindow::showProgressEnd()
{
	m_progressBar->setValue(100);
	m_progressBar->setVisible(false);
}

void PrototypeMainWindow::onVtkStartEvent()
{
	if (QThread::currentThread() == this->thread())
	{
		showProgressStart();
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else
	{
		QMetaObject::invokeMethod(this, "showProgressStart", Qt::QueuedConnection);
	}
}

void PrototypeMainWindow::onVtkEndEvent()
{
	if (QThread::currentThread() == this->thread())
	{
		showProgressEnd();
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else
	{
		QMetaObject::invokeMethod(this, "showProgressEnd", Qt::QueuedConnection);
	}
}

void PrototypeMainWindow::onVtkProgressEvent()
{
	if (!m_imageLoader) return;

	const int value = static_cast<int>(
		std::clamp(m_imageLoader->GetProgress(), 0.0, 1.0) * 100.0);

	if (QThread::currentThread() == this->thread())
	{
		showProgressValue(value);
		m_progressBar->update();
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}
	else
	{
		QMetaObject::invokeMethod(this, "showProgressValue",
			Qt::QueuedConnection, Q_ARG(int, value));
	}
}

// ---------------------------------------------------------------------------
// PCA overlay management
// ---------------------------------------------------------------------------

void PrototypeMainWindow::clearPcaOverlay()
{
	if (!ui || !ui->volumeView)
		return;

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	for (auto& a : m_axisActors)
	{
		if (a) { ren->RemoveActor(a); a = nullptr; }
	}
	for (auto& a : m_tipActors)
	{
		if (a) { ren->RemoveActor(a); a = nullptr; }
	}
	for (auto& a : m_ringActors)
	{
		if (a) { ren->RemoveActor(a); a = nullptr; }
	}

	// Remove landmark label actors added by onLandmark()
	for (auto& a : m_landmarkLabelActors)
	{
		if (a) { ren->RemoveActor(a); a = nullptr; }
	}
}

// ---------------------------------------------------------------------------
// Island actor management
// ---------------------------------------------------------------------------

void PrototypeMainWindow::clearIslandActors()
{
	if (!ui || !ui->volumeView)
		return;

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	// Remove surface actors
	for (auto& a : m_islandActors)
	{
		if (a) ren->RemoveActor(a);
	}
	m_islandActors.clear();

	// Remove the scalar bar when it exists
	if (m_islandScalarBar)
	{
		ren->RemoveActor(m_islandScalarBar);
		m_islandScalarBar = nullptr;
	}
}

// ---------------------------------------------------------------------------
// Graph-cut seed actor management
// ---------------------------------------------------------------------------

void PrototypeMainWindow::clearGraphCutSeedActors()
{
	if (!ui || !ui->volumeView)
		return;

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	// Remove the debug seed-cloud actors (FG green + BG orange) added by
	// onRegionsGraphCut() and release the smart-pointer references.
	for (auto& a : m_graphCutSeedActors)
	{
		if (a) { ren->RemoveActor(a); a = nullptr; }
	}
	m_graphCutSeedActors.clear();
}

// ---------------------------------------------------------------------------
// PCA JSON serialisation helper
// ---------------------------------------------------------------------------

// static
QJsonObject PrototypeMainWindow::pcaResultToJson(const PrototypeHelpers::PcaResult& pca)
{
	auto packVec3 = [](const double v[3]) -> QJsonArray
		{
			return QJsonArray{ v[0], v[1], v[2] };
		};

	// Axes: array of 3 objects, one per principal axis
	QJsonArray axesArray;
	for (int i = 0; i < 3; ++i)
	{
		QJsonObject axisObj;
		axisObj[QStringLiteral("index")] = i;
		axisObj[QStringLiteral("eigenvalue")] = pca.eigenvalues[i];
		axisObj[QStringLiteral("direction")] = packVec3(pca.axes[i]);
		axesArray.append(axisObj);
	}

	QJsonObject obj;
	obj[QStringLiteral("centroid")] = packVec3(pca.centroid);
	obj[QStringLiteral("circumRadius")] = pca.circumRadius;
	obj[QStringLiteral("axes")] = axesArray;
	return obj;
}

// ---------------------------------------------------------------------------
// Prototype sidecar output path + write
// ---------------------------------------------------------------------------

QString PrototypeMainWindow::prototypeOutputPath() const
{
	if (m_sidecarPath.isEmpty() || m_cropPath.isEmpty())
		return {};

	// Derive the output filename from the crop image basename:
	//   <crop_basename>_prototype.json
	const QString cropBaseName = QFileInfo(m_cropPath).completeBaseName();
	const QString outputName = cropBaseName + QStringLiteral("_prototype.json");

	// Place the file in the same directory as the source sidecar.
	const QString sidecarDir = QFileInfo(m_sidecarPath).absolutePath();
	return QDir(sidecarDir).filePath(outputName);
}

bool PrototypeMainWindow::writePrototypeSidecar() const
{
	// Nothing to write if no landmark run has completed yet.
	if (m_landmarkJson.isEmpty())
	{
		qDebug("writePrototypeSidecar: no landmark data to write; skipping.");
		return true;
	}

	const QString outputPath = prototypeOutputPath();
	if (outputPath.isEmpty())
	{
		qWarning("writePrototypeSidecar: cannot determine output path "
				 "(sidecar or crop path not set); skipping.");
		return false;
	}

	QFile f(outputPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		qWarning("writePrototypeSidecar: failed to open '%s' for writing: %s",
				 qUtf8Printable(outputPath),
				 qUtf8Printable(f.errorString()));
		return false;
	}

	const QByteArray json = QJsonDocument(m_landmarkJson).toJson(QJsonDocument::Indented);
	f.write(json);
	f.close();

	qDebug("writePrototypeSidecar: wrote %lld bytes to '%s'",
		   static_cast<long long>(json.size()),
		   qUtf8Printable(outputPath));
	return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void PrototypeMainWindow::loadFromSidecar(const QString& sidecarPath)
{
	const QJsonObject sidecar = PrototypeHelpers::readJsonObjectFileOrThrow(sidecarPath);

	const QString cropPath = PrototypeHelpers::cropPathFromSidecarOrThrow(sidecar);
	qDebug("Project:   %s", qUtf8Printable(sidecarPath));
	qDebug("Crop path: %s", qUtf8Printable(cropPath));

	m_threshold = PrototypeHelpers::thresholdFromSidecar(sidecar);

	if (!QFileInfo::exists(cropPath))
	{
		throw std::runtime_error(
			("Error: crop output does not exist:\n" + cropPath).toStdString());
	}

	// Optional: exercise canonical sidecar mapping logic.
	const QJsonObject canonical = JsonUtils::readJsonSidecar(cropPath);
	if (!canonical.isEmpty())
		qDebug("JsonUtils canonical sidecar found for crop path.");

	if (!ImageLoader::CanReadFile(cropPath))
		throw std::runtime_error(("Unsupported or unreadable file: " + cropPath).toStdString());

	// Cache the paths so closeEvent() can derive the prototype output filename.
	m_sidecarPath = QFileInfo(sidecarPath).absoluteFilePath();
	m_cropPath = cropPath;

	m_imageLoader->SetInputPath(cropPath);
	m_imageLoader->SetImageType(ImageLoader::ImageType::NIFTI);
	m_imageLoader->Update();

	vtkSmartPointer<vtkImageData> out = m_imageLoader->GetOutput();
	if (!out)
		throw std::runtime_error(("ImageLoader returned null output for: " + cropPath).toStdString());

	const int* dims = out->GetDimensions();
	if (dims[0] <= 1 || dims[1] <= 1 || dims[2] <= 1)
		throw std::runtime_error(("ImageLoader produced invalid volume dimensions for: " + cropPath).toStdString());

	// Keep a deep copy of the original image so Restart can restore it without
	// re-reading from disk.
	m_originalImage = vtkSmartPointer<vtkImageData>::New();
	m_originalImage->DeepCopy(out);

	setImage(out);

	// A freshly loaded image starts the workflow at Idle (only Reslice enabled).
	setWorkflowStep(WorkflowStep::Idle);
}

void PrototypeMainWindow::setImage(vtkImageData* image)
{
	if (!ui || !ui->volumeView)
		return;

	// Remove any PCA overlay from a previous image.
	clearPcaOverlay();

	// Remove island surface actors, scalar bar, and graph-cut seed actors
	// that belong to a prior segmentation run.  These must be cleared before
	// m_labelImage is released so the actor pipeline does not hold a dangling
	// reference to a VTK image that is about to be replaced.
	clearIslandActors();
	clearGraphCutSeedActors();

	// Release derived segmentation data so downstream steps (onLandmark,
	// onRegions*) always start from a clean slate for the incoming image.
	m_labelImage = nullptr;
	m_islands.clear();

	// Cache raw pointer for use by onLandmark() (lifetime owned by m_imageLoader pipeline).
	m_image = image;

	// Invalidate any previously cached PCA result and landmark data.
	m_pca.valid = false;
	m_landmarkResult = QJsonObject{};
	m_landmarkPoints = {};

	ui->volumeView->setImageData(image);
	ui->volumeView->updateData();

	// Determine window/level from the image and the cached sidecar threshold.
	// level  = threshold (falls back to scalar range midpoint if not present)
	// window = 2 x overall standard deviation (from computeScalarThresholdStats)
	//
	// computeScalarThresholdStats() is called unconditionally here so that
	// m_imageStats is always populated for downstream steps (e.g. Clean),
	// regardless of whether a finite threshold is available.  When no threshold
	// is present the foreground/background partition keys will reflect a
	// degenerate split (all voxels treated as background), which is acceptable
	// because the Clean step is gated on a finite threshold being available.
	if (!image)
		return;

	// Compute threshold-partitioned statistics once and cache for all consumers.
	// Pass the threshold only when it is finite; computeScalarThresholdStats
	// handles a NaN threshold gracefully (all voxels fall into background).
	const double effectiveThreshold = std::isfinite(m_threshold)
		? m_threshold
		: std::numeric_limits<double>::quiet_NaN();

	m_imageStats = PrototypeHelpers::computeScalarThresholdStats(image, effectiveThreshold);

	qDebug("setImage: imageStats - mean=%.4f  stdDev=%.4f  "
		   "meanFg=%.4f  stdDevFg=%.4f  meanBg=%.4f  stdDevBg=%.4f",
		   m_imageStats.value(QStringLiteral("mean")).toDouble(),
		   m_imageStats.value(QStringLiteral("stdDev")).toDouble(),
		   m_imageStats.value(QStringLiteral("meanFg")).toDouble(),
		   m_imageStats.value(QStringLiteral("stdDevFg")).toDouble(),
		   m_imageStats.value(QStringLiteral("meanBg")).toDouble(),
		   m_imageStats.value(QStringLiteral("stdDevBg")).toDouble());

	const double scalarRange[2] = {
		m_imageStats.value(QStringLiteral("min")).toDouble(0.0),
		m_imageStats.value(QStringLiteral("max")).toDouble(255.0) };

	// level: threshold when finite, otherwise midpoint of the scalar range.
	// window: 2 x whole-volume standard deviation sourced from m_imageStats.
	const double level = std::isfinite(m_threshold)
		? m_threshold
		: 0.5 * (scalarRange[0] + scalarRange[1]);

	const double window = 2.0 * m_imageStats.value(QStringLiteral("stdDev")).toDouble(1.0);

	ui->volumeView->setColorWindowLevel(window, level);
	syncSliceView(image, window, level);

	// ------------------------------------------------------------------
	// PCA overlay: only when a finite threshold is available
	// ------------------------------------------------------------------
	if (!std::isfinite(m_threshold))
	{
		qDebug("setImage: no finite threshold - PCA overlay skipped.");
		return;
	}

	// Progress callback: updates the status-bar progress bar and pumps
	// paint events so the bar repaints between the two expensive passes.
	// The PCA occupies the [0..100] range independently of the VTK load.
	showProgressStart();
	const auto pcaProgress = [this](int percent)
		{
			showProgressValue(percent);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		};

	const bool ok = PrototypeHelpers::computePca(image, m_threshold, m_pca, pcaProgress);
	showProgressEnd();

	if (!ok)
		return;

	// ------------------------------------------------------------------
	// Cache the PCA result to JSON.
	//
	// m_originalPcaJson is written ONLY on the first (original) load, i.e.
	// when no resliced image exists yet.  This ensures callers can always
	// retrieve the pre-reslice PCA as the definitive starting point even
	// after multiple reslice passes have been performed.
	//
	// m_reslicedPcaJson is written by onReslice() after this function
	// returns, so we do not touch it here.
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		m_originalPcaJson = pcaResultToJson(m_pca);
		qDebug("setImage: original PCA JSON cached:\n%s",
			   qUtf8Printable(QJsonDocument(m_originalPcaJson).toJson(QJsonDocument::Indented)));
	}

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	// Sphere glyph radius = 8 % of the circumsphere radius (4x the original 2 % size)
	const double glyphR = 0.08 * m_pca.circumRadius;

	// Axis colours: R=axis0 (largest variance), G=axis1, B=axis2
	const double axisColors[3][3] = {
		{ 1.0, 0.2, 0.2 },  // axis 0 - red
		{ 0.2, 1.0, 0.2 },  // axis 1 - green
		{ 0.2, 0.2, 1.0 },  // axis 2 - blue
	};

	for (int i = 0; i < 3; ++i)
	{
		const double* col = axisColors[i];
		const double  R = m_pca.circumRadius;

		// Tip points along +axis and -axis
		double tipPos[3], tipNeg[3];
		for (int d = 0; d < 3; ++d)
		{
			tipPos[d] = m_pca.centroid[d] + R * m_pca.axes[i][d];
			tipNeg[d] = m_pca.centroid[d] - R * m_pca.axes[i][d];
		}

		// Shaft from -tip to +tip
		m_axisActors[i] = PrototypeHelpers::makeLineActor(tipNeg, tipPos, col[0], col[1], col[2], 2.5);
		ren->AddActor(m_axisActors[i]);

		// Sphere glyphs at both ends (4x the original 2 % size)
		m_tipActors[static_cast<std::size_t>(i * 2)] = PrototypeHelpers::makeSphereActor(tipPos, glyphR, col[0], col[1], col[2]);
		m_tipActors[static_cast<std::size_t>(i * 2 + 1)] = PrototypeHelpers::makeSphereActor(tipNeg, glyphR, col[0], col[1], col[2]);
		ren->AddActor(m_tipActors[static_cast<std::size_t>(i * 2)]);
		ren->AddActor(m_tipActors[static_cast<std::size_t>(i * 2 + 1)]);

		// Ring i:
		//   - centre  : PCA centroid (all three rings share the same centre)
		//   - normal  : axes[i]  (the eigen direction for this axis)
		//   - radius  : circumsphere radius R
		// The ring lies in the plane perpendicular to axes[i] passing through
		// the centroid, so each ring slices through the centre of the point cloud.
		m_ringActors[i] = PrototypeHelpers::makeRingActor(m_pca.centroid, m_pca.axes[i], R,
														   col[0], col[1], col[2], 2.0);
		ren->AddActor(m_ringActors[i]);
	}

	ui->volumeView->renderer()->ResetCamera();
	ui->volumeView->renderer()->ResetCameraClippingRange();

	// Align the camera to the PCA axes immediately after the initial
	// ResetCamera so the bone is framed with the correct orientation from
	// the first frame, before Reslice or Landmark have been run.
	alignCameraToMediumAxis();
}


// ---------------------------------------------------------------------------
// File open slot - lets the user pick a project sidecar JSON at any time
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onFileOpen()
{
	const QString path = QFileDialog::getOpenFileName(
		this,
		tr("Open Project Sidecar"),
		QString(),                              // start in last-used directory
		tr("JSON Sidecar Files (*.json);;All Files (*)"));

	if (path.isEmpty())
		return;   // user cancelled - leave current state untouched

	try
	{
		// Reset workflow state before loading so stale actors and cached data
		// from a previous session do not bleed into the new one.
		onRestart();

		loadFromSidecar(path);
	}
	catch (const std::exception& ex)
	{
		QMessageBox::critical(
			this,
			tr("CTAXPrototype - Load Error"),
			QString::fromStdString(ex.what()));
	}
}

// ---------------------------------------------------------------------------
// Landmark slot
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onLandmark()
{
	if (!m_pca.valid)
	{
		qWarning("onLandmark: no valid PCA result available; load an image first.");
		return;
	}

	if (!m_image)
	{
		qWarning("onLandmark: no image cached.");
		return;
	}

	if (!std::isfinite(m_threshold))
	{
		qWarning("onLandmark: threshold is not finite; cannot search for surface.");
		return;
	}

	vtkRenderer* ren = (ui && ui->volumeView) ? ui->volumeView->renderer() : nullptr;

	// Sphere glyph radius reused from setImage() (8 % of circumsphere radius)
	const double glyphR = 0.08 * m_pca.circumRadius;

	// Label offset: push the text slightly away from the sphere glyph so it
	// does not sit inside it.  1.5 x sphere radius in world units.
	const double labelOffset = 1.5 * glyphR;

	// Axis colours matching those in setImage()
	const double axisColors[3][3] = {
		{ 1.0, 0.2, 0.2 },  // axis 0 - red
		{ 0.2, 1.0, 0.2 },  // axis 1 - green
		{ 0.2, 0.2, 1.0 },  // axis 2 - blue
	};

	// Axis label names: axis 0 = largest eigenvalue (longest), axis 2 = smallest
	const char* axisNames[3] = { "L", "M", "S" };   // Largest / Medium / Smallest

	// JSON arrays to accumulate per-axis landmark data
	QJsonArray jsonLandmarks;

	// Remove any existing landmark label actors before rebuilding
	if (ren)
	{
		for (auto& a : m_landmarkLabelActors)
		{
			if (a) { ren->RemoveActor(a); a = nullptr; }
		}
	}

	for (int i = 0; i < 3; ++i)
	{
		const double* col = axisColors[i];

		const double axisDirPos[3] = { m_pca.axes[i][0],  m_pca.axes[i][1],  m_pca.axes[i][2] };
		const double axisDirNeg[3] = { -m_pca.axes[i][0], -m_pca.axes[i][1], -m_pca.axes[i][2] };

		PrototypeHelpers::findSurfacePointFromBoundary(
			m_image, m_pca.centroid, axisDirPos, m_threshold,
			m_landmarkPoints[static_cast<std::size_t>(i)][0].data());
		PrototypeHelpers::findSurfacePointFromBoundary(
			m_image, m_pca.centroid, axisDirNeg, m_threshold,
			m_landmarkPoints[static_cast<std::size_t>(i)][1].data());

		const double* lPos = m_landmarkPoints[static_cast<std::size_t>(i)][0].data();
		const double* lNeg = m_landmarkPoints[static_cast<std::size_t>(i)][1].data();

		qDebug("Landmark axis %d  +: (%.2f, %.2f, %.2f)  -: (%.2f, %.2f, %.2f)",
			   i,
			   lPos[0], lPos[1], lPos[2],
			   lNeg[0], lNeg[1], lNeg[2]);

		// Relocate the existing tip sphere actors to the new surface positions
		if (ren)
		{
			const std::size_t posIdx = static_cast<std::size_t>(i * 2);
			const std::size_t negIdx = static_cast<std::size_t>(i * 2 + 1);

			if (m_tipActors[posIdx]) ren->RemoveActor(m_tipActors[posIdx]);
			if (m_tipActors[negIdx]) ren->RemoveActor(m_tipActors[negIdx]);

			m_tipActors[posIdx] = PrototypeHelpers::makeSphereActor(lPos, glyphR, col[0], col[1], col[2]);
			m_tipActors[negIdx] = PrototypeHelpers::makeSphereActor(lNeg, glyphR, col[0], col[1], col[2]);

			ren->AddActor(m_tipActors[posIdx]);
			ren->AddActor(m_tipActors[negIdx]);

			// ------------------------------------------------------------------
			// Billboard text labels at each landmark tip.
			//
			// The label for the positive tip is "<name>+" and for the negative
			// tip is "<name>-".  Each label is offset along its own eigenvector
			// direction so it clears the sphere glyph.
			//
			// vtkBillboardTextActor3D always faces the camera so the text is
			// readable from any viewpoint without requiring a vtkFollower camera
			// reference.  DisplayOffset shifts the label in screen pixels after
			// billboard projection - (10, 10) moves it up-right of the anchor.
			// ------------------------------------------------------------------
			auto makeLandmarkLabel =
				[&](const double pt[3], const double dir[3],
					const char* sign, const double c[3])
				-> vtkSmartPointer<vtkBillboardTextActor3D>
				{
					// World-space anchor = tip point + offset along eigenvector
					const double ax = pt[0] + dir[0] * labelOffset;
					const double ay = pt[1] + dir[1] * labelOffset;
					const double az = pt[2] + dir[2] * labelOffset;

					auto label = vtkSmartPointer<vtkBillboardTextActor3D>::New();

					const std::string text = std::string(axisNames[i]) + sign;
					label->SetInput(text.c_str());
					label->SetPosition(ax, ay, az);

					// Small screen-space nudge so the text doesn't overlap the sphere
					label->SetDisplayOffset(8, 8);

					vtkTextProperty* tp = label->GetTextProperty();
					tp->SetFontFamilyToArial();
					tp->SetFontSize(14);
					tp->SetBold(1);
					tp->SetItalic(0);
					tp->SetShadow(1);           // thin drop-shadow improves legibility
					tp->SetShadowOffset(1, -1);
					tp->SetColor(c[0], c[1], c[2]);
					tp->SetOpacity(1.0);

					return label;
				};

			const std::size_t lblPosIdx = static_cast<std::size_t>(i * 2);
			const std::size_t lblNegIdx = static_cast<std::size_t>(i * 2 + 1);

			m_landmarkLabelActors[lblPosIdx] =
				makeLandmarkLabel(lPos, axisDirPos, "+", col);
			m_landmarkLabelActors[lblNegIdx] =
				makeLandmarkLabel(lNeg, axisDirNeg, "-", col);

			ren->AddActor(m_landmarkLabelActors[lblPosIdx]);
			ren->AddActor(m_landmarkLabelActors[lblNegIdx]);
		}

		// Accumulate JSON for this axis
		auto packVec3 = [](const double v[3]) -> QJsonArray
			{
				return QJsonArray{ v[0], v[1], v[2] };
			};

		QJsonObject axisObj;
		axisObj[QStringLiteral("index")] = i;
		axisObj[QStringLiteral("eigenvalue")] = m_pca.eigenvalues[i];
		axisObj[QStringLiteral("eigenvector")] = packVec3(m_pca.axes[i]);
		axisObj[QStringLiteral("landmarkPos")] = packVec3(lPos);
		axisObj[QStringLiteral("landmarkNeg")] = packVec3(lNeg);
		jsonLandmarks.append(axisObj);
	}

	// ------------------------------------------------------------------
	// Build and cache the per-axis raw landmark result (existing behaviour)
	// ------------------------------------------------------------------
	auto packVec3 = [](const double v[3]) -> QJsonArray
		{
			return QJsonArray{ v[0], v[1], v[2] };
		};

	m_landmarkResult = QJsonObject{};
	m_landmarkResult[QStringLiteral("centroid")] = packVec3(m_pca.centroid);
	m_landmarkResult[QStringLiteral("circumRadius")] = m_pca.circumRadius;
	m_landmarkResult[QStringLiteral("threshold")] = m_threshold;
	m_landmarkResult[QStringLiteral("axes")] = jsonLandmarks;

	// ------------------------------------------------------------------
	// Build the consolidated landmark JSON cache (written to disk on close).
	// ------------------------------------------------------------------
	m_landmarkJson = QJsonObject{};
	m_landmarkJson[QStringLiteral("sourceSidecar")] = m_sidecarPath;
	m_landmarkJson[QStringLiteral("cropImage")] = m_cropPath;
	m_landmarkJson[QStringLiteral("threshold")] = m_threshold;

	if (!m_originalPcaJson.isEmpty())
		m_landmarkJson[QStringLiteral("originalPca")] = m_originalPcaJson;

	if (!m_reslicedPcaJson.isEmpty())
		m_landmarkJson[QStringLiteral("reslicedPca")] = m_reslicedPcaJson;

	m_landmarkJson[QStringLiteral("landmarks")] = m_landmarkResult;

	qDebug("onLandmark: landmark JSON cached:\n%s",
		   qUtf8Printable(QJsonDocument(m_landmarkJson).toJson(QJsonDocument::Indented)));

	if (ren)
	{
		ui->volumeView->render();
	}

	// Landmark completed - advance to Landmarked: Regions and Regions Alt enabled.
	setWorkflowStep(WorkflowStep::Landmarked);
}

// ---------------------------------------------------------------------------
// Reslice slot
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onReslice()
{
	if (!m_pca.valid)
	{
		qWarning("onReslice: no valid PCA result available; load an image first.");
		return;
	}

	if (!m_image)
	{
		qWarning("onReslice: no image cached.");
		return;
	}

	auto resliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
	resliceAxes->Identity();

	for (int row = 0; row < 3; ++row)
	{
		resliceAxes->SetElement(row, 0, m_pca.axes[0][row]);
		resliceAxes->SetElement(row, 1, m_pca.axes[1][row]);
		resliceAxes->SetElement(row, 2, m_pca.axes[2][row]);
		resliceAxes->SetElement(row, 3, m_pca.centroid[row]);
	}

	qDebug("onReslice: reslice axes matrix:");
	for (int r = 0; r < 4; ++r)
	{
		qDebug("  [ %8.4f  %8.4f  %8.4f  %8.4f ]",
			   resliceAxes->GetElement(r, 0),
			   resliceAxes->GetElement(r, 1),
			   resliceAxes->GetElement(r, 2),
			   resliceAxes->GetElement(r, 3));
	}

	showProgressStart();
	showProgressValue(10);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	const double bgMean = m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);

	auto reslice = vtkSmartPointer<vtkImageReslice>::New();
	reslice->SetInputData(m_image);
	reslice->SetResliceAxes(resliceAxes);
	reslice->SetInterpolationModeToCubic();
	reslice->AutoCropOutputOn();
	reslice->SetOutputDimensionality(3);
	reslice->SetBackgroundLevel(bgMean);
	reslice->Update();

	showProgressValue(90);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	vtkImageData* resliced = reslice->GetOutput();
	if (!resliced)
	{
		qWarning("onReslice: vtkImageReslice produced null output.");
		showProgressEnd();
		return;
	}

	const int* outDims = resliced->GetDimensions();
	qDebug("onReslice: output dimensions: %d x %d x %d", outDims[0], outDims[1], outDims[2]);

	m_reslicedImage = vtkSmartPointer<vtkImageData>::New();
	m_reslicedImage->DeepCopy(resliced);

	showProgressEnd();

	setImage(m_reslicedImage);

	if (m_pca.valid)
	{
		m_reslicedPcaJson = pcaResultToJson(m_pca);
		qDebug("onReslice: resliced PCA JSON cached:\n%s",
			   qUtf8Printable(QJsonDocument(m_reslicedPcaJson).toJson(QJsonDocument::Indented)));
	}

	// Increment the cumulative reslice counter for this session.
	// The Clean button gate (m_resliceCount >= 8) is re-evaluated by
	// setWorkflowStep() every time a new step is entered.
	++m_resliceCount;
	qDebug("onReslice: m_resliceCount=%d", m_resliceCount);

	// Reslice completed - advance to Resliced: only Landmark enabled.
	setWorkflowStep(WorkflowStep::Resliced);
}

// ---------------------------------------------------------------------------
// Shared helper - build island actors from a completed segmentation result
// and merge the regions summary into the landmark JSON cache.
// Called by both onRegions() and onRegionsAlt() after segmentation completes.
// ---------------------------------------------------------------------------

void PrototypeMainWindow::applyIslandSegmentationResult(
	const std::vector<PrototypeHelpers::BoneIsland>& islands,
	vtkSmartPointer<vtkImageData>                    labelImage)
{
	// Cache the label image so it stays alive for the actors' pipeline
	m_labelImage = labelImage;

	// Cache the island vector for downstream consumers (Clean, export, stats).
	m_islands = islands;

	// Remove actors from any previous regions run (including scalar bar)
	clearIslandActors();

	vtkRenderer* ren = (ui && ui->volumeView) ? ui->volumeView->renderer() : nullptr;

	const int nIslands = static_cast<int>(islands.size());

	// ------------------------------------------------------------------
	// Determine the voxel-count range across all islands for colour mapping
	// ------------------------------------------------------------------
	vtkIdType minVoxels = islands[0].voxelCount;
	vtkIdType maxVoxels = islands[0].voxelCount;

	for (const auto& isl : islands)
	{
		minVoxels = std::min(minVoxels, isl.voxelCount);
		maxVoxels = std::max(maxVoxels, isl.voxelCount);
	}

	qDebug("applyIslandSegmentationResult: voxel-count range  min=%lld  max=%lld",
		   static_cast<long long>(minVoxels),
		   static_cast<long long>(maxVoxels));

	// ------------------------------------------------------------------
	// Build the colour transfer function over [minVoxels, maxVoxels]
	// ------------------------------------------------------------------
	auto colorTF = PrototypeHelpers::makeIslandColorTF(
		static_cast<double>(minVoxels),
		static_cast<double>(maxVoxels));

	// ------------------------------------------------------------------
	// Create one surface actor per island, coloured by its voxel count
	// ------------------------------------------------------------------
	QJsonArray regionsArray;

	for (int idx = 0; idx < nIslands; ++idx)
	{
		const auto& island = islands[static_cast<std::size_t>(idx)];

		// Sample the transfer function at this island's voxel count
		double rgb[3] = { 1.0, 1.0, 1.0 };
		colorTF->GetColor(static_cast<double>(island.voxelCount), rgb);

		auto actor = PrototypeHelpers::makeIslandSurfaceActor(
			m_labelImage,
			island.label,
			rgb[0], rgb[1], rgb[2],
			0.55);

		m_islandActors.push_back(actor);

		if (ren)
			ren->AddActor(actor);

		qDebug("applyIslandSegmentationResult: island %d  label=%d  voxels=%lld  rgb=(%.3f,%.3f,%.3f)",
			   idx, island.label,
			   static_cast<long long>(island.voxelCount),
			   rgb[0], rgb[1], rgb[2]);

		// Augment the existing island JSON with the mapped colour for reference
		QJsonObject islandJson = island.json;
		islandJson[QStringLiteral("colorR")] = rgb[0];
		islandJson[QStringLiteral("colorG")] = rgb[1];
		islandJson[QStringLiteral("colorB")] = rgb[2];
		regionsArray.append(islandJson);
	}

	// ------------------------------------------------------------------
	// Scalar bar - only when there are at least 2 distinct islands so the
	// colour scale has meaningful variation to display
	// ------------------------------------------------------------------
	if (nIslands > 1 && ren)
	{
		m_islandScalarBar = PrototypeHelpers::makeIslandScalarBar(
			colorTF,
			minVoxels,
			maxVoxels);

		ren->AddActor(m_islandScalarBar);

		qDebug("applyIslandSegmentationResult: scalar bar added (%d islands, range %lld-%lld voxels).",
			   nIslands,
			   static_cast<long long>(minVoxels),
			   static_cast<long long>(maxVoxels));
	}

	// ------------------------------------------------------------------
	// Merge regions summary into the landmark JSON cache
	// ------------------------------------------------------------------
	m_landmarkJson[QStringLiteral("regions")] = regionsArray;

	qDebug("applyIslandSegmentationResult: %d islands cached in landmarkJson[\"regions\"].",
		   nIslands);

	if (ren)
		ui->volumeView->render();
}

// ---------------------------------------------------------------------------
// Threshold + seeded BFS flood-fill ? island surface actors
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Threshold + seeded BFS flood-fill → island surface actors
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// onRegions
//
// Runs an initial BFS segmentation at the baseline threshold, displays the
// result, then opens the non-modal IterationProgressDialog so the user can
// iterate autonomously.  onRegions() returns as soon as the dialog is shown;
// the iteration loop runs inside the dialog when the user clicks Run.
// ---------------------------------------------------------------------------
void PrototypeMainWindow::onRegions()
{
	// ------------------------------------------------------------------
	// Pre-conditions
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		qWarning("onRegions: no resliced image available; run Reslice first.");
		return;
	}
	if (m_landmarkResult.isEmpty())
	{
		qWarning("onRegions: no landmark points available; run Landmark first.");
		return;
	}
	if (!std::isfinite(m_threshold))
	{
		qWarning("onRegions: threshold is not finite; cannot segment.");
		return;
	}

	// ------------------------------------------------------------------
	// Collect the 6 landmark world-space seed points.
	// Captured by value in the iterate callback so they remain valid after
	// onRegions() returns.
	// ------------------------------------------------------------------
	std::vector<std::array<double, 3>> seeds;
	seeds.reserve(6);
	for (int i = 0; i < 3; ++i)
		for (int d = 0; d < 2; ++d)
		{
			const double* pt =
				m_landmarkPoints[static_cast<std::size_t>(i)]
				[static_cast<std::size_t>(d)].data();
			seeds.push_back({ pt[0], pt[1], pt[2] });
		}

	const auto makeProgress = [this](int percent)
		{
			showProgressValue(percent);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		};

	// ------------------------------------------------------------------
	// Step 1: initial region grow at baseline threshold
	// ------------------------------------------------------------------
	showProgressStart();
	qDebug("onRegions: initial region grow  threshold=%.4f", m_threshold);

	vtkSmartPointer<vtkImageData> labelImage;
	std::vector<PrototypeHelpers::BoneIsland> islands =
		PrototypeHelpers::segmentBoneIslands(
			m_reslicedImage, m_threshold, seeds, labelImage, makeProgress);

	showProgressEnd();

	if (islands.empty())
	{
		qWarning("onRegions: no bone islands found at baseline threshold.");
		return;
	}

	// ------------------------------------------------------------------
	// Step 2: display initial result; hide the volume ray-cast so the
	// island surface actors are unobscured.
	// ------------------------------------------------------------------
	applyIslandSegmentationResult(islands, labelImage);
	ui->volumeView->hideAllContent();
	setWorkflowStep(WorkflowStep::Segmented);

	// ------------------------------------------------------------------
	// Step 3: compute baseline stats to populate the iteration dialog.
	// ------------------------------------------------------------------
	const PrototypeHelpers::RegionStats baseStats =
		PrototypeHelpers::computeRegionStats(m_reslicedImage, labelImage);
	const double baseVolume =
		PrototypeHelpers::computeRegionVolumeMm3(labelImage);

	qDebug("onRegions: baseline region stats — mean=%d  stdDev=%d  volume=%.3f x 10^3 mm^3",
		   int(baseStats.mean), int(baseStats.stdDev), baseVolume * 1000);

	// ------------------------------------------------------------------
	// Step 4: open the non-modal iteration dialog.
	//
	// The dialog is heap-allocated and parented to this window so Qt
	// manages its lifetime.  WA_DeleteOnClose frees it when the user
	// closes it.  seeds is captured by value so it remains valid after
	// onRegions() returns.
	// ------------------------------------------------------------------
	auto* progressDlg = new IterationProgressDialog(
		m_threshold,
		baseStats.mean,
		baseStats.stdDev,
		baseVolume,
		m_reslicedImage->GetSpacing(),
		this);

	progressDlg->setIterateCallback(
		[this, seeds](double threshold) -> std::vector<PrototypeHelpers::BoneIsland>
		{
			const auto progress = [this](int pct)
				{
					showProgressValue(pct);
					m_progressBar->update();
					QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
				};

			showProgressStart();
			vtkSmartPointer<vtkImageData> iterLabel;
			auto iterIslands = PrototypeHelpers::segmentBoneIslands(
				m_reslicedImage, threshold, seeds, iterLabel, progress);
			showProgressEnd();

			if (!iterIslands.empty())
				applyIslandSegmentationResult(iterIslands, iterLabel);

			return iterIslands;
		});

	// Capture baseline islands and labelImage by value — onRegions() returns
	// before the dialog's Reset button can be clicked, so stack locals must
	// be copied into the closure.
	progressDlg->setResetCallback(
		[this, islands, labelImage]()
		{
			applyIslandSegmentationResult(islands, labelImage);
		});

	progressDlg->setAttribute(Qt::WA_DeleteOnClose);
	progressDlg->show();

	// onRegions() returns here.  The iteration loop runs inside progressDlg
	// when the user clicks Run.  The dialog and its callbacks remain valid
	// for the lifetime of this PrototypeMainWindow (its Qt parent).
}

// ---------------------------------------------------------------------------
// Morphological pipeline (smooth -> erode -> dilate -> connectivity)
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onRegionsAlt()
{
	// ------------------------------------------------------------------
	// Pre-conditions (identical to onRegions)
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		qWarning("onRegionsAlt: no resliced image available; run Reslice first.");
		return;
	}

	if (m_landmarkResult.isEmpty())
	{
		qWarning("onRegionsAlt: no landmark points available; run Landmark first.");
		return;
	}

	if (!std::isfinite(m_threshold))
	{
		qWarning("onRegionsAlt: threshold is not finite; cannot segment.");
		return;
	}

	// ------------------------------------------------------------------
	// Collect the 6 landmark world-space seed points from m_landmarkPoints.
	// Each of the 3 axes contributes one positive and one negative seed.
	// ------------------------------------------------------------------
	std::vector<std::array<double, 3>> seeds;
	seeds.reserve(6);

	for (int i = 0; i < 3; ++i)
	{
		for (int d = 0; d < 2; ++d)
		{
			const double* pt = m_landmarkPoints[static_cast<std::size_t>(i)]
				[static_cast<std::size_t>(d)].data();
			seeds.push_back({ pt[0], pt[1], pt[2] });
		}
	}

	qDebug("onRegionsAlt: running morphological pipeline with %zu seeds, threshold=%.4f",
		   seeds.size(), m_threshold);

	// ------------------------------------------------------------------
	// Progress callback
	// ------------------------------------------------------------------
showProgressStart();
	const auto regionProgress = [this](int percent)
		{
			showProgressValue(percent);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		};

	// ------------------------------------------------------------------
	// Run alternate segmentation (default smoothStdDev=1.0, morphKernelSize=1)
	// ------------------------------------------------------------------
	vtkSmartPointer<vtkImageData> labelImage;

	const std::vector<PrototypeHelpers::BoneIsland> islands =
		PrototypeHelpers::segmentBoneIslandsAlternate(
			m_reslicedImage,
			m_threshold,
			seeds,
			labelImage,
			1.0,  // smoothStdDev: Gaussian standard deviation in voxel units
			1,    // morphKernelSize: half-width ? 3x3x3 structuring element
			regionProgress);

	showProgressEnd();

	if (islands.empty())
	{
		qWarning("onRegionsAlt: no bone islands were found.");
		return;
	}

	applyIslandSegmentationResult(islands, labelImage);

	// Segmentation completed - advance to Segmented: no step buttons enabled.
	setWorkflowStep(WorkflowStep::Segmented);
}

void PrototypeMainWindow::onRegionsGraphCut()
{
	if (!m_reslicedImage || m_landmarkResult.isEmpty() || !std::isfinite(m_threshold))
	{
		qWarning("onRegionsGraphCut: pre-conditions not met "
				 "(reslice + landmark + finite threshold required).");
		return;
	}

	showProgressStart();
	const auto progress = [this](int pct)
		{
			showProgressValue(pct);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		};

	// ------------------------------------------------------------------
	// Build foreground and background seed images from landmark paths.
	//
	// Foreground: voxel paths from the bone centroid to each of the 5
	//             selected landmark surface points (Lpos excluded - that
	//             tip is adjacent to a neighbouring bone).
	// Background: threshold-gated outward rays from the same 5 landmark
	//             tips, stopping on re-entry into any bone-density tissue.
	// ------------------------------------------------------------------
	vtkSmartPointer<vtkImageData> fgSeedImage;
	vtkSmartPointer<vtkImageData> bgSeedImage;

	PrototypeHelpers::buildGraphCutSeedImages(
		m_reslicedImage,
		m_landmarkPoints,
		m_pca.axes,
		fgSeedImage,
		bgSeedImage,
		m_threshold);

	// ------------------------------------------------------------------
	// Debug visualisation: add seed point cloud actors to the renderer.
	// Green = foreground seeds, orange = background seeds.
	// Actors are tracked in m_graphCutSeedActors so onRestart() can
	// remove them when the workflow is reset.
	// ------------------------------------------------------------------
	vtkRenderer* ren = (ui && ui->volumeView) ? ui->volumeView->renderer() : nullptr;
	if (ren)
	{
		auto fgActor = PrototypeHelpers::makeSeedImageActor(fgSeedImage, 0.0, 1.0, 0.0, 4.0);
		auto bgActor = PrototypeHelpers::makeSeedImageActor(bgSeedImage, 1.0, 0.3, 0.0, 4.0);
		ren->AddActor(fgActor);
		ren->AddActor(bgActor);
		m_graphCutSeedActors.push_back(fgActor);
		m_graphCutSeedActors.push_back(bgActor);
		ui->volumeView->render();
	}

	// ------------------------------------------------------------------
	// Extract world-space coordinates of all seed voxels for
	// segmentBoneIslandsGraphCut.
	// ------------------------------------------------------------------
	const double* origin = m_reslicedImage->GetOrigin();
	const double* spacing = m_reslicedImage->GetSpacing();
	const int* dims = m_reslicedImage->GetDimensions();

	const vtkIdType totalVoxels =
		static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];

	std::vector<std::array<double, 3>> fgSeeds;
	std::vector<std::array<double, 3>> bgSeeds;
	fgSeeds.reserve(static_cast<std::size_t>(totalVoxels / 10));
	bgSeeds.reserve(static_cast<std::size_t>(totalVoxels / 10));

	auto* fgPtr = static_cast<unsigned char*>(fgSeedImage->GetScalarPointer());
	auto* bgPtr = static_cast<unsigned char*>(bgSeedImage->GetScalarPointer());

	for (vtkIdType k = 0; k < dims[2]; ++k)
		for (vtkIdType j = 0; j < dims[1]; ++j)
			for (vtkIdType i = 0; i < dims[0]; ++i)
			{
				const vtkIdType flat = k * dims[1] * dims[0] + j * dims[0] + i;
				const double wx = origin[0] + i * spacing[0];
				const double wy = origin[1] + j * spacing[1];
				const double wz = origin[2] + k * spacing[2];
				if (fgPtr[flat]) fgSeeds.push_back({ wx, wy, wz });
				if (bgPtr[flat]) bgSeeds.push_back({ wx, wy, wz });
			}

	qDebug("onRegionsGraphCut: %zu FG seed voxels, %zu BG seed voxels.",
		   fgSeeds.size(), bgSeeds.size());

	// ------------------------------------------------------------------
	// Run graph-cut segmentation
	// ------------------------------------------------------------------
	vtkSmartPointer<vtkImageData> labelImage;
	const std::vector<PrototypeHelpers::BoneIsland> islands =
		PrototypeHelpers::segmentBoneIslandsGraphCut(
			m_reslicedImage,
			m_threshold,
			fgSeeds,
			bgSeeds,
			labelImage,
			/*sigma=*/100.0,
			/*minIslandVoxels=*/50,
			progress);

	showProgressEnd();

	if (islands.empty())
	{
		qWarning("onRegionsGraphCut: no bone islands found.");
		return;
	}

	applyIslandSegmentationResult(islands, labelImage);
	setWorkflowStep(WorkflowStep::Segmented);
}

// ---------------------------------------------------------------------------
// Clean slot - post-segmentation clean step
//
// Algorithm:
//   1. Derive the noise distribution from the cached background statistics:
//      draw = clamp(backgroundMean + U(-2sig, +2sig),  volMin, volMax)
//      where U(-2sig, +2sig) is a uniform random value in [-2*bgStdDev, +2*bgStdDev].
//   2. Deep-copy the resliced volume into C.
//   3. For every voxel v in the resliced volume:
//        if v > threshold  AND  v is NOT inside any segmented island label:
//            replace the corresponding voxel in C with a drawn noise value.
//   4. Display C via setImage().
//
// Pre-conditions (all checked below):
//   - m_reslicedImage    : resliced volume exists
//   - m_labelImage       : segmentation label map exists (0 = background, ?1 = island)
//   - m_threshold        : finite threshold
//   - m_backgroundMean / m_backgroundStdDev : cached from setImage()
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onClean()
{
	qDebug("onClean: clean step triggered (resliceCount=%d).", m_resliceCount);

	// ------------------------------------------------------------------
	// Pre-conditions
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		qWarning("onClean: no resliced image available; run Reslice first.");
		return;
	}

	if (!m_labelImage)
	{
		qWarning("onClean: no label image available; run Regions or Regions Alt first.");
		return;
	}

	if (!std::isfinite(m_threshold))
	{
		qWarning("onClean: threshold is not finite; cannot clean.");
		return;
	}

	// ------------------------------------------------------------------
	// Scalar range of the resliced volume - used to clamp random draws
	// ------------------------------------------------------------------

	const double scalarRange[2] = {
	m_imageStats.value(QStringLiteral("min")).toDouble(0.0),
	m_imageStats.value(QStringLiteral("max")).toDouble(255.0) };
	const double volMin = scalarRange[0];
	const double volMax = scalarRange[1];

	// ------------------------------------------------------------------
	// Background noise distribution parameters sourced from m_imageStats,
	// which is populated by setImage() via computeScalarThresholdStats().
	// "meanBg"   - mean scalar value of all below-threshold voxels.
	// "stdDevBg" - standard deviation of below-threshold voxels.
	// ------------------------------------------------------------------
	const double bgMean = m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);
	const double bgStdDev = m_imageStats.value(QStringLiteral("stdDevBg")).toDouble(0.0);
	const double noiseHalfWidth = 2.0 * bgStdDev;   // +/- 2? uniform range

	qDebug("onClean: bgMean=%.4f  bgStdDev=%.4f  noiseHalfWidth=%.4f  "
		   "volMin=%.4f  volMax=%.4f  threshold=%.4f",
		   bgMean, bgStdDev, noiseHalfWidth, volMin, volMax, m_threshold);

	// ------------------------------------------------------------------
	// Initialize C++17 random number generator
	// uniform_real_distribution over [-noiseHalfWidth, +noiseHalfWidth]
	// ------------------------------------------------------------------
	std::mt19937_64                          rng{ std::random_device{}() };
	std::uniform_real_distribution<double>   noiseDist(-noiseHalfWidth, noiseHalfWidth);

	// ------------------------------------------------------------------
	// Deep-copy the resliced volume into C (the output cleaned volume)
	// ------------------------------------------------------------------
	auto cleanedImage = vtkSmartPointer<vtkImageData>::New();
	cleanedImage->DeepCopy(m_reslicedImage);

	vtkDataArray* reslicedScalars = m_reslicedImage->GetPointData()->GetScalars();
	vtkDataArray* labelScalars = m_labelImage->GetPointData()->GetScalars();
	vtkDataArray* cleanedScalars = cleanedImage->GetPointData()->GetScalars();

	if (!reslicedScalars || !labelScalars || !cleanedScalars)
	{
		qWarning("onClean: scalar arrays missing on resliced, label, or cleaned image.");
		return;
	}

	const vtkIdType nReslicedPoints = m_reslicedImage->GetNumberOfPoints();
	const vtkIdType nLabelPoints = m_labelImage->GetNumberOfPoints();

	// The label image is produced from the resliced image so their point
	// counts must match.  Guard against an unexpected mismatch.
	if (nReslicedPoints != nLabelPoints)
	{
		qWarning("onClean: resliced image point count (%lld) does not match "
				 "label image point count (%lld); aborting.",
				 static_cast<long long>(nReslicedPoints),
				 static_cast<long long>(nLabelPoints));
		showProgressEnd();
		return;
	}

	showProgressStart();

	// ------------------------------------------------------------------
	// Voxel loop  [progress 0 ? 80]
	//
	// For each voxel i:
	//   - scalar v  = resliced intensity
	//   - label  l  = segmentation label (0 = unlabelled / outside all islands)
	//
	// Replace in C when:
	//   v > threshold   (foreground intensity)
	//   AND l == 0      (not inside any segmented island)
	// ------------------------------------------------------------------
	vtkIdType replacedCount = 0;

	for (vtkIdType i = 0; i < nReslicedPoints; ++i)
	{
		// Progress: report every 64 K voxels, mapped to the [0, 80] sub-range.
		if ((i & 0xFFFF) == 0)
		{
			const int pct = static_cast<int>(
				std::clamp(static_cast<double>(i) / static_cast<double>(nReslicedPoints), 0.0, 1.0) * 80.0);
			showProgressValue(pct);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		}

		const double v = reslicedScalars->GetTuple1(i);
		const double l = labelScalars->GetTuple1(i);

		// Keep voxels that are below the threshold (background) or inside an island.
		if (v <= m_threshold || l != 0.0)
			continue;

		// Voxel is above-threshold AND outside every segmented island -
		// replace it with a background-noise draw clamped to [volMin, volMax].
		const double noise = noiseDist(rng);
		const double noiseVal = std::clamp(bgMean + noise, volMin, volMax);
		cleanedScalars->SetTuple1(i, noiseVal);
		++replacedCount;
	}

	cleanedScalars->Modified();
	cleanedImage->Modified();

	qDebug("onClean: replaced %lld above-threshold outside-island voxels with background noise.",
		   static_cast<long long>(replacedCount));

	// ------------------------------------------------------------------
	// VOI crop: tighten the cleaned volume to the region of interest.
	//
	// Steps:
	//   1. Compute Dmin = half the shortest inter-landmark paired distance
	//      (pos-neg pair along the same axis) in world coordinates.
	//   2. Build a vtkBoundingBox B over all segmented island voxels in
	//      world coordinates (label > 0 in m_labelImage).
	//   3. Inflate B by Dmin uniformly from its centroid.
	//   4. Map B to voxel extent in cleanedImage and apply vtkExtractVOI.
	// ------------------------------------------------------------------

	// Step 1 - Dmin: half the shortest paired landmark distance.
	// m_landmarkPoints[axis][0] = positive direction surface point
	// m_landmarkPoints[axis][1] = negative direction surface point
	double minPairedDist = std::numeric_limits<double>::max();
	for (int axis = 0; axis < 3; ++axis)
	{
		const double* lPos = m_landmarkPoints[static_cast<std::size_t>(axis)][0].data();
		const double* lNeg = m_landmarkPoints[static_cast<std::size_t>(axis)][1].data();
		const double dx = lPos[0] - lNeg[0];
		const double dy = lPos[1] - lNeg[1];
		const double dz = lPos[2] - lNeg[2];
		const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
		minPairedDist = std::min(minPairedDist, dist);
	}

	const double dMin = 0.5 * minPairedDist;

	qDebug("onClean: minPairedLandmarkDist=%.4f  dMin=%.4f", minPairedDist, dMin);

	// Step 2 - island bounding box in world coordinates  [progress 80 ? 95]
	// Iterate the label image; accumulate world positions of all labelled voxels.
	showProgressValue(80);
	m_progressBar->update();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	vtkBoundingBox islandBB;
	for (const auto& island : m_islands)
	{
		const QJsonArray bbMinArr = island.json.value(QStringLiteral("bbMin")).toArray();
		const QJsonArray bbMaxArr = island.json.value(QStringLiteral("bbMax")).toArray();

		if (bbMinArr.size() < 3 || bbMaxArr.size() < 3)
		{
			qWarning("onClean: island label=%d has malformed bbMin/bbMax in JSON; skipped.",
					 island.label);
			continue;
		}

		const double bMin[3] = {
			bbMinArr[0].toDouble(),
			bbMinArr[1].toDouble(),
			bbMinArr[2].toDouble()
		};
		const double bMax[3] = {
			bbMaxArr[0].toDouble(),
			bbMaxArr[1].toDouble(),
			bbMaxArr[2].toDouble()
		};

		// Build a per-island box and union it into the global accumulator.
		vtkBoundingBox islandBox;
		islandBox.SetMinPoint(bMin[0], bMin[1], bMin[2]);
		islandBox.SetMaxPoint(bMax[0], bMax[1], bMax[2]);
		islandBB.AddBox(islandBox);

		qDebug("onClean: island label=%d  bbMin=(%.3f,%.3f,%.3f)  bbMax=(%.3f,%.3f,%.3f)",
			   island.label,
			   bMin[0], bMin[1], bMin[2],
			   bMax[0], bMax[1], bMax[2]);
	}

	if (!islandBB.IsValid())
	{
		qWarning("onClean: global island bounding box is invalid "
				 "(no islands with valid bbMin/bbMax); aborting VOI crop.");
		showProgressEnd();
		return;
	}

	islandBB.Inflate(dMin);

	// Step 4 - map world BB corners to continuous voxel indices and round to nearest integer.
	// vtkExtractVOI::SetVOI takes absolute extent indices (not dimension-relative),
	// so we clamp to the image's actual extent [ext[0],ext[1]] x [ext[2],ext[3]] x [ext[4],ext[5]].
	showProgressValue(95);
	m_progressBar->update();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	double voxelIdxMin[3];
	double voxelIdxMax[3];

	double worldPtMin[3];
	double worldPtMax[3];
	islandBB.GetMinPoint(worldPtMin);
	islandBB.GetMaxPoint(worldPtMax);

	cleanedImage->TransformPhysicalPointToContinuousIndex(worldPtMin, voxelIdxMin);
	cleanedImage->TransformPhysicalPointToContinuousIndex(worldPtMax, voxelIdxMax);

	int extent[6];
	cleanedImage->GetExtent(extent);  // absolute extent: [xMin,xMax, yMin,yMax, zMin,zMax]

	const int voiXMin = std::clamp(static_cast<int>(std::lround(voxelIdxMin[0])), extent[0], extent[1]);
	const int voiXMax = std::clamp(static_cast<int>(std::lround(voxelIdxMax[0])), extent[0], extent[1]);
	const int voiYMin = std::clamp(static_cast<int>(std::lround(voxelIdxMin[1])), extent[2], extent[3]);
	const int voiYMax = std::clamp(static_cast<int>(std::lround(voxelIdxMax[1])), extent[2], extent[3]);
	const int voiZMin = std::clamp(static_cast<int>(std::lround(voxelIdxMin[2])), extent[4], extent[5]);
	const int voiZMax = std::clamp(static_cast<int>(std::lround(voxelIdxMax[2])), extent[4], extent[5]);

	qDebug("onClean: VOI voxel extent  x=[%d,%d]  y=[%d,%d]  z=[%d,%d]",
		   voiXMin, voiXMax, voiYMin, voiYMax, voiZMin, voiZMax);

	auto extractVOI = vtkSmartPointer<vtkExtractVOI>::New();
	extractVOI->SetInputData(cleanedImage);
	extractVOI->SetVOI(voiXMin, voiXMax, voiYMin, voiYMax, voiZMin, voiZMax);
	extractVOI->SetSampleRate(1, 1, 1);
	extractVOI->Update();

	vtkImageData* cropped = extractVOI->GetOutput();
	if (cropped && cropped->GetNumberOfPoints() > 0)
	{
		auto croppedCopy = vtkSmartPointer<vtkImageData>::New();
		croppedCopy->DeepCopy(cropped);
		cleanedImage = croppedCopy;

		const int* croppedDims = cleanedImage->GetDimensions();
		qDebug("onClean: VOI crop applied - new dims: %d x %d x %d",
			   croppedDims[0], croppedDims[1], croppedDims[2]);
	}
	else
	{
		qWarning("onClean: vtkExtractVOI produced empty output; skipping crop.");
	}


	showProgressEnd();

	// ------------------------------------------------------------------
	// Cache the cleaned volume and update the display.
	// setImage() is NOT called here to avoid resetting the workflow state
	// and recomputing PCA.  Instead we update only the VolumeView directly,
	// mirroring the minimal path used elsewhere when the image content
	// changes without replacing the pipeline source.
	// ------------------------------------------------------------------
	m_reslicedImage = cleanedImage;
	m_image = m_reslicedImage.Get();

	ui->volumeView->setImageData(m_reslicedImage);
	ui->volumeView->updateData();

	const double level = std::isfinite(m_threshold)
		? m_threshold
		: 0.5 * (scalarRange[0] + scalarRange[1]);

	const double window = 2.0 * m_imageStats.value(QStringLiteral("stdDev")).toDouble(1.0);

	ui->volumeView->setColorWindowLevel(window, level);
	syncSliceView(m_reslicedImage, window, level);

	ui->volumeView->renderer()->ResetCamera();
	ui->volumeView->renderer()->ResetCameraClippingRange();
	ui->volumeView->render();

	// Clean completed - advance to Cleaned: all step buttons disabled.
	setWorkflowStep(WorkflowStep::Cleaned);
}

// ---------------------------------------------------------------------------
// Restart slot - revert to the original image and reset the full workflow
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onRestart()
{
	if (!m_originalImage)
	{
		qDebug("onRestart: no original image cached; nothing to restore.");
		return;
	}

	qDebug("onRestart: reverting to original image and resetting workflow.");

	// Discard all derived state so setImage() treats this as a clean first load.
	// Clear island actors and scalar bar before releasing the label image.
	clearIslandActors();
	m_labelImage = nullptr;
	m_reslicedImage = nullptr;
	m_islands.clear();
	m_reslicedPcaJson = QJsonObject{};
	m_landmarkResult = QJsonObject{};
	m_landmarkJson = QJsonObject{};
	m_landmarkPoints = {};
	m_imageStats = QJsonObject{};

	// Reset the reslice counter so the Clean gate starts from zero on restart.
	m_resliceCount = 0;

	// Restore the view to the original image with its original PCA axes.
	// setImage() will recompute the PCA from the original image data and
	// rebuild all PCA overlay actors, which gives back the default axes.
	setImage(m_originalImage);

	// Ensure m_originalPcaJson is refreshed from the just-computed PCA
	// (setImage only writes it when m_reslicedImage is null, which it now is).
	if (m_pca.valid)
	{
		m_originalPcaJson = pcaResultToJson(m_pca);
		qDebug("onRestart: original PCA JSON refreshed.");
	}

	// Reset the workflow to the start: only Reslice enabled.
	setWorkflowStep(WorkflowStep::Idle);
}

// ---------------------------------------------------------------------------
// Async load
// ---------------------------------------------------------------------------

void PrototypeMainWindow::loadFromSidecarAsync(const QString& sidecarPath)
{
	QTimer::singleShot(0, this, [this, sidecarPath]()
	{
		loadFromSidecar(sidecarPath);
	});
}

// ---------------------------------------------------------------------------
// Export Reslice slot
//
// Pipeline:
//   1. onReslice()   - PCA-aligned reslice of the current volume.
//   2. onLandmark()  - Locate surface landmark points on the resliced volume.
//   3. Threshold     - Identify bone voxels (scalar > m_threshold).
//   4. Padded BB     - Build a bounding box over bone voxels, inflated by
//                      dMin = 0.5 * shortest inter-landmark paired distance.
//   5. vtkExtractVOI - Crop to the padded bounding box.
//   6. NIfTI export  - Write the cropped volume to an auto-generated path:
//                        <sidecar_dir>/<crop_basename>_reslice_export.nii
//
// Pre-conditions:
//   - A valid PCA result (m_pca.valid) and cached image (m_image) must exist.
//   - m_threshold must be finite.
// ---------------------------------------------------------------------------

void PrototypeMainWindow::onExportReslice()
{
	// ------------------------------------------------------------------
	// Pre-conditions
	// ------------------------------------------------------------------
	if (!m_pca.valid || !m_image)
	{
		QMessageBox::warning(this, tr("Export Reslice"),
			tr("Load an image with a valid PCA result before exporting."));
		return;
	}

	if (!std::isfinite(m_threshold))
	{
		QMessageBox::warning(this, tr("Export Reslice"),
			tr("A finite threshold is required to perform the export.\n"
			"Open a project sidecar that contains a threshold value."));
		return;
	}

	// ------------------------------------------------------------------
	// Step 1: Reslice
	// ------------------------------------------------------------------
	onReslice();

	if (!m_reslicedImage)
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("Reslice step failed; export aborted."));
		return;
	}

	// ------------------------------------------------------------------
	// Step 2: Landmark
	// ------------------------------------------------------------------
	onLandmark();

	// m_landmarkPoints is populated by onLandmark(); validate it.
	{
		bool hasPoints = false;
		for (int ax = 0; ax < 3; ++ax)
			for (int d = 0; d < 2; ++d)
				if (m_landmarkPoints[static_cast<std::size_t>(ax)]
					[static_cast<std::size_t>(d)][0] != 0.0 ||
					m_landmarkPoints[static_cast<std::size_t>(ax)]
					[static_cast<std::size_t>(d)][1] != 0.0 ||
					m_landmarkPoints[static_cast<std::size_t>(ax)]
					[static_cast<std::size_t>(d)][2] != 0.0)
				{
					hasPoints = true;
					break;
				}

		if (!hasPoints)
		{
			QMessageBox::critical(this, tr("Export Reslice"),
				tr("Landmark step did not produce valid surface points; export aborted."));
			return;
		}
	}

	// ------------------------------------------------------------------
	// Step 3: Build a bone-voxel bounding box from the resliced image.
	//
	// Iterate every voxel in m_reslicedImage; accumulate world-space
	// positions of all voxels with scalar > m_threshold.
	// ------------------------------------------------------------------
	showProgressStart();
	showProgressValue(5);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	vtkDataArray* scalars = m_reslicedImage->GetPointData()->GetScalars();
	if (!scalars)
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("Resliced image has no scalar data; export aborted."));
		showProgressEnd();
		return;
	}

	const double* origin = m_reslicedImage->GetOrigin();
	const double* spacing = m_reslicedImage->GetSpacing();
	const int* dims = m_reslicedImage->GetDimensions();

	vtkBoundingBox boneBB;
	const vtkIdType totalVoxels =
		static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];

	for (vtkIdType idx = 0; idx < totalVoxels; ++idx)
	{
		if ((idx & 0xFFFF) == 0)
		{
			const int pct = static_cast<int>(
				std::clamp(static_cast<double>(idx) /
				static_cast<double>(totalVoxels), 0.0, 1.0) * 40.0) + 5;
			showProgressValue(pct);
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		}

		if (scalars->GetTuple1(idx) <= m_threshold)
			continue;

		// Flat index -> (i, j, k)
		const vtkIdType i = idx % dims[0];
		const vtkIdType j = (idx / dims[0]) % dims[1];
		const vtkIdType k = idx / (static_cast<vtkIdType>(dims[0]) * dims[1]);

		const double wx = origin[0] + i * spacing[0];
		const double wy = origin[1] + j * spacing[1];
		const double wz = origin[2] + k * spacing[2];

		boneBB.AddPoint(wx, wy, wz);
	}

	if (!boneBB.IsValid())
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("No above-threshold (bone) voxels found in the resliced volume; "
			"export aborted."));
		showProgressEnd();
		return;
	}

	// ------------------------------------------------------------------
	// Step 4: Compute dMin = 0.5 * shortest paired landmark distance,
	//         then inflate the bounding box uniformly.
	// ------------------------------------------------------------------
	double minPairedDist = std::numeric_limits<double>::max();
	for (int ax = 0; ax < 3; ++ax)
	{
		const double* lPos = m_landmarkPoints[static_cast<std::size_t>(ax)][0].data();
		const double* lNeg = m_landmarkPoints[static_cast<std::size_t>(ax)][1].data();
		const double dx = lPos[0] - lNeg[0];
		const double dy = lPos[1] - lNeg[1];
		const double dz = lPos[2] - lNeg[2];
		minPairedDist = std::min(minPairedDist, std::sqrt(dx * dx + dy * dy + dz * dz));
	}

	const double dMin = 0.5 * minPairedDist;
	boneBB.Inflate(dMin);

	qDebug("onExportReslice: bone BB inflated by dMin=%.4f", dMin);

	// ------------------------------------------------------------------
	// Step 5: Map world BB to voxel extent and run vtkExtractVOI
	// ------------------------------------------------------------------
	showProgressValue(50);
	m_progressBar->update();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	double worldMin[3], worldMax[3];
	boneBB.GetMinPoint(worldMin);
	boneBB.GetMaxPoint(worldMax);

	double voxelIdxMin[3], voxelIdxMax[3];
	m_reslicedImage->TransformPhysicalPointToContinuousIndex(worldMin, voxelIdxMin);
	m_reslicedImage->TransformPhysicalPointToContinuousIndex(worldMax, voxelIdxMax);

	int extent[6];
	m_reslicedImage->GetExtent(extent);

	const int voiXMin = std::clamp(static_cast<int>(std::lround(voxelIdxMin[0])), extent[0], extent[1]);
	const int voiXMax = std::clamp(static_cast<int>(std::lround(voxelIdxMax[0])), extent[0], extent[1]);
	const int voiYMin = std::clamp(static_cast<int>(std::lround(voxelIdxMin[1])), extent[2], extent[3]);
	const int voiYMax = std::clamp(static_cast<int>(std::lround(voxelIdxMax[1])), extent[2], extent[3]);
	const int voiZMin = std::clamp(static_cast<int>(std::lround(voxelIdxMin[2])), extent[4], extent[5]);
	const int voiZMax = std::clamp(static_cast<int>(std::lround(voxelIdxMax[2])), extent[4], extent[5]);

	qDebug("onExportReslice: VOI extent  x=[%d,%d]  y=[%d,%d]  z=[%d,%d]",
		voiXMin, voiXMax, voiYMin, voiYMax, voiZMin, voiZMax);

	auto extractVOI = vtkSmartPointer<vtkExtractVOI>::New();
	extractVOI->SetInputData(m_reslicedImage);
	extractVOI->SetVOI(voiXMin, voiXMax, voiYMin, voiYMax, voiZMin, voiZMax);
	extractVOI->SetSampleRate(1, 1, 1);
	extractVOI->Update();

	vtkImageData* cropped = extractVOI->GetOutput();
	if (!cropped || cropped->GetNumberOfPoints() == 0)
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("VOI crop produced an empty volume; export aborted."));
		showProgressEnd();
		return;
	}

	const int* croppedDims = cropped->GetDimensions();
	qDebug("onExportReslice: cropped dims: %d x %d x %d",
		croppedDims[0], croppedDims[1], croppedDims[2]);

	// ------------------------------------------------------------------
	// Step 6: Auto-generate output path and write NIfTI
	//
	// Pattern: <sidecar_dir>/<crop_basename>_reslice_export.nii
	// Fallback (no sidecar): <temp_dir>/reslice_export.nii
	// ------------------------------------------------------------------
	showProgressValue(80);
	m_progressBar->update();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	QString outputPath;
	if (!m_sidecarPath.isEmpty() && !m_cropPath.isEmpty())
	{
		const QString cropBase = QFileInfo(m_cropPath).completeBaseName();
		const QString sidecarDir = QFileInfo(m_sidecarPath).absolutePath();
		outputPath = QDir(sidecarDir).filePath(cropBase + QStringLiteral("_reslice_export.nii"));
	}
	else
	{
		outputPath = QDir::temp().filePath(QStringLiteral("reslice_export.nii"));
	}

	auto writer = vtkSmartPointer<vtkNIFTIImageWriter>::New();
	writer->SetInputData(cropped);
	writer->SetFileName(outputPath.toUtf8().constData());
	writer->Write();

	showProgressEnd();

	qDebug("onExportReslice: wrote NIfTI to '%s'", qUtf8Printable(outputPath));

	statusBar()->showMessage(
		tr("Export Reslice: saved to %1").arg(outputPath), 6000);

	QMessageBox::information(this, tr("Export Reslice"),
		tr("Resliced and cropped volume saved to:\n%1").arg(outputPath));
}

// ---------------------------------------------------------------------------
// syncSliceView
//
// Pushes the current image and window/level to the companion SliceView so it
// always shows an XY slice of whatever volume the VolumeView is displaying.
// Called from setImage() and onClean() — any path that changes m_image.
// ---------------------------------------------------------------------------

void PrototypeMainWindow::syncSliceView(vtkImageData* image, double window, double level)
{
	if (!ui || !ui->sliceView || !ui->volumeView)
		return;

	ui->sliceView->setImageData(image);
	ui->sliceView->updateData();

	if (image)
	{
		ui->sliceView->setWindowLevelNative(window, level);
	}
}

// ---------------------------------------------------------------------------
// alignCameraToMediumAxis
//
// Realigns the VolumeView camera so that:
//   - The view-up vector points along the medium PCA axis (axis index 1,
//     positive direction), derived from m_pca.axes[1].
//   - The view direction points along the small PCA axis positive-to-negative
//     direction (axis index 2, negated), derived from m_pca.axes[2].
//   - The focal point is set to the PCA centroid.
//   - The camera-to-focal distance is preserved from the current camera.
//
// Uses m_pca directly so it can be called as soon as setImage() has finished
// computing the PCA, before onReslice() or onLandmark() have run.
// ---------------------------------------------------------------------------

void PrototypeMainWindow::alignCameraToMediumAxis()
{
	if (!m_pca.valid || !ui || !ui->volumeView)
		return;

	vtkRenderer* ren = ui->volumeView->renderer();
	if (!ren)
		return;

	vtkCamera* cam = ren->GetActiveCamera();
	if (!cam)
		return;

	// ------------------------------------------------------------------
	// View-up: medium PCA axis positive direction (axis index 1).
	// m_pca.axes[1] is already a unit vector; use it directly.
	// ------------------------------------------------------------------
	const double* viewUp = m_pca.axes[1];

	// ------------------------------------------------------------------
	// View direction: small PCA axis positive -> negative (axis index 2
	// negated).  The camera looks along -axes[2] so the small axis points
	// "into" the screen.
	// ------------------------------------------------------------------
	const double viewDir[3] = {
		-m_pca.axes[2][0],
		-m_pca.axes[2][1],
		-m_pca.axes[2][2]
	};

	// ------------------------------------------------------------------
	// Focal point: PCA centroid.
	// Distance: preserved from the current camera so the zoom level is
	// unchanged across successive calls (e.g. Reslice -> Landmark).
	// ------------------------------------------------------------------
	const double* c = m_pca.centroid;
	const double  dist = cam->GetDistance();

	// New camera position: step back from the centroid along -viewDir.
	const double camPos[3] = {
		c[0] - dist * viewDir[0],
		c[1] - dist * viewDir[1],
		c[2] - dist * viewDir[2]
	};

	cam->SetFocalPoint(c[0], c[1], c[2]);
	cam->SetPosition(camPos[0], camPos[1], camPos[2]);
	cam->SetViewUp(viewUp[0], viewUp[1], viewUp[2]);

	ren->ResetCameraClippingRange();
	ui->volumeView->render();

	qDebug("alignCameraToMediumAxis: "
		"viewDir=(%.4f,%.4f,%.4f)  viewUp=(%.4f,%.4f,%.4f)  "
		"focalPt=(%.4f,%.4f,%.4f)  dist=%.4f",
		viewDir[0], viewDir[1], viewDir[2],
		viewUp[0], viewUp[1], viewUp[2],
		c[0], c[1], c[2],
		dist);
}

// ---------------------------------------------------------------------------
// applyIslandRetentionFilter
//
// Shows actors whose island label is in retainedLabels and hides all others.
// The m_islands vector order matches m_islandActors order (both built in
// applyIslandSegmentationResult), so they are iterated together by index.
// ---------------------------------------------------------------------------
void PrototypeMainWindow::applyIslandRetentionFilter(const QSet<int>& retainedLabels)
{
	for (std::size_t i = 0; i < m_islandActors.size() && i < m_islands.size(); ++i)
	{
		if (!m_islandActors[i])
			continue;

		const bool retain = retainedLabels.contains(m_islands[i].label);
		m_islandActors[i]->SetVisibility(retain ? 1 : 0);

		qDebug("applyIslandRetentionFilter: island label=%d  retain=%s",
			   m_islands[i].label, retain ? "yes" : "no");
	}

	if (ui && ui->volumeView)
		ui->volumeView->render();
}
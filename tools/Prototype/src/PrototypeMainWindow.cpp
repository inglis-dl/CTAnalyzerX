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
#include <vtkImageContinuousDilate3D.h>
#include <vtkImageData.h>
#include <vtkImageReslice.h>
#include <vtkMath.h>
#include <vtkMatrix3x3.h>
#include <vtkMatrix4x4.h>
#include <vtkNIFTIImageWriter.h>
#include <vtkPointData.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>
#include <vtkTextProperty.h>
#include <vtkTransform.h>

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
#include <QLegendMarker>
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
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#define PARALLEL_ISLANDS 1  // set to 0 to disable concurrent iteration of islands

QT_CHARTS_USE_NAMESPACE   // expands to: using namespace QtCharts; (Qt5 only)

namespace {

	// ---------------------------------------------------------------------------
// IslandCleanDialog
//
// Modal dialog presented by onClean().  One row per segmented island shows:
//   Col 0  Coloured swatch + "Island N" label, matched to the VolumeView
//          legend via makeIslandColorTF.
//   Col 1  Volume in ×10³ mm³.
//   Col 2  Remove checkbox.
//
// Default selection: largest island unchecked (retained); all others checked.
// A spin box controls the number of 3×3×3 dilation passes applied to the
// binary island mask before its voxels are replaced with background noise.
// ---------------------------------------------------------------------------
	class IslandCleanDialog : public QDialog
	{
	public:
		explicit IslandCleanDialog(
			const std::vector<PrototypeHelpers::BoneIsland>& islands,
			const double                                      voxelSpacing[3],
			QWidget* parent = nullptr)
			: QDialog(parent)
		{
			setWindowTitle(tr("Clean Islands"));
			setMinimumWidth(480);

			const double voxelVol =
				voxelSpacing[0] * voxelSpacing[1] * voxelSpacing[2];

			// Build the colour TF over the current island voxel-count range,
			// matching the VolumeView scalar bar exactly.
			vtkIdType minVox = islands[0].voxelCount;
			vtkIdType maxVox = islands[0].voxelCount;
			for (const auto& isl : islands)
			{
				minVox = std::min(minVox, isl.voxelCount);
				maxVox = std::max(maxVox, isl.voxelCount);
			}
			auto colorTF = PrototypeHelpers::makeIslandColorTF(
				static_cast<double>(minVox),
				static_cast<double>(maxVox));

			// Largest island is retained by default.
			const int largestLabel =
				std::max_element(islands.begin(), islands.end(),
					[](const PrototypeHelpers::BoneIsland& a,
				const PrototypeHelpers::BoneIsland& b)
					{ return a.voxelCount < b.voxelCount; })->label;

			// ── Island table ──────────────────────────────────────────────────
			auto* table = new QTableWidget(
				static_cast<int>(islands.size()), 3, this);
			table->setHorizontalHeaderLabels(
				{ tr("Island"),
				  tr("Volume (\u00D710\u207B\u00B3 mm\u00B3)"),
				  tr("Remove") });
			table->setSelectionMode(QAbstractItemView::NoSelection);
			table->setEditTriggers(QAbstractItemView::NoEditTriggers);
			table->verticalHeader()->setVisible(false);
			table->horizontalHeader()->setSectionResizeMode(
				0, QHeaderView::Fixed);
			table->horizontalHeader()->setSectionResizeMode(
				1, QHeaderView::Stretch);
			table->horizontalHeader()->setSectionResizeMode(
				2, QHeaderView::Fixed);
			table->setColumnWidth(0, 90);
			table->setColumnWidth(2, 72);

			for (int row = 0; row < static_cast<int>(islands.size()); ++row)
			{
				const auto& isl = islands[static_cast<std::size_t>(row)];

				// Sample the same TF used by the VolumeView legend.
				double rgb[3] = { 1.0, 1.0, 1.0 };
				colorTF->GetColor(static_cast<double>(isl.voxelCount), rgb);

				const QColor bgColor(
					static_cast<int>(rgb[0] * 255.0),
					static_cast<int>(rgb[1] * 255.0),
					static_cast<int>(rgb[2] * 255.0));

				// Perceived luminance — choose black or white text.
				const double lum =
					0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
				const QColor fgColor = (lum > 0.5) ? Qt::black : Qt::white;

				// Col 0: coloured swatch with island label text.
				auto* swatchItem = new QTableWidgetItem(
					tr("Island %1").arg(isl.label));
				swatchItem->setTextAlignment(Qt::AlignCenter);
				swatchItem->setBackground(QBrush(bgColor));
				swatchItem->setForeground(QBrush(fgColor));
				table->setItem(row, 0, swatchItem);

				// Col 1: volume (right-aligned).
				const double volMm3x1k =
					static_cast<double>(isl.voxelCount) * voxelVol * 1000.0;
				auto* volItem = new QTableWidgetItem(
					QString::number(volMm3x1k, 'g', 4));
				volItem->setTextAlignment(Qt::AlignVCenter | Qt::AlignRight);
				table->setItem(row, 1, volItem);

				// Col 2: Remove checkbox, centred in a cell widget.
				auto* chkWidget = new QWidget(table);
				auto* chk = new QCheckBox(chkWidget);
				chk->setChecked(isl.label != largestLabel);

				auto* chkLayout = new QHBoxLayout(chkWidget);
				chkLayout->addWidget(chk);
				chkLayout->setAlignment(Qt::AlignCenter);
				chkLayout->setContentsMargins(0, 0, 0, 0);
				table->setCellWidget(row, 2, chkWidget);

				m_checkboxes.push_back({ isl.label, chk });
			}

			// ── Dilation passes control ────────────────────────────────────────
			m_spinDilations = new QSpinBox(this);
			m_spinDilations->setRange(1, 20);
			m_spinDilations->setValue(1);
			m_spinDilations->setToolTip(
				tr("Number of 3\u00D73\u00D73 morphological dilation passes applied\n"
				"to the island mask before voxels are replaced with noise.\n"
				"Higher values widen the removed boundary around each island."));

			auto* form = new QFormLayout;
			// Orphan foreground checkbox — removes above-threshold voxels that
			// are not connected to any landmark seed point (label == 0 in the
			// segmentation output).
			m_chkOrphans = new QCheckBox(
				tr("Remove unlabeled foreground regions"), this);
			m_chkOrphans->setChecked(true);
			m_chkOrphans->setToolTip(
				tr("Also replace above-threshold voxels that are not connected\n"
				"to any landmark seed point (not part of any labeled island).\n"
				"These are bone fragments or adjacent structures that lie\n"
				"outside the seeded segmentation."));

			form->addRow(tr("Dilation passes:"), m_spinDilations);
			form->addRow(QString(), m_chkOrphans);

			// ── Buttons ───────────────────────────────────────────────────────
			auto* buttons = new QDialogButtonBox(
				QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
			connect(buttons, &QDialogButtonBox::accepted,
				this, &QDialog::accept);
			connect(buttons, &QDialogButtonBox::rejected,
				this, &QDialog::reject);

			// ── Root layout ───────────────────────────────────────────────────
			auto* root = new QVBoxLayout(this);
			root->addWidget(table, 1);
			root->addLayout(form);
			root->addWidget(buttons);
		}

		// Returns the labels whose Remove checkbox is checked.
		std::vector<int> labelsToRemove() const
		{
			std::vector<int> result;
			for (const auto& [label, chk] : m_checkboxes)
				if (chk && chk->isChecked())
					result.push_back(label);
			return result;
		}

		int dilationCount() const { return m_spinDilations->value(); }

		bool removeOrphanForeground() const
		{
			return m_chkOrphans && m_chkOrphans->isChecked();
		}

	private:
		QSpinBox* m_spinDilations = nullptr;
		QCheckBox* m_chkOrphans = nullptr;
		std::vector<std::pair<int, QCheckBox*>> m_checkboxes;
	};


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
			std::vector<PrototypeHelpers::BoneIsland>(double threshold, bool firstIteration)
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
			setupForm->addRow(tr("Region volume (\u00D710\u207B\u00B3 mm\u00B3):"),
				new QLabel(QString::number(regionVolumeMm3 * 1000.0, 'f', 1), this));

			m_spinIterations = new QSpinBox(this);
			m_spinIterations->setRange(2, 100);
			m_spinIterations->setValue(20);
			setupForm->addRow(tr("Max Iterations:"), m_spinIterations);

			m_spinTargetIslands = new QSpinBox(this);
			m_spinTargetIslands->setRange(2, 255);
			m_spinTargetIslands->setValue(2);
			setupForm->addRow(tr("Target Islands:"), m_spinTargetIslands);

			// Volume threshold: displayed in the same ×10³ mm³ units as the
			// iteration table.  Initial value 1.0 represents 0.001 mm³ × 1000.
			// The run stops when the absolute volume change between the last
			// two appended rows falls at or below this value.
			m_spinVolumeThreshold = new QDoubleSpinBox(this);
			m_spinVolumeThreshold->setRange(0.0, 1000.0);
			m_spinVolumeThreshold->setSingleStep(0.1);
			m_spinVolumeThreshold->setDecimals(3);
			m_spinVolumeThreshold->setValue(1.0);
			setupForm->addRow(tr("Volume threshold (\u00D710\u207B\u00B3 mm\u00B3):"),
				m_spinVolumeThreshold);

			m_spinMultiplier = new QDoubleSpinBox(this);
			m_spinMultiplier->setRange(0.01, 10.0);
			m_spinMultiplier->setSingleStep(0.01);
			m_spinMultiplier->setDecimals(3);
			m_spinMultiplier->setValue(0.1);
			setupForm->addRow(tr("Std-dev multiplier:"), m_spinMultiplier);

			// Read-only label: multiplier × stdDev updated live as the spin changes.
			m_labelStepSize = new QLabel(this);
			m_labelStepSize->setTextInteractionFlags(Qt::NoTextInteraction);
			updateStepSizeLabel(m_spinMultiplier->value());
			setupForm->addRow(tr("Threshold step:"), m_labelStepSize);

			// Live read-only label: baseline + iterations × step
			m_labelFinalThreshold = new QLabel(this);
			m_labelFinalThreshold->setTextInteractionFlags(Qt::NoTextInteraction);
			updateFinalThresholdLabel(m_spinIterations->value(), m_spinMultiplier->value());
			setupForm->addRow(tr("Final threshold:"), m_labelFinalThreshold);

			// Start threshold: default (baseline) or user-supplied override.
			// The spinbox is only active when the Override checkbox is checked.
			m_chkCustomStart = new QCheckBox(tr("Override"), this);

			m_spinCustomStart = new QDoubleSpinBox(this);
			m_spinCustomStart->setDecimals(2);
			m_spinCustomStart->setRange(-99999.0, 99999.0);
			m_spinCustomStart->setSingleStep(1.0);
			m_spinCustomStart->setValue(baselineThreshold);
			m_spinCustomStart->setEnabled(false);

			auto* startRow = new QHBoxLayout;
			startRow->setContentsMargins(0, 0, 0, 0);
			startRow->addWidget(m_chkCustomStart);
			startRow->addWidget(m_spinCustomStart, 1);
			auto* startRowWidget = new QWidget(this);
			startRowWidget->setLayout(startRow);
			setupForm->addRow(tr("Start threshold:"), startRowWidget);

			connect(m_chkCustomStart, &QCheckBox::toggled, this,
				[this](bool checked)
				{
					m_spinCustomStart->setEnabled(checked);
					updateFinalThresholdLabel(
						m_spinIterations->value(), m_spinMultiplier->value());
				});

			connect(m_spinCustomStart,
				QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this,
				[this](double)
				{
					if (m_chkCustomStart->isChecked())
						updateFinalThresholdLabel(
							m_spinIterations->value(), m_spinMultiplier->value());
				});

			// Recompute whenever either spin changes
			connect(m_spinIterations,
				QOverload<int>::of(&QSpinBox::valueChanged),
				this,
				[this](int)
				{
					updateFinalThresholdLabel(
						m_spinIterations->value(),
						m_spinMultiplier->value());
				});

			connect(m_spinMultiplier,
				QOverload<double>::of(&QDoubleSpinBox::valueChanged),
				this,
				[this](double)
				{
					updateFinalThresholdLabel(
						m_spinIterations->value(),
						m_spinMultiplier->value());
				});

			auto* setupBox = new QGroupBox(tr("Setup"), this);
			setupBox->setLayout(setupForm);

			m_btnRun = new QPushButton(tr("Run"), this);
			connect(m_btnRun, &QPushButton::clicked, this,
				[this] { runIterations(); });

			m_btnReset = new QPushButton(tr("Reset"), this);
			connect(m_btnReset, &QPushButton::clicked, this,
				[this] { resetState(); });

			// Refine: enabled when >=2 rows are spanned by the table selection.
			// Sets start threshold = min selected threshold and back-computes the
			// multiplier so that iterations x step = max selected threshold - start.
			m_btnRefine = new QPushButton(tr("Refine from Selection"), this);
			m_btnRefine->setEnabled(false);
			m_btnRefine->setToolTip(
				tr("Set the start threshold to the lowest selected threshold and compute\n"
				"a step multiplier so that iterations \u00D7 step spans to the highest\n"
				"selected threshold."));
			connect(m_btnRefine, &QPushButton::clicked, this,
				[this] { applyRefinement(); });

			auto* leftLayout = new QVBoxLayout;
			leftLayout->addWidget(setupBox);
			leftLayout->addSpacing(8);
			leftLayout->addWidget(m_btnRun);
			leftLayout->addWidget(m_btnReset);
			leftLayout->addWidget(m_btnRefine);
			leftLayout->addStretch();

			// ── Right panel: iteration history table (top) ───────────────────
			// One row per completed iteration; the Select column is mutually
			// exclusive so exactly one iteration can be nominated for Apply.
			m_iterationTable = new QTableWidget(0, 5, this);
			m_iterationTable->setHorizontalHeaderLabels(
				{ tr("#"), tr("Threshold"), tr("Volume (\u00D710\u00B3 mm\u00B3)"), tr("Islands"), tr("Select") });
			// ExtendedSelection: click selects a row; Ctrl+click / Shift+click adds to or ranges the selection.
			m_iterationTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
			m_iterationTable->setSelectionBehavior(QAbstractItemView::SelectRows);
			m_iterationTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
			m_iterationTable->verticalHeader()->setVisible(false);
			m_iterationTable->horizontalHeader()->setSectionResizeMode(
				2, QHeaderView::Stretch);
			m_iterationTable->horizontalHeader()->setStretchLastSection(false);

			// QButtonGroup enforces mutual exclusivity across all row checkboxes.
			m_selectionGroup = new QButtonGroup(this);
			m_selectionGroup->setExclusive(true);

			auto* historyBox = new QGroupBox(tr("Iteration History"), this);
			auto* historyBoxLayout = new QVBoxLayout(historyBox);
			historyBoxLayout->addWidget(m_iterationTable);

			// ── Right panel: chart (bottom) ───────────────────────────────────
			m_series = new QLineSeries(this);
			m_series->setName(tr("Volume"));

			m_seriesPoints = new QScatterSeries(this);
			m_seriesPoints->setName(tr("Points"));
			m_seriesPoints->setMarkerSize(10.0);
			m_seriesPoints->setColor(Qt::red);
			m_seriesPoints->setMarkerShape(QScatterSeries::MarkerShapeCircle);
			m_seriesPoints->setBorderColor(Qt::black);

			m_axisX = new QValueAxis(this);
			m_axisX->setTitleText(tr("Threshold"));
			m_axisX->setRange(0.0, 1.0);

			m_axisY = new QValueAxis(this);
			m_axisY->setTitleText(tr("Volume (\u00D710\u00B3 mm\u00B3)"));
			m_axisY->setRange(0.0, 1.0);

			m_chart = new QChart;
			m_chart->addSeries(m_series);
			m_chart->addSeries(m_seriesPoints);
			m_chart->setTitle(tr("Threshold vs. Region Volume"));
			m_chart->legend()->setVisible(true);

			m_chart->addAxis(m_axisX, Qt::AlignBottom);
			m_chart->addAxis(m_axisY, Qt::AlignLeft);

			m_series->attachAxis(m_axisX);
			m_series->attachAxis(m_axisY);
			m_seriesPoints->attachAxis(m_axisX);
			m_seriesPoints->attachAxis(m_axisY);

			// Vertical bracket lines drawn from the x-axis to the data marker
			// at the first and last selected row.  Amber dashed, hidden from legend.
			const QPen bracketPen(QColor(255, 165, 0), 2, Qt::DashLine);

			m_bracketLeft = new QLineSeries(this);
			m_bracketLeft->setPen(bracketPen);
			m_bracketLeft->setName(tr("Selection"));

			m_bracketRight = new QLineSeries(this);
			m_bracketRight->setPen(bracketPen);
			m_bracketRight->setName(QString());

			m_chart->addSeries(m_bracketLeft);
			m_chart->addSeries(m_bracketRight);
			m_bracketLeft->attachAxis(m_axisX);
			m_bracketLeft->attachAxis(m_axisY);
			m_bracketRight->attachAxis(m_axisX);
			m_bracketRight->attachAxis(m_axisY);

			// Hide the duplicate right-bracket entry from the chart legend.
			const auto rightMarkers = m_chart->legend()->markers(m_bracketRight);
			if (!rightMarkers.isEmpty())
				rightMarkers.first()->setVisible(false);

			// Update bracket lines and row highlights whenever table selection changes.
			connect(m_iterationTable, &QTableWidget::itemSelectionChanged,
				this, [this]() { updateSelectionBrackets(); });

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
		// Returns the active starting threshold: custom value when override is
		// checked, the original baseline threshold otherwise.
		double effectiveStartThreshold() const
		{
			return (m_chkCustomStart && m_chkCustomStart->isChecked())
				? m_spinCustomStart->value()
				: m_baseThreshold;
		}

		void updateStepSizeLabel(double multiplier)
		{
			m_labelStepSize->setText(
				QString::number(multiplier * m_baseStdDev, 'f', 3));
		}

		void updateFinalThresholdLabel(int iterations, double multiplier)
		{
			const double finalThreshold =
				effectiveStartThreshold()
				+ static_cast<double>(iterations) * multiplier * m_baseStdDev;
			m_labelFinalThreshold->setText(
				QString::number(finalThreshold, 'f', 2));
		}

		// Populate the start threshold and multiplier spin boxes from the current
		// table selection so that the next Run will iterate from the minimum
		// selected threshold to the maximum selected threshold.
		void applyRefinement()
		{
			const QList<QTableWidgetSelectionRange> ranges =
				m_iterationTable->selectedRanges();
			if (ranges.isEmpty() || m_iterationThresholds.empty())
				return;

			int minRow = std::numeric_limits<int>::max();
			int maxRow = std::numeric_limits<int>::min();
			for (const auto& range : ranges)
			{
				minRow = std::min(minRow, range.topRow());
				maxRow = std::max(maxRow, range.bottomRow());
			}

			const int nData = static_cast<int>(m_iterationThresholds.size());
			if (minRow >= maxRow || minRow < 0 || maxRow >= nData)
				return;

			const double threshMin = m_iterationThresholds[static_cast<std::size_t>(minRow)];
			const double threshMax = m_iterationThresholds[static_cast<std::size_t>(maxRow)];
			const int    iters = m_spinIterations->value();

			// Override the start threshold with the selection minimum.
			m_chkCustomStart->setChecked(true);
			m_spinCustomStart->setValue(threshMin);

			// Back-compute the multiplier:
			//   iters × multiplier × stdDev = threshMax - threshMin
			//   multiplier = (threshMax - threshMin) / ((iters - 1) × stdDev)
			if (iters >= 2 && m_baseStdDev > 0.0)
			{
				const double newMultiplier =
					(threshMax - threshMin)
					/ (static_cast<double>(iters-1) * m_baseStdDev);

				// QDoubleSpinBox::setValue() silently clamps to [minimum, maximum].
				// The refined multiplier may be far outside the default [0.01, 10.0]
				// range (e.g. very small for a tight bracket, large for a small stdDev).
				// Expand the range symmetrically before setting the value so the
				// assigned value is exact.  The range is only ever widened so
				// subsequent manual edits retain full freedom.
				const double safeMin = std::min(m_spinMultiplier->minimum(),
											   std::max(1e-7, newMultiplier));
				const double safeMax = std::max(m_spinMultiplier->maximum(),
											   newMultiplier);
				m_spinMultiplier->setRange(safeMin, safeMax);

				// Compute the required decimal precision from the magnitude of
				// newMultiplier using log base-10:
				//   decimals = -floor(log10(v)) + 2
				// This gives 2 significant digits after the leading digit:
				//   v = 0.1   -> decimals = 1 + 2 = 3   ("0.100")
				//   v = 0.01  -> decimals = 2 + 2 = 4   ("0.0100")
				//   v = 0.001 -> decimals = 3 + 2 = 5   ("0.00100")
				// Clamped to [3, 9] so values >= 0.1 always show at least 3 places
				// and extremely small values are capped at 9.
				const int decimals = (newMultiplier > 0.0)
					? std::clamp(
						static_cast<int>(-std::floor(std::log10(newMultiplier))) + 2,
						3, 9)
					: 3;
				m_spinMultiplier->setDecimals(decimals);

				m_spinMultiplier->setValue(newMultiplier);
			}

			updateStepSizeLabel(m_spinMultiplier->value());
			updateFinalThresholdLabel(iters, m_spinMultiplier->value());
		}

		// Called whenever the table selection changes.
		// - Clears all row highlights then re-applies them across the contiguous
		//   range from the topmost to the bottommost selected row.
		// - Draws a vertical dashed bracket from the x-axis to the data marker
		//   at the minimum and maximum threshold of the selected range.
		void updateSelectionBrackets()
		{
			m_bracketLeft->clear();
			m_bracketRight->clear();

			// Remove all existing row background highlights (columns 0-3 only;
			// column 4 hosts a QWidget for the checkbox and has no QTableWidgetItem).
			const int nRows = m_iterationTable->rowCount();
			for (int row = 0; row < nRows; ++row)
			{
				for (int col = 0; col < 4; ++col)
				{
					auto* item = m_iterationTable->item(row, col);
					if (item)
						item->setBackground(QBrush());
				}
			}

			const QList<QTableWidgetSelectionRange> ranges =
				m_iterationTable->selectedRanges();

			if (ranges.isEmpty() || m_iterationThresholds.empty())
			{
				m_btnRefine->setEnabled(false);
				return;
			}

			// Determine the overall span: topmost and bottommost selected row.
			int minRow = std::numeric_limits<int>::max();
			int maxRow = std::numeric_limits<int>::min();
			for (const auto& range : ranges)
			{
				minRow = std::min(minRow, range.topRow());
				maxRow = std::max(maxRow, range.bottomRow());
			}

			if (minRow > maxRow || minRow < 0 || maxRow >= nRows)
				return;

			// Highlight every row in the span, not just the explicitly selected ones.
			const QColor highlightColor(255, 245, 157); // light amber
			for (int row = minRow; row <= maxRow; ++row)
			{
				for (int col = 0; col < 4; ++col)
				{
					auto* item = m_iterationTable->item(row, col);
					if (item)
						item->setBackground(QBrush(highlightColor));
				}
			}

			// Draw bracket lines from the axis y-minimum up to each data marker.
			const double yMin = m_axisY->min();
			const int    nData = static_cast<int>(m_iterationThresholds.size());

			if (minRow < nData)
			{
				const double xL = m_iterationThresholds[static_cast<std::size_t>(minRow)];
				const double yL = m_iterationVolumes[static_cast<std::size_t>(minRow)];
				m_bracketLeft->append(xL, yMin);
				m_bracketLeft->append(xL, yL);
			}

			// Right bracket only when the selection covers more than one row.
			if (maxRow != minRow && maxRow < nData)
			{
				const double xR = m_iterationThresholds[static_cast<std::size_t>(maxRow)];
				const double yR = m_iterationVolumes[static_cast<std::size_t>(maxRow)];
				m_bracketRight->append(xR, yMin);
				m_bracketRight->append(xR, yR);
			}

			// Refine requires at least two distinct rows to define a range.
			m_btnRefine->setEnabled(maxRow > minRow && maxRow < nData);
		}

		void runIterations()
		{
			if (!m_iterateFunc)
				return;

			// When a custom start threshold is active (e.g. set via Refine from
			// Selection) perform a clean reset before running so the table, chart
			// and 3D view all start from a known baseline state.  The three
			// user-configured parameters are captured before the reset and
			// restored immediately afterwards so the run proceeds with the
			// refined settings intact.
			if (m_chkCustomStart->isChecked())
			{
				const double savedStart = m_spinCustomStart->value();
				const int    savedIterations = m_spinIterations->value();
				const double savedMultiplier = m_spinMultiplier->value();

				resetState(); // clears table, chart, 3D view; unchecks override

				m_chkCustomStart->setChecked(true);
				m_spinCustomStart->setValue(savedStart);
				m_spinIterations->setValue(savedIterations);
				m_spinMultiplier->setValue(savedMultiplier);

				updateStepSizeLabel(savedMultiplier);
				updateFinalThresholdLabel(savedIterations, savedMultiplier);
			}

			m_btnRun->setEnabled(false);
			m_btnReset->setEnabled(false);
			m_spinIterations->setEnabled(false);
			m_spinTargetIslands->setEnabled(false);
			m_spinVolumeThreshold->setEnabled(false);

			const int    maxIter = m_spinIterations->value();
			const double multiplier = m_spinMultiplier->value();
			const double startThreshold = effectiveStartThreshold();
			const int maxIslands = m_spinTargetIslands->value();
			const double volumeThreshold = m_spinVolumeThreshold->value();

			// Clear per-run data so a second Run without an explicit Reset starts
			// from a clean table and matched threshold/volume vectors.
			m_iterationThresholds.clear();
			m_iterationVolumes.clear();
			m_iterationTable->setRowCount(0);
			const auto existingButtons = m_selectionGroup->buttons();
			for (auto* btn : existingButtons)
				m_selectionGroup->removeButton(btn);

			rebuildTableColumns(maxIslands);

			// Tracks whether the loop exited via the target-islands criterion
			// and which row first reached that count.  Used by the post-loop
			// convergence check.
			bool targetReached = false;
			int  targetRowM = -1;

			for (int iter = 1; iter <= maxIter; ++iter)
			{
				const double threshold =
					startThreshold
					+ static_cast<double>(iter) * multiplier * m_baseStdDev;

				const auto islands = m_iterateFunc(threshold, iter == 1);

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

				m_series->append(threshold, volumeMm3x1k);
				m_seriesPoints->append(threshold, volumeMm3x1k);

				// Track data extents and update the persistent axes in-place.
				// This avoids removing/re-adding series (which recreates axes
				// and causes ghost tick labels on every iteration).
				m_dataMinX = std::min(m_dataMinX, threshold);
				m_dataMaxX = std::max(m_dataMaxX, threshold);
				m_dataMinY = std::min(m_dataMinY, volumeMm3x1k);
				m_dataMaxY = std::max(m_dataMaxY, volumeMm3x1k);

				const double rangeX = m_dataMaxX - m_dataMinX;
				const double rangeY = m_dataMaxY - m_dataMinY;
				const double padX = (rangeX > 0.0) ? 0.05 * rangeX : std::max(1.0, std::abs(m_dataMinX) * 0.05);
				const double padY = (rangeY > 0.0) ? 0.05 * rangeY : std::max(1.0, std::abs(m_dataMinY) * 0.05);

				m_axisX->setRange(m_dataMinX - padX, m_dataMaxX + padX);
				m_axisY->setRange(std::max(0.0, m_dataMinY - padY), m_dataMaxY + padY);
				// Append one row to the iteration history table.
				appendIterationRow(iter, threshold, volumeMm3x1k, islands);

				QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

				// Stop as soon as the segmentation produces the desired number of islands.
				if (static_cast<int>(islands.size()) >= maxIslands)
				{
					qDebug("IterationProgressDialog: iter=%d — target island count %d reached (%zu islands); stopping.",
						iter, maxIslands, islands.size());

					targetReached = true;
					targetRowM = m_iterationTable->rowCount() - 1;

					// Auto-select the two bracketing rows so Refine from Selection
					// is immediately primed with the correct parameters:
					//   row M   = this row (first to reach target island count)  -> TM
					//   row N   = M - 1  (last row below target count)          -> TN
					//   K = (TM - TN) / (maxIterations x stdDev)
					const int rowM = targetRowM;
					const int rowN = rowM - 1;

					m_iterationTable->clearSelection();

					if (rowN >= 0)
					{
						// Select the contiguous range [N, M] across all columns.
						m_iterationTable->setRangeSelected(
							QTableWidgetSelectionRange(
							rowN, 0, rowM, m_iterationTable->columnCount() - 1),
							true);
					}
					else if (rowM >= 0)
					{
						// Only one row exists; select it alone.
						m_iterationTable->setRangeSelected(
							QTableWidgetSelectionRange(
							rowM, 0, rowM, m_iterationTable->columnCount() - 1),
							true);
					}

					// Scroll so the selection is visible.
					if (auto* item = m_iterationTable->item(rowM, 0))
						m_iterationTable->scrollToItem(item);

					break;
				}
			}

			// Post-loop convergence check.
			// Only evaluated when the loop stopped because the target island
			// count was reached AND there are at least two rows to compare.
			if (targetReached
				&& targetRowM >= 1
				&& targetRowM < static_cast<int>(m_iterationVolumes.size()))
			{
				const int    rowM = targetRowM;
				const int    rowN = rowM - 1;
				const double volChange = std::abs(
					m_iterationVolumes[static_cast<std::size_t>(rowM)]
					- m_iterationVolumes[static_cast<std::size_t>(rowN)]);

				if (volChange <= m_spinVolumeThreshold->value())
				{
					// Volume change is below the threshold: refinement would not
					// meaningfully improve the result.  Clear the table row
					// selection entirely so the island cell colours remain visible,
					// then check rowM's Select checkbox so Apply Selection is primed.
					m_iterationTable->clearSelection();

					if (auto* btn = m_selectionGroup->button(rowM))
						btn->setChecked(true);

					m_btnRefine->setEnabled(false);

					if (auto* item = m_iterationTable->item(rowM, 0))
						m_iterationTable->scrollToItem(item);

					QMessageBox::warning(
						this,
						tr("Volume Convergence \u2014 Refinement Not Required"),
						tr("The volume change between the two bracketing rows is "
						"<b>%1 \u00D710\u207B\u00B3 mm\u00B3</b>, which is at or below "
						"the volume threshold of <b>%2 \u00D710\u207B\u00B3 mm\u00B3</b>.<br><br>"
						"Further refinement is unlikely to improve the result.<br><br>"
						"Row <b>%3</b> (threshold&nbsp;%4) has been selected automatically."
						"<br>Click <i>Apply Selection</i> to finalise.")
							.arg(volChange, 0, 'f', 4)
							.arg(m_spinVolumeThreshold->value(), 0, 'f', 4)
							.arg(rowM + 1)
							.arg(m_iterationThresholds[static_cast<std::size_t>(rowM)], 0, 'f', 2));
				}
			}

			m_btnRun->setEnabled(true);
			m_btnReset->setEnabled(true);
			m_spinIterations->setEnabled(true);
			m_spinMultiplier->setEnabled(true);
			m_spinTargetIslands->setEnabled(true);
			m_spinVolumeThreshold->setEnabled(true);
		}

		// Rebuilds the iteration table column headers to include islandCount
		// per-island volume columns between Islands and Select.
		// Call before any rows are appended for a new Run, and with 0 to reset.
		void rebuildTableColumns(int islandCount)
		{
			m_islandColumnCount = islandCount;

			const int totalCols = 5 + m_islandColumnCount;
			m_iterationTable->setColumnCount(totalCols);

			QStringList headers;
			headers << tr("#")
				<< tr("Threshold")
				<< tr("Volume (\u00D710\u207B\u00B3 mm\u00B3)")
				<< tr("Islands");

			for (int i = 0; i < m_islandColumnCount; ++i)
				headers << tr("Island %1 (\u00D710\u207B\u00B3 mm\u00B3)").arg(i + 1);

			headers << tr("Select");
			m_iterationTable->setHorizontalHeaderLabels(headers);

			// Total volume column always stretches; island columns use interactive
			// resize so the user can adjust; Select column is fixed.
			m_iterationTable->horizontalHeader()->setSectionResizeMode(
				2, QHeaderView::Stretch);
			m_iterationTable->horizontalHeader()->setStretchLastSection(false);
		}

		// Appends one row to the iteration history table.
		// islands: the raw BoneIsland result for this iteration — used to populate
		// per-island volume cells and derive per-cell background colours from the
		// same cool-to-warm TF used by the VolumeView legend.
		void appendIterationRow(int iter, double threshold, double volumeMm3x1k,
			const std::vector<PrototypeHelpers::BoneIsland>& islands)
		{
			const int row = m_iterationTable->rowCount();
			m_iterationTable->insertRow(row);

			m_iterationThresholds.push_back(threshold);
			m_iterationVolumes.push_back(volumeMm3x1k);

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
				makeItem(QString::number(static_cast<int>(islands.size()))));

			// ── Per-island volume columns ─────────────────────────────────────
			// Build the same cool-to-warm colour TF used by the VolumeView legend
			// (makeIslandColorTF) so each island cell's background exactly matches
			// the corresponding surface actor colour shown in the 3D view.
			if (m_islandColumnCount > 0)
			{
				// Compute the voxel-count range for this row's islands so the TF
				// maps consistently to the same scale as the VolumeView scalar bar.
				vtkIdType minVox = islands.empty() ? 0 : islands[0].voxelCount;
				vtkIdType maxVox = minVox;
				for (const auto& isl : islands)
				{
					minVox = std::min(minVox, isl.voxelCount);
					maxVox = std::max(maxVox, isl.voxelCount);
				}

				vtkSmartPointer<vtkColorTransferFunction> colorTF;
				if (!islands.empty())
					colorTF = PrototypeHelpers::makeIslandColorTF(
						static_cast<double>(minVox),
						static_cast<double>(maxVox));

				for (int col = 0; col < m_islandColumnCount; ++col)
				{
					const int tableCol = 4 + col;

					if (col < static_cast<int>(islands.size()))
					{
						const auto& isl =
							islands[static_cast<std::size_t>(col)];

						const double islVolMm3x1k =
							static_cast<double>(isl.voxelCount)
							* m_voxelVolMm3 * 1000.0;

						// Sample the same TF used by the VolumeView legend.
						double rgb[3] = { 1.0, 1.0, 1.0 };
						colorTF->GetColor(
							static_cast<double>(isl.voxelCount), rgb);

						const QColor bgColor(
							static_cast<int>(rgb[0] * 255.0),
							static_cast<int>(rgb[1] * 255.0),
							static_cast<int>(rgb[2] * 255.0));

						// Perceived luminance — choose black or white text
						// so it remains readable on any background hue.
						const double lum =
							0.299 * rgb[0]
							+ 0.587 * rgb[1]
							+ 0.114 * rgb[2];
						const QColor fgColor =
							(lum > 0.5) ? Qt::black : Qt::white;

						auto* item = makeItem(
							QString::number(islVolMm3x1k, 'g', 3));
						item->setBackground(QBrush(bgColor));
						item->setForeground(QBrush(fgColor));
						m_iterationTable->setItem(row, tableCol, item);
					}
					else
					{
						// No island occupies this slot — empty cell, no colour.
						m_iterationTable->setItem(row, tableCol, makeItem(QString()));
					}
				}
			}

			// ── Select checkbox — always the last column ──────────────────────
			const int selectCol = 4 + m_islandColumnCount;

			auto* chkWidget = new QWidget(m_iterationTable);
			auto* chk = new QCheckBox(chkWidget);
			m_selectionGroup->addButton(chk, row);

			auto* chkLayout = new QHBoxLayout(chkWidget);
			chkLayout->addWidget(chk);
			chkLayout->setAlignment(Qt::AlignCenter);
			chkLayout->setContentsMargins(0, 0, 0, 0);

			m_iterationTable->setCellWidget(row, selectCol, chkWidget);
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
			m_iterationVolumes.clear();
			m_iterationTable->setRowCount(0);
			rebuildTableColumns(0);

			m_btnRefine->setEnabled(false);

			m_chkCustomStart->setChecked(false);
			m_spinCustomStart->setValue(m_baseThreshold);

			// Restore all spins to their defaults so a Reset gives a fully
			// clean slate: multiplier range/precision, and volume threshold.
			m_spinMultiplier->setDecimals(3);
			m_spinMultiplier->setRange(0.001, 10.0);
			m_spinMultiplier->setValue(0.1);
			m_spinVolumeThreshold->setValue(1.0);

			m_series->clear();
			m_seriesPoints->clear();
			m_bracketLeft->clear();
			m_bracketRight->clear();

			m_dataMinX = std::numeric_limits<double>::max();
			m_dataMaxX = std::numeric_limits<double>::lowest();
			m_dataMinY = std::numeric_limits<double>::max();
			m_dataMaxY = std::numeric_limits<double>::lowest();

			m_axisX->setRange(0.0, 1.0);
			m_axisY->setRange(0.0, 1.0);

			if (m_resetFunc)
				m_resetFunc();

			qDebug("IterationProgressDialog: state reset to baseline.");
		}

		double          m_baseThreshold;
		double          m_baseStdDev;
		double          m_voxelVolMm3;
		int m_islandColumnCount = 0;

		QSpinBox* m_spinIterations = nullptr;
		QSpinBox* m_spinTargetIslands = nullptr;
		QDoubleSpinBox* m_spinMultiplier = nullptr;
		QDoubleSpinBox* m_spinVolumeThreshold = nullptr;
		QLabel* m_labelStepSize = nullptr;
		QLabel* m_labelFinalThreshold = nullptr;
		QCheckBox* m_chkCustomStart = nullptr;
		QDoubleSpinBox* m_spinCustomStart = nullptr;
		QPushButton* m_btnRun = nullptr;
		QPushButton* m_btnReset = nullptr;
		QPushButton* m_btnRefine = nullptr;

		QTableWidget* m_iterationTable = nullptr;
		QButtonGroup* m_selectionGroup = nullptr;
		QLineSeries* m_series = nullptr;
		QScatterSeries* m_seriesPoints = nullptr;
		QLineSeries* m_bracketLeft = nullptr;
		QLineSeries* m_bracketRight = nullptr;
		QChart* m_chart = nullptr;
		QValueAxis* m_axisX = nullptr;
		QValueAxis* m_axisY = nullptr;

		double m_dataMinX = std::numeric_limits<double>::max();
		double m_dataMaxX = std::numeric_limits<double>::lowest();
		double m_dataMinY = std::numeric_limits<double>::max();
		double m_dataMaxY = std::numeric_limits<double>::lowest();

		IterateFunc              m_iterateFunc;
		ResetFunc                m_resetFunc;
		std::vector<double>      m_iterationThresholds;
		std::vector<double>      m_iterationVolumes;
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
	// "File" toolbar button — always enabled.
	m_actFile = new QAction(tr("File"), this);
	m_actFile->setToolTip(tr("Open a project sidecar JSON file"));
	ui->toolBar->addAction(m_actFile);
	connect(m_actFile, &QAction::triggered, this, &PrototypeMainWindow::onFileOpen);

	// "Initialize" toolbar button — runs Reslice then Landmark in one step.
	// Replaces the separate Reslice and Landmark buttons so the user reaches
	// the Landmarked state (Regions enabled) with a single click.
	m_actInitialize = new QAction(tr("Initialize"), this);
	m_actInitialize->setToolTip(
		tr("PCA-reslice the volume then locate surface landmark points (Reslice + Landmark)"));
	ui->toolBar->addAction(m_actInitialize);
	connect(m_actInitialize, &QAction::triggered,
		this, &PrototypeMainWindow::onInitialize);

	// "Regions" toolbar button: threshold + seeded BFS island segmentation
	m_actRegions = new QAction(tr("Regions"), this);
	m_actRegions->setToolTip(tr("Segment bone islands from the resliced volume using landmark seeds"));
	ui->toolBar->addAction(m_actRegions);
	connect(m_actRegions, &QAction::triggered, this, &PrototypeMainWindow::onRegions);

#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	// "Graph Cut" toolbar button: ITK ImageGridCutFilter (multi-threaded GridCut solver)
	m_actRegionsGraphCut = new QAction(tr("Graph Cut"), this);
	m_actRegionsGraphCut->setToolTip(tr(
		"Segment bone islands using ITK graph cut (ImageGridCutFilter, multi-threaded GridCut solver)"));
	ui->toolBar->addAction(m_actRegionsGraphCut);
	connect(m_actRegionsGraphCut, &QAction::triggered,
			this, &PrototypeMainWindow::onRegionsGraphCut);
#endif // CTAXPROTOTYPE_ENABLE_GRAPH_CUT

	// "Clean" toolbar button: post-segmentation clean step.
	// Enabled only after segmentation completes AND at least 8 Reslice operations
	// have been performed in the current session (m_resliceCount >= 8).
	m_actClean = new QAction(tr("Clean"), this);
	m_actClean->setToolTip(tr("Run the post-segmentation clean step"));
	ui->toolBar->addAction(m_actClean);
	connect(m_actClean, &QAction::triggered, this, &PrototypeMainWindow::onClean);

	// "Export Reslice" toolbar button: reslice -> landmark -> threshold crop -> NIfTI export.
	m_actExportReslice = new QAction(tr("Export Reslice"), this);
	m_actExportReslice->setToolTip(tr(
		"Reslice, landmark, threshold-crop the volume and export as a NIfTI file"));
	ui->toolBar->addAction(m_actExportReslice);
	connect(m_actExportReslice, &QAction::triggered,
		this, &PrototypeMainWindow::onExport);

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

	connect(ui->sliceView, &SliceView::cursorDataChanged, this,
	[this](const QString& text)
	{
		if (text.isEmpty())
			statusBar()->clearMessage();
		else
			statusBar()->showMessage(text);
	});

	// ── Orphan mask toggle ────────────────────────────────────────────────────
// Placed in the SliceView's right-aligned header area beside the expand
// button.  Disabled until onInitialize() step 3 produces a valid orphan
// mask; consumed and disabled again by onClean().
	m_actToggleOrphanMask = new QAction(tr("Mask"), this);
	m_actToggleOrphanMask->setCheckable(true);
	m_actToggleOrphanMask->setChecked(false);
	m_actToggleOrphanMask->setEnabled(false);
	m_actToggleOrphanMask->setToolTip(
		tr("Toggle between the resliced image and the orphan mask.\n"
		"White voxels are foreground regions not connected to any\n"
		"landmark seed point — these will be removed by Clean."));
	ui->sliceView->addHeaderAction(m_actToggleOrphanMask);

	connect(m_actToggleOrphanMask, &QAction::toggled,
		this, [this](bool checked)
		{
			if (!ui || !ui->sliceView)
				return;

			if (checked && m_orphanMaskImage)
			{
				// Orphan mask is VTK_UNSIGNED_CHAR 0/1.
				// window=1, level=0.5 maps 0→black, 1→white.
				ui->sliceView->setImageData(m_orphanMaskImage);
				ui->sliceView->updateData();
				ui->sliceView->setWindowLevelNative(1.0, 0.5);
			}
			else
			{
				// Restore the resliced image with its current WL parameters.
				vtkImageData* primary =
					m_reslicedImage ? m_reslicedImage.Get() : m_image;

				const double level = std::isfinite(m_threshold)
					? m_threshold : 0.0;
				const double window = 2.0 * m_imageStats
					.value(QStringLiteral("stdDev")).toDouble(1.0);

				ui->sliceView->setImageData(primary);
				ui->sliceView->updateData();
				if (primary)
					ui->sliceView->setWindowLevelNative(window, level);
			}
		});
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

	const bool atIdle = (step == WorkflowStep::Idle);
	const bool atLandmarked = (step == WorkflowStep::Landmarked);
	const bool atSegmented = (step == WorkflowStep::Segmented);
	const bool atCleaned = (step == WorkflowStep::Cleaned);

	m_actFile->setEnabled(true);
	m_actInitialize->setEnabled(atIdle);
	m_actRegions->setEnabled(atLandmarked);
#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	m_actRegionsGraphCut->setEnabled(atLandmarked);
#endif
	m_actClean->setEnabled(atSegmented || atCleaned);

	// Export is only meaningful once onClean() has produced a cleaned
	// resliced image that can be projected back into original image space.
	m_actExportReslice->setEnabled(atCleaned);

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
	m_progressBar->repaint();
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
}


// ---------------------------------------------------------------------------
// Graph-cut seed actor management
// ---------------------------------------------------------------------------
#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
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
#endif // CTAXPROTOTYPE_ENABLE_GRAPH_CUT


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

	setWindowTitle(
		tr("CTAXPrototype \u2014 %1").arg(QFileInfo(m_cropPath).fileName()));

	// A freshly loaded image starts the workflow at Idle (only Reslice enabled).
	setWorkflowStep(WorkflowStep::Idle);
}

void PrototypeMainWindow::setImage(vtkSmartPointer<vtkImageData> image)
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
#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	clearGraphCutSeedActors();
#endif

	// Release derived segmentation data so downstream steps (onLandmark,
	// onRegions*) always start from a clean slate for the incoming image.
	m_labelImage = nullptr;
	m_islands.clear();

	// Cache the image for use by onLandmark() and onReslice().
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

	m_lastResliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
	m_lastResliceAxes->DeepCopy(resliceAxes);

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

	if (islands.empty())
	{
		qWarning("applyIslandSegmentationResult: called with empty islands vector; aborting.");
		return;
	}

	const auto [minIt, maxIt] = std::minmax_element(islands.begin(), islands.end(),
		[](const PrototypeHelpers::BoneIsland& a, const PrototypeHelpers::BoneIsland& b)
		{ return a.voxelCount < b.voxelCount; });
	const vtkIdType minVoxels = minIt->voxelCount;
	const vtkIdType maxVoxels = maxIt->voxelCount;

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
	const int nIslands = static_cast<int>(islands.size());
	vtkRenderer* ren = (ui && ui->volumeView) ? ui->volumeView->renderer() : nullptr;

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

#ifdef PARALLEL_ISLANDS
	std::vector<PrototypeHelpers::BoneIsland> islands =
		PrototypeHelpers::segmentBoneIslandsParallel(
			m_reslicedImage, m_threshold, seeds, labelImage, makeProgress);
#else
	std::vector<PrototypeHelpers::BoneIsland> islands =
		PrototypeHelpers::segmentBoneIslands(
			m_reslicedImage, m_threshold, seeds, labelImage, makeProgress);
#endif

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

	qDebug("onRegions: baseline region stats — mean=%d  stdDev=%d  volume=%.3f x 10^-3 mm^3",
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
		[this, seeds](double threshold, bool firstIteration) -> std::vector<PrototypeHelpers::BoneIsland>
		{
			const auto progress = [this](int pct)
				{
					showProgressValue(pct);
					m_progressBar->update();
					QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
				};

			const std::vector<std::array<double, 3>> adjustedSeeds =
				PrototypeHelpers::computeInwardAdjustedSeeds(
					m_reslicedImage, threshold, seeds, m_pca);

			showProgressStart();
			vtkSmartPointer<vtkImageData> iterLabel;

#ifdef PARALLEL_ISLANDS
			auto iterIslands = PrototypeHelpers::segmentBoneIslandsParallel(
				m_reslicedImage, threshold, adjustedSeeds, iterLabel, progress);
#else
			auto iterIslands = PrototypeHelpers::segmentBoneIslands(
				m_reslicedImage, threshold, adjustedSeeds, iterLabel, progress);
#endif

			showProgressEnd();

			if (!iterIslands.empty())
				applyIslandSegmentationResult(iterIslands, iterLabel);

			// ── Incremental orphan accumulation ───────────────────────────
			// Reset the cumulative mask at the start of each fresh Run so
			// stale results from a previous run are not carried forward.
			if (firstIteration)
				m_orphanMaskImage = nullptr;

			// Identify orphans at the current iteration threshold using the
			// inward-adjusted seeds (which reflect where seeds actually land
			// at this threshold).  OR-merge into the cumulative mask so any
			// island that was ever disconnected from a seed — even if it later
			// merges back or disappears entirely — is permanently captured.
			if (m_reslicedImage)
			{
				vtkSmartPointer<vtkImageData> iterOrphanMask;
				PrototypeHelpers::identifyOrphanIslands(
					m_reslicedImage, threshold, adjustedSeeds,
					iterOrphanMask, /*progressCb=*/nullptr);

				if (iterOrphanMask)
				{
					if (!m_orphanMaskImage)
					{
						// First contribution — take it directly.
						m_orphanMaskImage = iterOrphanMask;
					}
					else
					{
						// OR-merge: any voxel orphaned at any threshold stays
						// marked, even if it disappears at a later iteration.
						const vtkIdType nVox =
							m_orphanMaskImage->GetNumberOfPoints();
						if (iterOrphanMask->GetNumberOfPoints() != nVox)
						{
							qWarning("onRegions orphan OR-merge: mask dimension mismatch "
								"(acc=%lld  new=%lld); skipping merge for this iteration.",
								static_cast<long long>(nVox),
								static_cast<long long>(iterOrphanMask->GetNumberOfPoints()));
						}
						else
						{
							auto* accPtr = static_cast<uint8_t*>(
								m_orphanMaskImage->GetScalarPointer());
							const auto* newPtr = static_cast<const uint8_t*>(
								iterOrphanMask->GetScalarPointer());
							for (vtkIdType i = 0; i < nVox; ++i)
								accPtr[i] |= newPtr[i];
							m_orphanMaskImage->Modified();
						}
					}

					if (m_actToggleOrphanMask)
						m_actToggleOrphanMask->setEnabled(true);
				}
			}

			return iterIslands;
		});

	// Capture baseline islands and labelImage by value — onRegions() returns
	// before the dialog's Reset button can be clicked, so stack locals must
	// be copied into the closure.
	progressDlg->setResetCallback(
		[this, islands, labelImage]()
		{
			m_orphanMaskImage = nullptr;
			if (m_actToggleOrphanMask)
				m_actToggleOrphanMask->setEnabled(false);
			applyIslandSegmentationResult(islands, labelImage);
		});

	progressDlg->setAttribute(Qt::WA_DeleteOnClose);
	progressDlg->show();

	// onRegions() returns here.  The iteration loop runs inside progressDlg
	// when the user clicks Run.  The dialog and its callbacks remain valid
	// for the lifetime of this PrototypeMainWindow (its Qt parent).
}

#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
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
	ui->volumeView->hideAllContent();
	setWorkflowStep(WorkflowStep::Segmented);
}
#endif // CTAXPROTOTYPE_ENABLE_GRAPH_CUT

// ---------------------------------------------------------------------------
// onClean
//
// Algorithm
// ─────────
//  1. User selects which islands to remove and how many dilation passes
//     to apply via IslandCleanDialog.
//  2. Build a combined binary mask: selected islands OR orphan islands.
//     Orphans are merged before dilation so all removed regions receive
//     the same noise-padded boundary margin.
//  3. Dilate the combined mask N times (3×3×3) to carve a clean margin.
//  4. Replace every dilated-mask voxel in m_reslicedImage with the mean
//     background value.
//  5. Hide the removed island surface actors and refresh both views.
//
// The retained island plays no part: its voxels are absent from both the
// selected-island mask and the orphan mask, so they are never touched.
// ---------------------------------------------------------------------------
void PrototypeMainWindow::onClean()
{
	qDebug("onClean: island-targeted clean triggered.");

	// ------------------------------------------------------------------
	// Pre-conditions
	// ------------------------------------------------------------------
	if (!m_reslicedImage)
	{
		qWarning("onClean: no resliced image available.");
		return;
	}
	if (!m_labelImage)
	{
		qWarning("onClean: no label image; run Regions or Graph Cut first.");
		return;
	}
	if (m_islands.empty())
	{
		qWarning("onClean: no segmented islands available.");
		return;
	}
	if (!std::isfinite(m_threshold))
	{
		qWarning("onClean: threshold is not finite.");
		return;
	}

	// ------------------------------------------------------------------
	// Show island selection dialog
	// ------------------------------------------------------------------
	IslandCleanDialog dlg(m_islands, m_reslicedImage->GetSpacing(), this);
	if (dlg.exec() != QDialog::Accepted)
		return;

	const std::vector<int> labelsToRemove = dlg.labelsToRemove();
	const int              nDilations = dlg.dilationCount();

	if (labelsToRemove.empty())
	{
		qDebug("onClean: no islands selected for removal.");
		return;
	}

	showProgressStart();
	showProgressValue(5);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	const double* spacing = m_reslicedImage->GetSpacing();
	const double* origin = m_reslicedImage->GetOrigin();
	const int* dims = m_reslicedImage->GetDimensions();
	const vtkIdType totalVoxels =
		static_cast<vtkIdType>(dims[0]) * dims[1] * dims[2];

	vtkDataArray* labelScalars = m_labelImage->GetPointData()->GetScalars();
	vtkDataArray* reslicedScalars = m_reslicedImage->GetPointData()->GetScalars();

	if (!labelScalars || !reslicedScalars)
	{
		qWarning("onClean: scalar arrays missing.");
		showProgressEnd();
		return;
	}

	const double bgMean =
		m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);
	const double scalarMin =
		m_imageStats.value(QStringLiteral("min")).toDouble(0.0);
	const double scalarMax =
		m_imageStats.value(QStringLiteral("max")).toDouble(255.0);

	const std::unordered_set<int> removeSet(
		labelsToRemove.begin(), labelsToRemove.end());

	// ------------------------------------------------------------------
	// Step 1: build combined binary mask — selected islands OR orphans.
	//
	// Orphan voxels are merged into the mask before dilation so that the
	// dilation passes apply equally to both selected islands and orphan
	// islands.  This ensures a consistent noise-padded margin around all
	// removed regions regardless of how they were identified.
	// ------------------------------------------------------------------
	vtkDataArray* orphanScalars = nullptr;
	if (m_orphanMaskImage)
	{
		orphanScalars = m_orphanMaskImage->GetPointData()->GetScalars();
		qDebug("onClean: orphan mask available — merged into pre-dilation removal mask.");
	}
	else
	{
		qDebug("onClean: no orphan mask cached "
			   "(run Initialize to enable orphan removal).");
	}

	auto maskImage = vtkSmartPointer<vtkImageData>::New();
	maskImage->SetDimensions(dims);
	maskImage->SetSpacing(spacing);
	maskImage->SetOrigin(origin);
	maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

	auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
	for (vtkIdType i = 0; i < totalVoxels; ++i)
	{
		const int  lbl = static_cast<int>(labelScalars->GetTuple1(i));
		const bool inIsland = removeSet.count(lbl) > 0;
		const bool inOrphan = orphanScalars && orphanScalars->GetTuple1(i) > 0.5;
		maskPtr[i] = (inIsland || inOrphan) ? 1u : 0u;
	}
	maskImage->Modified();

	showProgressValue(15);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ------------------------------------------------------------------
	// Step 2: dilate the combined mask N times (3×3×3 structuring element).
	// Dilation applies to both selected islands and orphan islands so all
	// removed regions receive the same noise-padded boundary margin.
	// ------------------------------------------------------------------
	vtkSmartPointer<vtkImageData> dilatedMask = maskImage;

	for (int d = 0; d < nDilations; ++d)
	{
		auto dilate = vtkSmartPointer<vtkImageContinuousDilate3D>::New();
		dilate->SetInputData(dilatedMask);
		dilate->SetKernelSize(3, 3, 3);
		dilate->Update();

		auto copy = vtkSmartPointer<vtkImageData>::New();
		copy->DeepCopy(dilate->GetOutput());
		dilatedMask = copy;

		showProgressValue(15 + static_cast<int>(
			40.0 * (d + 1) / static_cast<double>(nDilations)));
		QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
	}

	vtkDataArray* dilatedScalars = dilatedMask->GetPointData()->GetScalars();
	if (!dilatedScalars)
	{
		qWarning("onClean: dilation produced no scalars.");
		showProgressEnd();
		return;
	}

	showProgressValue(60);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ------------------------------------------------------------------
	// Step 3: replace all dilated voxels with background mean.
	// The dilated mask already contains both island and orphan contributions
	// so a single pass suffices — no post-dilation OR is required.
	// ------------------------------------------------------------------
	vtkIdType replacedIsland = 0;
	vtkIdType replacedOrphan = 0;

	for (vtkIdType i = 0; i < totalVoxels; ++i)
	{
		if (dilatedScalars->GetTuple1(i) <= 0.5)
			continue;

		reslicedScalars->SetTuple1(i, bgMean);

		// Attribute each replaced voxel for the debug log.
		// Voxels introduced purely by dilation expansion (not in any source
		// mask) are counted with the island total as they border island tissue.
		const int  lbl = static_cast<int>(labelScalars->GetTuple1(i));
		const bool inIsland = removeSet.count(lbl) > 0;
		const bool inOrphan = orphanScalars && orphanScalars->GetTuple1(i) > 0.5;
		if (inOrphan && !inIsland) ++replacedOrphan;
		else                       ++replacedIsland;
	}

	reslicedScalars->Modified();
	m_reslicedImage->Modified();

	qDebug("onClean: %lld island voxel(s) + %lld orphan voxel(s) replaced "
		   "with background noise (%d dilation pass(es)).",
		static_cast<long long>(replacedIsland),
		static_cast<long long>(replacedOrphan),
		nDilations);

	showProgressValue(80);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// The orphan mask has been consumed — disable the toggle so the user
	// cannot view a now-stale mask after the resliced image has been modified.
	m_orphanMaskImage = nullptr;
	if (m_actToggleOrphanMask)
	{
		m_actToggleOrphanMask->setChecked(false); // triggers restore of primary
		m_actToggleOrphanMask->setEnabled(false);
	}

	// ------------------------------------------------------------------
	// Step 4: hide removed island surface actors
	// ------------------------------------------------------------------
	QSet<int> retainedLabels;
	for (const auto& isl : m_islands)
		if (!removeSet.count(isl.label))
			retainedLabels.insert(isl.label);

	applyIslandRetentionFilter(retainedLabels);

	// ------------------------------------------------------------------
	// Step 5: refresh VolumeView and SliceView
	// ------------------------------------------------------------------
	m_image = m_reslicedImage;
	ui->volumeView->setImageData(m_reslicedImage);
	ui->volumeView->updateData();

	const double level = std::isfinite(m_threshold)
		? m_threshold
		: 0.5 * (scalarMin + scalarMax);
	const double window = 2.0 * m_imageStats
		.value(QStringLiteral("stdDev")).toDouble(1.0);

	ui->volumeView->setColorWindowLevel(window, level);
	syncSliceView(m_reslicedImage, window, level);

	showProgressEnd();
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
	m_lastResliceAxes = nullptr;
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

	m_orphanMaskImage = nullptr;

	if (m_actToggleOrphanMask)
	{
		m_actToggleOrphanMask->setChecked(false);
		m_actToggleOrphanMask->setEnabled(false);
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
// onExport
//
// Full pipeline:
//   1. Inverse-reslice cleaned image back into original space.
//   2. PCA + rotate (reslice) the result so it is bone-aligned.
//   3. Recompute PCA on the rotated image to get the centroid in rotated space.
//   4. Find 6 surface landmark seeds on the rotated image.
//   5. Region-grow from seeds at baseline threshold.
//   6. Binarize the label mask: 0 = background, 255 = bone.
//   7. Locate the tight bounding box of the binarized mask.
//   8. Inflate the bounding box by a 10-voxel margin.
//   9. Crop the grayscale rotated image to the inflated VOI and export NIfTI.
//  10. Crop the binarized mask to the same inflated VOI and export NIfTI.
//
// Output files:
//   <sidecar_dir>/<crop_basename>_export_grayscale.nii
//   <sidecar_dir>/<crop_basename>_export_mask.nii
// ---------------------------------------------------------------------------
void PrototypeMainWindow::onExport()
{
	if (!m_reslicedImage || !m_originalImage ||
		!m_lastResliceAxes || !std::isfinite(m_threshold))
	{
		QMessageBox::warning(this, tr("Export Reslice"),
			tr("The export requires a completed Clean step.\n"
			"Run Initialize \u2192 Regions (or Graph Cut) \u2192 Clean first."));
		return;
	}

	showProgressStart();
	showProgressValue(5);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Step 1: project cleaned resliced image back into original space ───────
	const auto invResult = applyInverseResliceToOriginal();

	showProgressValue(15);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	if (!invResult)
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("Inverse reslice failed; export aborted."));
		showProgressEnd();
		return;
	}

	// ── Step 2a: PCA on the inverse-resliced result ───────────────────────────
	PrototypeHelpers::PcaResult exportPca;
	const bool pcaOk = PrototypeHelpers::computePca(
		invResult, m_threshold, exportPca, nullptr);

	if (!pcaOk || !exportPca.valid)
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("PCA failed on inverse-resliced image; export aborted."));
		showProgressEnd();
		return;
	}

	showProgressValue(25);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Step 2b: rotate the image using the PCA axes ──────────────────────────
	auto resliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
	resliceAxes->Identity();
	for (int row = 0; row < 3; ++row)
	{
		resliceAxes->SetElement(row, 0, exportPca.axes[0][row]);
		resliceAxes->SetElement(row, 1, exportPca.axes[1][row]);
		resliceAxes->SetElement(row, 2, exportPca.axes[2][row]);
		resliceAxes->SetElement(row, 3, exportPca.centroid[row]);
	}

	const double bgMean =
		m_imageStats.value(QStringLiteral("meanBg")).toDouble(0.0);

	auto resliceFilter = vtkSmartPointer<vtkImageReslice>::New();
	resliceFilter->SetInputData(invResult);
	resliceFilter->SetResliceAxes(resliceAxes);
	resliceFilter->SetInterpolationModeToCubic();
	resliceFilter->AutoCropOutputOn();
	resliceFilter->SetOutputDimensionality(3);
	resliceFilter->SetBackgroundLevel(bgMean);
	resliceFilter->Update();

	// ── Step 3: cache the grayscale rotated image ─────────────────────────────
	auto rotatedImage = vtkSmartPointer<vtkImageData>::New();
	rotatedImage->DeepCopy(resliceFilter->GetOutput());

	showProgressValue(35);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Step 3b: recompute PCA on the rotated image (centroid in rotated space)─
	PrototypeHelpers::PcaResult rotatedPca;
	const bool rotPcaOk = PrototypeHelpers::computePca(
		rotatedImage, m_threshold, rotatedPca, nullptr);

	if (!rotPcaOk || !rotatedPca.valid)
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("PCA failed on rotated image; export aborted."));
		showProgressEnd();
		return;
	}

	showProgressValue(40);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Step 4: find 6 surface landmark seeds on the rotated image ───────────
	std::array<std::array<double, 3>, 3> landmarkPos;
	std::array<std::array<double, 3>, 3> landmarkNeg;

	for (int i = 0; i < 3; ++i)
	{
		const double axisDirPos[3] = {
			rotatedPca.axes[i][0], rotatedPca.axes[i][1], rotatedPca.axes[i][2]
		};
		const double axisDirNeg[3] = {
			-rotatedPca.axes[i][0], -rotatedPca.axes[i][1], -rotatedPca.axes[i][2]
		};

		PrototypeHelpers::findSurfacePointFromBoundary(
			rotatedImage, rotatedPca.centroid, axisDirPos, m_threshold,
			landmarkPos[i].data());
		PrototypeHelpers::findSurfacePointFromBoundary(
			rotatedImage, rotatedPca.centroid, axisDirNeg, m_threshold,
			landmarkNeg[i].data());

		qDebug("onExport: landmark axis %d  +: (%.2f,%.2f,%.2f)  -: (%.2f,%.2f,%.2f)",
			i,
			landmarkPos[i][0], landmarkPos[i][1], landmarkPos[i][2],
			landmarkNeg[i][0], landmarkNeg[i][1], landmarkNeg[i][2]);
	}

	std::vector<std::array<double, 3>> seeds;
	seeds.reserve(6);
	for (int i = 0; i < 3; ++i)
	{
		seeds.push_back(landmarkPos[i]);
		seeds.push_back(landmarkNeg[i]);
	}

	showProgressValue(45);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Step 5: region-grow from seeds at baseline threshold ─────────────────
	const auto growProgress = [this](int pct)
		{
			showProgressValue(45 + static_cast<int>(pct * 0.2));
			m_progressBar->update();
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
		};

	vtkSmartPointer<vtkImageData> labelImage;

#ifdef PARALLEL_ISLANDS
	const auto islands = PrototypeHelpers::segmentBoneIslandsParallel(
		rotatedImage, m_threshold, seeds, labelImage, growProgress);
#else
	const auto islands = PrototypeHelpers::segmentBoneIslands(
		rotatedImage, m_threshold, seeds, labelImage, growProgress);
#endif

	if (!labelImage)
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("Region grow produced no label image; export aborted."));
		showProgressEnd();
		return;
	}

	showProgressValue(65);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Step 6: binarize the mask — 0 = background, 255 = bone ───────────────
	// Use the label image's own extent as the authoritative iteration domain.
	// GetDimensions() is avoided: it always returns non-negative counts from
	// [0,0,0] and would produce wrong flat indices for images with a non-zero
	// extent origin.
	int lblExtent[6];
	labelImage->GetExtent(lblExtent);

	const int lblNX = lblExtent[1] - lblExtent[0] + 1;
	const int lblNY = lblExtent[3] - lblExtent[2] + 1;
	const int lblNZ = lblExtent[5] - lblExtent[4] + 1;
	const vtkIdType totalVoxels =
		static_cast<vtkIdType>(lblNX) * lblNY * lblNZ;

	auto maskImage = vtkSmartPointer<vtkImageData>::New();
	maskImage->SetExtent(lblExtent);
	maskImage->SetSpacing(labelImage->GetSpacing());
	maskImage->SetOrigin(labelImage->GetOrigin());
	maskImage->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

	auto* maskPtr = static_cast<unsigned char*>(maskImage->GetScalarPointer());
	vtkDataArray* lblScalars = labelImage->GetPointData()->GetScalars();

	// Flat index is relative to the extent origin throughout.
	for (int k = lblExtent[4]; k <= lblExtent[5]; ++k)
		for (int j = lblExtent[2]; j <= lblExtent[3]; ++j)
			for (int i = lblExtent[0]; i <= lblExtent[1]; ++i)
			{
				const vtkIdType flat =
					static_cast<vtkIdType>(k - lblExtent[4]) * lblNY * lblNX
					+ static_cast<vtkIdType>(j - lblExtent[2]) * lblNX
					+ (i - lblExtent[0]);

				maskPtr[flat] =
					(lblScalars->GetTuple1(flat) > 0.0) ? 255u : 0u;
			}

	maskImage->Modified();

	showProgressValue(70);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Step 7: find tight bounding box of the binarized mask ────────────────
	int boundsMin[3] = { lblExtent[1], lblExtent[3], lblExtent[5] };
	int boundsMax[3] = { lblExtent[0], lblExtent[2], lblExtent[4] };
	bool anyForeground = false;

	for (int k = lblExtent[4]; k <= lblExtent[5]; ++k)
		for (int j = lblExtent[2]; j <= lblExtent[3]; ++j)
			for (int i = lblExtent[0]; i <= lblExtent[1]; ++i)
			{
				const vtkIdType flat = 
					static_cast<vtkIdType>(k - lblExtent[4]) * lblNY * lblNX
					+ static_cast<vtkIdType>(j - lblExtent[2]) * lblNX
					+ (i - lblExtent[0]);

				if (maskPtr[flat] == 0u)
					continue;

				anyForeground = true;
				if (i < boundsMin[0]) boundsMin[0] = i;
				if (j < boundsMin[1]) boundsMin[1] = j;
				if (k < boundsMin[2]) boundsMin[2] = k;
				if (i > boundsMax[0]) boundsMax[0] = i;
				if (j > boundsMax[1]) boundsMax[1] = j;
				if (k > boundsMax[2]) boundsMax[2] = k;
			}

	if (!anyForeground)
	{
		QMessageBox::critical(this, tr("Export Reslice"),
			tr("No foreground voxels in the binarized mask; export aborted."));
		showProgressEnd();
		return;
	}

	showProgressValue(75);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Step 8: inflate bounds by 10-voxel margin, clamped to the extent ─────
	constexpr int margin = 10;
	const int voiMinX = std::max(lblExtent[0], boundsMin[0] - margin);
	const int voiMinY = std::max(lblExtent[2], boundsMin[1] - margin);
	const int voiMinZ = std::max(lblExtent[4], boundsMin[2] - margin);
	const int voiMaxX = std::min(lblExtent[1], boundsMax[0] + margin);
	const int voiMaxY = std::min(lblExtent[3], boundsMax[1] + margin);
	const int voiMaxZ = std::min(lblExtent[5], boundsMax[2] + margin);

	qDebug("onExport: mask bounds [%d,%d,%d]-[%d,%d,%d]  "
		"VOI (margin=%d) [%d,%d,%d]-[%d,%d,%d]",
		boundsMin[0], boundsMin[1], boundsMin[2],
		boundsMax[0], boundsMax[1], boundsMax[2],
		margin,
		voiMinX, voiMinY, voiMinZ,
		voiMaxX, voiMaxY, voiMaxZ);

	// ── Step 9: crop grayscale rotated image and export NIfTI ─────────────────
	auto extractGray = vtkSmartPointer<vtkExtractVOI>::New();
	extractGray->SetInputData(rotatedImage);
	extractGray->SetVOI(voiMinX, voiMaxX, voiMinY, voiMaxY, voiMinZ, voiMaxZ);
	extractGray->Update();

	// ── Step 10: crop binarized mask and export NIfTI ─────────────────────────
	auto extractMask = vtkSmartPointer<vtkExtractVOI>::New();
	extractMask->SetInputData(maskImage);
	extractMask->SetVOI(voiMinX, voiMaxX, voiMinY, voiMaxY, voiMinZ, voiMaxZ);
	extractMask->Update();

	showProgressValue(85);
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);

	// ── Determine output paths ────────────────────────────────────────────────
	QString grayPath, maskPath;
	if (!m_sidecarPath.isEmpty() && !m_cropPath.isEmpty())
	{
		const QString cropBase = QFileInfo(m_cropPath).completeBaseName();
		const QString sidecarDir = QFileInfo(m_sidecarPath).absolutePath();
		grayPath = QDir(sidecarDir).filePath(
			cropBase + QStringLiteral("_export_grayscale.nii"));
		maskPath = QDir(sidecarDir).filePath(
			cropBase + QStringLiteral("_export_mask.nii"));
	}
	else
	{
		grayPath = QDir::temp().filePath(QStringLiteral("export_grayscale.nii"));
		maskPath = QDir::temp().filePath(QStringLiteral("export_mask.nii"));
	}

	// ── Write NIfTI files ─────────────────────────────────────────────────────
	const auto writeNii = [](vtkImageData* img, const QString& path)
		{
			auto writer = vtkSmartPointer<vtkNIFTIImageWriter>::New();
			writer->SetInputData(img);
			writer->SetFileName(path.toUtf8().constData());
			writer->Write();
		};

	writeNii(extractGray->GetOutput(), grayPath);
	writeNii(extractMask->GetOutput(), maskPath);

	showProgressEnd();

	qDebug("onExport: grayscale written to '%s'.", qUtf8Printable(grayPath));
	qDebug("onExport: mask written to '%s'.", qUtf8Printable(maskPath));

	statusBar()->showMessage(
		tr("Export saved: %1  |  %2").arg(grayPath).arg(maskPath), 8000);

	QMessageBox::information(this, tr("Export Reslice"),
		tr("Grayscale:\n%1\n\nMask:\n%2").arg(grayPath).arg(maskPath));
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

	// When the orphan mask overlay is active keep the SliceView pointing at
	// the mask — do not let an upstream image change replace it.
	if (m_actToggleOrphanMask && m_actToggleOrphanMask->isChecked())
		return;

	ui->sliceView->setImageData(image);
	ui->sliceView->updateData();

	if (image)
		ui->sliceView->setWindowLevelNative(window, level);
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

// ---------------------------------------------------------------------------
// Initialize slot — Reslice + Landmark in one step
//
// Runs the PCA reslice pass then immediately locates the surface landmark
// points on the resulting volume, advancing the workflow directly from Idle
// to Landmarked and enabling the Regions buttons in a single user action.
// ---------------------------------------------------------------------------
void PrototypeMainWindow::onInitialize()
{
	if (!m_pca.valid || !m_image || !std::isfinite(m_threshold))
	{
		qWarning("onInitialize: pre-conditions not met.");
		return;
	}

	qDebug("onInitialize: step 1 — Reslice.");
	onReslice();

	if (!m_reslicedImage)
	{
		qWarning("onInitialize: Reslice step produced no output; aborting.");
		return;
	}

	qDebug("onInitialize: step 2 — Landmark.");
	onLandmark();

	// ------------------------------------------------------------------
	// Step 3: pre-identify orphan islands.
	//
	// An unseeded 26-connected BFS labels every foreground component in
	// the resliced image at the current threshold.  Components that do
	// not contain any landmark seed point are written into m_orphanMaskImage
	// as a binary mask.  onClean() adds these voxels to its removal mask
	// so they are replaced with background noise even when the user has
	// not explicitly selected them in the island table.
	// ------------------------------------------------------------------
	qDebug("onInitialize: step 3 — orphan island identification.");

	// Collect the 6 landmark seed world positions populated by onLandmark().
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

	showProgressStart();
	const auto orphanProgress = [this](int pct)
		{
			showProgressValue(pct);
			m_progressBar->update();
			QCoreApplication::processEvents(
				QEventLoop::ExcludeUserInputEvents, 10);
		};

	m_orphanMaskImage = nullptr;
	PrototypeHelpers::identifyOrphanIslands(
		m_reslicedImage, m_threshold, seeds,
		m_orphanMaskImage, orphanProgress);

	showProgressEnd();

	// Enable the SliceView toggle only when the orphan mask was successfully built.
	if (m_actToggleOrphanMask)
		m_actToggleOrphanMask->setEnabled(m_orphanMaskImage != nullptr);

	qDebug("onInitialize: complete — workflow is now Landmarked.");
}

/*
vtkSmartPointer<vtkImageData>
PrototypeMainWindow::applyInverseResliceToOriginal() const
{
	if (!m_reslicedImage || !m_originalImage ||
		!m_pca.valid || !std::isfinite(m_threshold))
	{
		qWarning("applyInverseResliceToOriginal: pre-conditions not met.");
		return nullptr;
	}

	const int* origDims = m_originalImage->GetDimensions();
	const int* reslDims = m_reslicedImage->GetDimensions();

	vtkDataArray* origScalars = m_originalImage->GetPointData()->GetScalars();
	vtkDataArray* reslScalars = m_reslicedImage->GetPointData()->GetScalars();

	if (!origScalars || !reslScalars)
	{
		qWarning("applyInverseResliceToOriginal: scalar arrays missing.");
		return nullptr;
	}

	// Deep-copy of original image — output with selectively replaced voxels.
	auto output = vtkSmartPointer<vtkImageData>::New();
	output->DeepCopy(m_originalImage);
	vtkDataArray* outScalars = output->GetPointData()->GetScalars();

	vtkIdType replaced = 0;
	vtkIdType outOfRange = 0;

	int extent[6];
	m_originalImage->GetExtent(extent);

	int resliceExtent[6];
	m_reslicedImage->GetExtent(resliceExtent);

	for (int k = extent[4]; k <= extent[5]; ++k)
		for (int j = extent[2]; j <= extent[3]; ++j)
			for (int i = extent[0]; i <= extent[1]; ++i)
			{
				double value = m_originalImage->GetScalarComponentAsDouble(i, j, k, 0);

				// Skip background voxels — onClean() never touched them.
				if (value < m_threshold)
					continue;

				// ── 1. Original voxel index → physical world point ───────────────────
				// Both images share the same physical world space so this point is
				// directly usable as input to the resliced image's index transform.
				const double contIdx[3] = {
					static_cast<double>(i),
					static_cast<double>(j),
					static_cast<double>(k)
				};

				double physPt[3] = {};
				m_originalImage->TransformContinuousIndexToPhysicalPoint(contIdx, physPt);

				// ── 2. Physical world point → continuous index in resliced image ─────
				double reslContIdx[3] = {};
				m_reslicedImage->TransformPhysicalPointToContinuousIndex(physPt, reslContIdx);

				// ── 3. Round to nearest voxel and bounds-check ───────────────────────
				const int ri = static_cast<int>(std::lround(reslContIdx[0]));
				const int rj = static_cast<int>(std::lround(reslContIdx[1]));
				const int rk = static_cast<int>(std::lround(reslContIdx[2]));

				if (ri < resliceExtent[0] || ri > resliceExtent[1] ||
					rj < resliceExtent[2] || rj > resliceExtent[3] ||
					rk < resliceExtent[4] || rk > resliceExtent[5])
				{
					++outOfRange;
					continue;
				}

				// ── 4. Lookup cleaned resliced scalar ────────────────────────────────
				const double reslVal = m_reslicedImage->GetScalarComponentAsDouble(ri, rj, rk, 0);

				// ── 5. Replace if the resliced voxel was cleaned (noise < threshold) ─
				if (reslVal < m_threshold)
				{
					int ijk[3] = { i, j, k };
					vtkIdType outputId = output->ComputePointId(ijk);

					outScalars->SetTuple1(outputId, reslVal);
					++replaced;
				}
			}

	outScalars->Modified();
	output->Modified();

	qDebug("applyInverseResliceToOriginal: "
		   "%lld voxel(s) replaced  %lld out-of-reslice-range.",
		static_cast<long long>(replaced),
		static_cast<long long>(outOfRange));

	return output;
}
*/

vtkSmartPointer<vtkImageData>
PrototypeMainWindow::applyInverseResliceToOriginal() const
{
	if (!m_reslicedImage || !m_originalImage ||
		!m_lastResliceAxes || !std::isfinite(m_threshold))
	{
		qWarning("applyInverseResliceToOriginal: pre-conditions not met "
				 "(need reslicedImage, originalImage, lastResliceAxes and finite threshold).");
		return nullptr;
	}

	const int* origDims = m_originalImage->GetDimensions();
	const int* reslDims = m_reslicedImage->GetDimensions();

	vtkDataArray* origScalars = m_originalImage->GetPointData()->GetScalars();
	vtkDataArray* reslScalars = m_reslicedImage->GetPointData()->GetScalars();
	if (!origScalars || !reslScalars)
	{
		qWarning("applyInverseResliceToOriginal: scalar arrays missing.");
		return nullptr;
	}

	// ── Build IndexMatrix⁻¹ from vtkImageReslice::GetIndexMatrix() math ──────
	//
	// Forward IndexMatrix (built by onReslice()'s vtkImageReslice internally):
	//   IndexMatrix = inMatrix_orig × m_lastResliceAxes × outMatrix_resl
	//
	//   outMatrix_resl : resliced_index → resliced_physical
	//   m_lastResliceAxes : resliced_physical → original_physical
	//   inMatrix_orig  : original_physical → original_index
	//
	// We need the inverse:
	//   IndexMatrix⁻¹ = reslInMatrix × invResliceAxes × origOutMatrix
	//
	// Both matrices are built using the exact same element formulas from
	// GetIndexMatrix() in vtkImageReslice.cxx, applied to each image's
	// actual geometry (direction, spacing, origin) post-DeepCopy.

	auto buildOutMatrix = [](vtkImageData* img) -> vtkSmartPointer<vtkMatrix4x4>
		{
			const double* sp = img->GetSpacing();
			const double* org = img->GetOrigin();
			double dir[9];
			auto* dm = img->GetDirectionMatrix();
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					dir[3 * i + j] = dm->GetElement(i, j);

			auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
			mat->Zero();
			for (int i = 0; i < 3; ++i)
			{
				for (int j = 0; j < 3; ++j)
					mat->SetElement(i, j, dir[3 * i + j] * sp[j]);
				mat->SetElement(i, 3, org[i]);
			}
			mat->SetElement(3, 3, 1.0);
			return mat;
		};

	auto buildInMatrix = [](vtkImageData* img) -> vtkSmartPointer<vtkMatrix4x4>
		{
			const double* sp = img->GetSpacing();
			const double* org = img->GetOrigin();
			double dir[9], invDir[9];
			auto* dm = img->GetDirectionMatrix();
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					dir[3 * i + j] = dm->GetElement(i, j);
			vtkMatrix3x3::Invert(dir, invDir);

			auto mat = vtkSmartPointer<vtkMatrix4x4>::New();
			mat->Zero();
			for (int i = 0; i < 3; ++i)
			{
				double t = 0.0;
				for (int j = 0; j < 3; ++j)
				{
					mat->SetElement(i, j, invDir[3 * i + j] / sp[i]);
					t -= invDir[3 * i + j] * org[j] / sp[i];
				}
				mat->SetElement(i, 3, t);
			}
			mat->SetElement(3, 3, 1.0);
			return mat;
		};

	// origOutMatrix: original_index → original_physical
	const auto origOutMatrix = buildOutMatrix(m_originalImage);

	// invResliceAxes: original_physical → resliced_physical
	// Use m_lastResliceAxes — the matrix captured BEFORE setImage() overwrote
	// m_pca with the resliced-image PCA.  Rebuilding from m_pca here would use
	// the RESLICED image's PCA, not the axes that were actually applied.
	auto invResliceAxes = vtkSmartPointer<vtkMatrix4x4>::New();
	vtkMatrix4x4::Invert(m_lastResliceAxes, invResliceAxes);

	// reslInMatrix: resliced_physical → resliced_index
	const auto reslInMatrix = buildInMatrix(m_reslicedImage);

	// newIndexMatrix = reslInMatrix × invResliceAxes × origOutMatrix
	// Concatenation order mirrors GetIndexMatrix():
	//   SetMatrix(axes) → PreMultiply+Concatenate(out) → PostMultiply+Concatenate(in)
	auto xform = vtkSmartPointer<vtkTransform>::New();
	xform->SetMatrix(invResliceAxes);
	xform->PreMultiply();
	xform->Concatenate(origOutMatrix);
	xform->PostMultiply();
	xform->Concatenate(reslInMatrix);

	auto newIndexMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
	xform->GetMatrix(newIndexMatrix);

	qDebug("applyInverseResliceToOriginal: newIndexMatrix (origIdx → reslicedContIdx):");
	for (int r = 0; r < 4; ++r)
	{
		qDebug("  [ %10.5f  %10.5f  %10.5f  %10.5f ]",
			newIndexMatrix->GetElement(r, 0), newIndexMatrix->GetElement(r, 1),
			newIndexMatrix->GetElement(r, 2), newIndexMatrix->GetElement(r, 3));
	}

	// ── Deep-copy original; selectively overwrite cleaned bone voxels ─────────
	auto output = vtkSmartPointer<vtkImageData>::New();
	output->DeepCopy(m_originalImage);
	vtkDataArray* outScalars = output->GetPointData()->GetScalars();

	const int rNX = reslDims[0];
	const int rNY = reslDims[1];
	const int rNZ = reslDims[2];

	vtkIdType replaced = 0;
	vtkIdType outOfRange = 0;

	double origIdx[4] = { 0.0, 0.0, 0.0, 1.0 };
	double reslContIdx[4] = {};

	for (int k = 0; k < origDims[2]; ++k)
	{
		origIdx[2] = static_cast<double>(k);
		for (int j = 0; j < origDims[1]; ++j)
		{
			origIdx[1] = static_cast<double>(j);
			for (int i = 0; i < origDims[0]; ++i)
			{
				const vtkIdType origFlat =
					static_cast<vtkIdType>(k) * origDims[1] * origDims[0]
					+ static_cast<vtkIdType>(j) * origDims[0]
					+ i;

				if (origScalars->GetTuple1(origFlat) < m_threshold)
					continue;

				origIdx[0] = static_cast<double>(i);
				newIndexMatrix->MultiplyPoint(origIdx, reslContIdx);

				const int ri = static_cast<int>(std::lround(reslContIdx[0]));
				const int rj = static_cast<int>(std::lround(reslContIdx[1]));
				const int rk = static_cast<int>(std::lround(reslContIdx[2]));

				if (ri < 0 || ri >= rNX ||
					rj < 0 || rj >= rNY ||
					rk < 0 || rk >= rNZ)
				{
					++outOfRange;
					continue;
				}

				const vtkIdType reslFlat =
					static_cast<vtkIdType>(rk) * rNY * rNX
					+ static_cast<vtkIdType>(rj) * rNX
					+ ri;

				const double reslVal = reslScalars->GetTuple1(reslFlat);

				if (reslVal < m_threshold)
				{
					outScalars->SetTuple1(origFlat, reslVal);
					++replaced;
				}
			}
		}
	}

	outScalars->Modified();
	output->Modified();

	qDebug("applyInverseResliceToOriginal: "
		   "%lld voxel(s) replaced  %lld out-of-reslice-range.",
		static_cast<long long>(replaced),
		static_cast<long long>(outOfRange));

	return output;
}
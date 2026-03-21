#pragma once

#include <QMainWindow>
#include <QProgressBar>
#include <QJsonObject>

#include <vtkSmartPointer.h>

#include <array>
#include <limits>
#include <vector>

#include "PrototypeHelpers.h"

class vtkActor;
class vtkImageData;
class vtkMatrix4x4;
class vtkScalarBarActor;
class ImageLoader;
class vtkEventQtSlotConnect;
class QAction;

namespace Ui { class MainWindow; }

class PrototypeMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit PrototypeMainWindow(QWidget* parent = nullptr);
	~PrototypeMainWindow() override;

	void loadFromSidecar(const QString& sidecarPath);
	void loadFromSidecarAsync(const QString& sidecarPath);
	void setImage(vtkImageData* image);

	const QJsonObject& landmarkResult()  const { return m_landmarkResult; }
	const QJsonObject& landmarkJson()    const { return m_landmarkJson; }
	const QJsonObject& originalPcaJson() const { return m_originalPcaJson; }
	const QJsonObject& reslicedPcaJson() const { return m_reslicedPcaJson; }

protected:
	void closeEvent(QCloseEvent* event) override;

private slots:
	void onVtkStartEvent();
	void onVtkEndEvent();
	void onVtkProgressEvent();
	void showProgressStart();
	void showProgressValue(int percent);
	void showProgressEnd();
	void onLandmark();
	void onReslice();

	// Triggered by the "Regions" toolbar button.
	// Thresholds the resliced volume, runs a seeded 26-connected BFS flood-fill
	// from each landmark point, colours each island surface by voxel count using
	// a cool-to-warm transfer function, and shows a scalar bar when nIslands > 1.
	void onRegions();

	// Triggered by the "Regions Alt" toolbar button.
	// Runs the morphological pipeline (Gaussian smooth ? erode ? dilate ?
	// seeded vtkImageThresholdConnectivity) and displays the island surfaces
	// using the same colour/scalar-bar logic as onRegions().
	void onRegionsAlt();

	// Triggered by the "Clean" toolbar button.
	// Enabled only after segmentation (Regions or Regions Alt) has completed
	// and at least 8 Reslice steps have been performed.
	// Stub: disables all preceding workflow step buttons and advances to Cleaned.
	void onClean();

	void onOutlineToggled(bool checked);

	// Triggered by the "Restart" toolbar button.
	// Reverts the view to the original loaded image and its default PCA-positioned
	// axes, clears all reslice/landmark/region state, and resets the workflow
	// step buttons so only "Reslice" is enabled.
	void onRestart();

	// Triggered by the "Watershed" toolbar button.
	// Runs the ITK watershed pipeline (CurvatureFlow smooth ? binary threshold ?
	// signed distance map ? invert ? watershed) and displays the island surfaces
	// using the same colour/scalar-bar logic as onRegions() and onRegionsAlt().
	void onRegionsWatershed();

private:
	// -----------------------------------------------------------------------
	// Workflow step state machine
	// -----------------------------------------------------------------------

	// Linear workflow: Idle ? Resliced ? Landmarked ? Segmented ? Cleaned
	// Restart transitions back to Idle from any state.
	// Route A: Resliced ? Landmarked ? Segmented (via onRegions)
	// Route B: Resliced ? Landmarked ? Segmented (via onRegionsAlt)
	// Clean is available after Segmented when m_resliceCount >= 8.
	enum class WorkflowStep
	{
		Idle,        // image loaded; only Reslice enabled
		Resliced,    // reslice done; only Landmark enabled
		Landmarked,  // landmark done; Regions and RegionsAlt enabled
		Segmented,   // segmentation done; Clean enabled (when reslice count >= 8)
		Cleaned      // clean done; no step buttons enabled
	};

	// Transition to a new workflow step and update all step button states.
	// Called at the end of each successful operation and by onRestart().
	void setWorkflowStep(WorkflowStep step);

	// -----------------------------------------------------------------------
	// Private members
	// -----------------------------------------------------------------------

	Ui::MainWindow* ui = nullptr;

	QString m_sidecarPath;
	QString m_cropPath;

	double m_threshold = std::numeric_limits<double>::quiet_NaN();

	vtkImageData* m_image = nullptr;

	// Original image retained so Restart can restore it without re-loading from disk.
	vtkSmartPointer<vtkImageData> m_originalImage;

	vtkSmartPointer<ImageLoader>           m_imageLoader;
	vtkSmartPointer<vtkEventQtSlotConnect> m_vtkConnections;
	QProgressBar* m_progressBar = nullptr;

	// Checkable toolbar action for the bounding-box outline toggle.
	QAction* m_actOutline = nullptr;

	// Workflow step toolbar actions (owned by the toolbar / QObject parent chain).
	QAction* m_actReslice = nullptr;
	QAction* m_actLandmark = nullptr;
	QAction* m_actRegions = nullptr;
	QAction* m_actRegionsAlt = nullptr;
	// "Clean" toolbar button: enabled after segmentation completes and
	// at least 8 Reslice steps have been performed in this session.
	QAction* m_actClean = nullptr;
	QAction* m_actRestart = nullptr;
	QAction* m_actRegionsWatershed = nullptr;

	// Current workflow position — drives enabled/disabled state of step buttons.
	WorkflowStep m_workflowStep = WorkflowStep::Idle;

	// Cumulative count of successful Reslice operations in the current session.
	// Clean is only enabled when this reaches the required minimum (8).
	int m_resliceCount = 0;

	std::array<vtkSmartPointer<vtkActor>, 3> m_axisActors;
	std::array<vtkSmartPointer<vtkActor>, 6> m_tipActors;
	std::array<vtkSmartPointer<vtkActor>, 3> m_ringActors;
	vtkSmartPointer<vtkActor>                m_circumsphereActor;

	PrototypeHelpers::PcaResult m_pca;

	std::array<std::array<std::array<double, 3>, 2>, 3> m_landmarkPoints{};

	QJsonObject m_landmarkResult;
	QJsonObject m_landmarkJson;

	vtkSmartPointer<vtkImageData> m_reslicedImage;
	vtkSmartPointer<vtkImageData> m_labelImage;

	// BoneIsland vector from the most recent segmentation run.
	// Populated by applyIslandSegmentationResult(); cleared by clearIslandActors()
	// and onRestart().  Consumers (e.g. future export, stats, Clean) read this
	// rather than re-running segmentation.
	std::vector<PrototypeHelpers::BoneIsland> m_islands;

	// Island surface actors produced by the most recent onRegions() call.
	// Cleared at the start of each new onRegions() run.
	std::vector<vtkSmartPointer<vtkActor>> m_islandActors;

	// Scalar bar actor showing the voxel-count colour scale.
	// Non-null only when onRegions() produced more than one island.
	// Removed from the renderer and reset by clearIslandActors().
	vtkSmartPointer<vtkScalarBarActor> m_islandScalarBar;

	QJsonObject m_originalPcaJson;
	QJsonObject m_reslicedPcaJson;

	// Scalar statistics for the current image, computed once per setImage() call
	// via PrototypeHelpers::computeScalarThresholdStats().  Empty when no image
	// has been loaded or when m_threshold is not finite.
	// Keys provided by computeScalarThresholdStats():
	//   "mean"     / "stdDev"     — whole-volume statistics
	//   "meanFg"   / "stdDevFg"   — above-threshold (foreground) statistics
	//   "meanBg"   / "stdDevBg"   — below-threshold (background) statistics
	// Consumers read individual values via m_imageStats.value("key").toDouble().
	QJsonObject m_imageStats;

	static QJsonObject pcaResultToJson(const PrototypeHelpers::PcaResult& pca);
	QString prototypeOutputPath() const;
	bool    writePrototypeSidecar() const;
	void    clearPcaOverlay();

	// Removes all island surface actors and the scalar bar (if present)
	// from the VolumeView renderer and clears the internal vectors/pointers.
	void clearIslandActors();

	// Shared post-segmentation helper: builds actors, scalar bar, and updates
	// the landmark JSON cache from a completed island segmentation result.
	// Called by both onRegions() and onRegionsAlt() to avoid duplication.
	void applyIslandSegmentationResult(
		const std::vector<PrototypeHelpers::BoneIsland>& islands,
		vtkSmartPointer<vtkImageData>                    labelImage);
};
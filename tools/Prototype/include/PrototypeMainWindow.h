#pragma once

#include <QMainWindow>
#include <QProgressBar>
#include <QJsonObject>

#include <vtkSmartPointer.h>

#include <array>
#include <limits>
#include <vector>

#include "PrototypeHelpers.h"

class ImageLoader;

class QAction;

class vtkActor;
class vtkBillboardTextActor3D;
class vtkEventQtSlotConnect;
class vtkImageData;
class vtkMatrix4x4;

namespace Ui { class MainWindow; }

class PrototypeMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit PrototypeMainWindow(QWidget* parent = nullptr);
	~PrototypeMainWindow() override;

	void loadFromSidecar(const QString& sidecarPath);
	void loadFromSidecarAsync(const QString& sidecarPath);
	void setImage(vtkSmartPointer<vtkImageData> image);

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

	// Triggered by the "Regions" toolbar button.
	// Thresholds the resliced volume, runs a seeded 26-connected BFS flood-fill
	// from each landmark point, colours each island surface by voxel count using
	// a cool-to-warm transfer function.
	void onRegions();

#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	// Triggered by the "Graph Cut" toolbar button.
	// Builds foreground seed paths (centroid -> 5 landmark tips) and background
	// seed rays (outward from the same 5 tips, threshold-gated), then runs
	// ITK ImageGridCutFilter (GridCut multi-threaded solver) to segment bone
	// islands.  Results are displayed using the same colour logic as onRegions().
	void onRegionsGraphCut();
#endif // CTAXPROTOTYPE_ENABLE_GRAPH_CUT

	// Triggered by the "Clean" toolbar button.
	// Enabled only after segmentation (Regions, Regions Alt, or Graph Cut) has
	// completed and at least 8 Reslice steps have been performed.
	// Replaces above-threshold voxels outside every segmented island with
	// background noise, then VOI-crops the result and advances to Cleaned.
	void onClean();

	// Triggered by the "Restart" toolbar button.
	// Reverts the view to the original loaded image and its default PCA-positioned
	// axes, clears all reslice/landmark/region/graph-cut state, and resets the
	// workflow step buttons so only "Reslice" is enabled.
	void onRestart();

	// Open a json sidecar file and load the cropped image.
	void onFileOpen();

	void onInitialize();

	void onExportReslice();

private:
	// -----------------------------------------------------------------------
	// Workflow step state machine
	// -----------------------------------------------------------------------

	enum class WorkflowStep
	{
		Idle,
		Resliced,
		Landmarked,
		Segmented,
		Cleaned
	};

	void setWorkflowStep(WorkflowStep step);
	void onLandmark();
	void onReslice();

	// -----------------------------------------------------------------------
	// Private members
	// -----------------------------------------------------------------------

	Ui::MainWindow* ui = nullptr;

	QString m_sidecarPath;
	QString m_cropPath;

	double m_threshold = std::numeric_limits<double>::quiet_NaN();

	vtkSmartPointer<vtkImageData> m_image;
	vtkSmartPointer<vtkImageData> m_originalImage;

	vtkSmartPointer<ImageLoader>           m_imageLoader;
	vtkSmartPointer<vtkEventQtSlotConnect> m_vtkConnections;
	QProgressBar* m_progressBar = nullptr;

	QAction* m_actExportReslice = nullptr;
	QAction* m_actFile = nullptr;
	QAction* m_actInitialize = nullptr;
	QAction* m_actRegions = nullptr;
#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	QAction* m_actRegionsGraphCut = nullptr;
#endif
	QAction* m_actClean = nullptr;
	QAction* m_actRestart = nullptr;
	QAction* m_actToggleOrphanMask = nullptr;

	WorkflowStep m_workflowStep = WorkflowStep::Idle;

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
	vtkSmartPointer<vtkImageData> m_orphanMaskImage;
	vtkSmartPointer<vtkMatrix4x4> m_lastResliceAxes;

	std::vector<PrototypeHelpers::BoneIsland> m_islands;
	std::vector<vtkSmartPointer<vtkActor>>    m_islandActors;
#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	// m_graphCutSeedActors is always present; populated only when graph-cut
	// segmentation is enabled so clearGraphCutSeedActors() can remain
	// unconditional and always leaves it empty when the feature is off.
	std::vector<vtkSmartPointer<vtkActor>>    m_graphCutSeedActors;
	void    clearGraphCutSeedActors();
#endif

	QJsonObject m_originalPcaJson;
	QJsonObject m_reslicedPcaJson;

	QJsonObject m_imageStats;

	static QJsonObject pcaResultToJson(const PrototypeHelpers::PcaResult& pca);
	QString prototypeOutputPath() const;
	bool    writePrototypeSidecar() const;
	void    clearPcaOverlay();
	void    clearIslandActors();
	void    applyIslandSegmentationResult(
		const std::vector<PrototypeHelpers::BoneIsland>& islands,
		vtkSmartPointer<vtkImageData>                    labelImage);

	std::array<vtkSmartPointer<vtkBillboardTextActor3D>, 6> m_landmarkLabelActors;

	void alignCameraToMediumAxis();

	// Synchronise the SliceView (XY orientation) with the current image and
	// window/level whenever a new image is pushed to the VolumeView.
	void syncSliceView(vtkImageData* image, double window, double level);

	// Hides island surface actors whose labels are not in retainedLabels.
	void applyIslandRetentionFilter(const QSet<int>& retainedLabels);

	// Applies the inverse of the PCA reslice transform to map modified voxels
	// from the cleaned resliced image back into original image coordinate space.
	vtkSmartPointer<vtkImageData> applyInverseResliceToOriginal() const;
};
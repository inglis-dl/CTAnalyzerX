
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

	void onRegions();

#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	void onRegionsGraphCut();
#endif

	void onClean();
	void onRestart();
	void onFileOpen();
	void onInitialize();
	void onExport();

private:
	enum class WorkflowStep { Idle, Resliced, Landmarked, Segmented, Cleaned };

	void setWorkflowStep(WorkflowStep step);
	void onLandmark();
	void onReslice();

	Ui::MainWindow* ui = nullptr;

	QString m_sidecarPath;
	QString m_cropPath;

	double m_threshold = std::numeric_limits<double>::quiet_NaN();

	vtkSmartPointer<vtkImageData> m_image;
	vtkSmartPointer<vtkImageData> m_originalImage;

	vtkSmartPointer<ImageLoader>           m_imageLoader;
	vtkSmartPointer<vtkEventQtSlotConnect> m_vtkConnections;
	QProgressBar* m_progressBar = nullptr;

	QAction* m_actExport = nullptr;
	QAction* m_actFile = nullptr;
	QAction* m_actInitialize = nullptr;
	QAction* m_actRegions = nullptr;
#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	QAction* m_actRegionsGraphCut = nullptr;
#endif
	QAction* m_actClean = nullptr;
	QAction* m_actRestart = nullptr;

	WorkflowStep m_workflowStep = WorkflowStep::Idle;
	int          m_resliceCount = 0;

	PrototypeHelpers::PcaResult m_pca;

	std::array<std::array<std::array<double, 3>, 2>, 3> m_landmarkPoints{};

	QJsonObject m_landmarkResult;
	QJsonObject m_landmarkJson;

	vtkSmartPointer<vtkImageData> m_reslicedImage;
	vtkSmartPointer<vtkImageData> m_labelImage;
	vtkSmartPointer<vtkImageData> m_orphanMaskImage;
	vtkSmartPointer<vtkMatrix4x4> m_lastResliceAxes;

	std::vector<PrototypeHelpers::BoneIsland> m_islands;

	QJsonObject m_originalPcaJson;
	QJsonObject m_reslicedPcaJson;
	QJsonObject m_imageStats;

	static QJsonObject pcaResultToJson(const PrototypeHelpers::PcaResult& pca);
	QString prototypeOutputPath() const;
	bool    writePrototypeSidecar() const;

	// Aux prop key constants: centralise strings to avoid typos.
	static constexpr const char* kKeyPcaAxes = "pca_axes";
	static constexpr const char* kKeyPcaTips = "pca_tips";
	static constexpr const char* kKeyPcaRings = "pca_rings";
	static constexpr const char* kKeyLandmarkLabels = "landmark_labels";
	static constexpr const char* kKeyGraphCutSeeds = "graphcut_seeds";
	// Per-island keys are formed as "island_<label>" at call sites.

	// Remove all PCA axis/tip/ring props and landmark labels from the VolumeView.
	void clearPcaOverlay();
	// Remove all island surface props from the VolumeView.
	void clearIslandActors();
#ifdef CTAXPROTOTYPE_ENABLE_GRAPH_CUT
	void clearGraphCutSeedActors();
#endif

	void applyIslandSegmentationResult(
		const std::vector<PrototypeHelpers::BoneIsland>& islands,
		vtkSmartPointer<vtkImageData>                    labelImage);

	void alignCameraToMediumAxis();
	void syncSliceView(vtkImageData* image, double window, double level);
	void applyIslandRetentionFilter(const QSet<int>& retainedLabels);
	vtkSmartPointer<vtkImageData> applyInverseResliceToOriginal() const;
};
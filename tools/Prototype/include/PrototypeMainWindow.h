#pragma once

#include <QMainWindow>
#include <QProgressBar>
#include <QJsonObject>

#include <vtkSmartPointer.h>

#include <array>
#include <limits>

class vtkActor;
class vtkImageData;
class vtkMatrix4x4;
class ImageLoader;
class vtkEventQtSlotConnect;

namespace Ui { class MainWindow; }

class PrototypeMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit PrototypeMainWindow(QWidget* parent = nullptr);
	~PrototypeMainWindow() override;

	// Reads the sidecar JSON at `sidecarPath`, caches the threshold value,
	// loads the referenced crop image, and calls setImage().
	void loadFromSidecar(const QString& sidecarPath);

	// Schedules loadFromSidecar() to run after the event loop starts,
	// so the window is visible and can show progress during the load.
	void loadFromSidecarAsync(const QString& sidecarPath);

	// Sets the image on the volume view and applies window/level derived from
	// the image data: level = cached threshold, window = scalar standard deviation.
	void setImage(vtkImageData* image);

	// Returns the most recently computed landmark result (may be empty if
	// onLandmark() has not yet been called successfully).
	const QJsonObject& landmarkResult() const { return m_landmarkResult; }

	// Returns the landmark JSON cache built after the most recent onLandmark() call.
	// Contains the original PCA, resliced PCA (if performed), and landmark points.
	// Empty until onLandmark() has completed successfully at least once.
	const QJsonObject& landmarkJson() const { return m_landmarkJson; }

	// Returns the PCA result JSON for the original (pre-reslice) volume.
	// Populated by the first setImage() call (i.e. on load from sidecar).
	// Empty until a successful PCA has been computed on the original image.
	const QJsonObject& originalPcaJson() const { return m_originalPcaJson; }

	// Returns the PCA result JSON for the resliced volume.
	// Populated by onReslice() after the resliced image has been visualised.
	// Empty until onReslice() has completed successfully.
	const QJsonObject& reslicedPcaJson() const { return m_reslicedPcaJson; }

	// PCA result cached from the last successful setImage() call.
	// Used by onLandmark() without re-running PCA.
	struct PcaResult
	{
		double centroid[3];    // binary-volume centroid in world coordinates
		double axes[3][3];     // eigenvectors as rows: axes[i] = i-th principal axis (unit vector)
		double eigenvalues[3]; // eigenvalues in descending order
		double circumRadius;   // radius of the circumsphere (fits outside the bounding box)
		bool   valid = false;  // set to true once a successful computePca() result is stored
	};

protected:
	// Intercept the window-close event to flush the prototype sidecar to disk
	// before the application exits.
	void closeEvent(QCloseEvent* event) override;

private slots:
	void onVtkStartEvent();
	void onVtkEndEvent();
	void onVtkProgressEvent();
	void showProgressStart();
	void showProgressValue(int percent);
	void showProgressEnd();

	// Triggered by the "Landmark" toolbar button.
	// Searches along each PCA axis from the centroid outward (+/-) to find the
	// first below-threshold transition, relocates the axis-tip sphere actors to
	// those surface points, and caches all results in m_landmarkResult and
	// m_landmarkJson.
	void onLandmark();

	// Triggered by the "Reslice" toolbar button.
	// Passes the original volume through vtkImageReslice with axes aligned to
	// the PCA eigenvectors and centroid origin, then visualises the result.
	void onReslice();

private:
	Ui::MainWindow* ui = nullptr;

	// Paths cached from the most recent loadFromSidecar() call.
	// Used to derive the prototype output sidecar path on close.
	QString m_sidecarPath; // absolute path of the source project sidecar (.json)
	QString m_cropPath;    // absolute path of the crop image referenced by the sidecar

	// Threshold cached from the sidecar JSON; used as the WL level in setImage().
	double m_threshold = std::numeric_limits<double>::quiet_NaN();

	// Cached raw image pointer (non-owning; owned by m_imageLoader pipeline).
	vtkImageData* m_image = nullptr;

	vtkSmartPointer<ImageLoader>           m_imageLoader;
	vtkSmartPointer<vtkEventQtSlotConnect> m_vtkConnections;
	QProgressBar* m_progressBar = nullptr;

	// PCA overlay actors (axis shafts + tip spheres). Cleared and rebuilt each setImage().
	// 3 axis lines + 6 sphere tip actors (both ends per axis) + 3 circumsphere ring actors.
	std::array<vtkSmartPointer<vtkActor>, 3> m_axisActors;
	std::array<vtkSmartPointer<vtkActor>, 6> m_tipActors;
	std::array<vtkSmartPointer<vtkActor>, 3> m_ringActors;
	vtkSmartPointer<vtkActor>                m_circumsphereActor;

	PcaResult m_pca;

	// Surface landmark points found by onLandmark() (world space).
	// Indexed as [axis 0..2][direction 0=positive, 1=negative][xyz].
	std::array<std::array<std::array<double, 3>, 2>, 3> m_landmarkPoints{};

	// JSON cache of the last landmark computation result (per-axis raw data).
	QJsonObject m_landmarkResult;

	// Consolidated JSON cache written after each successful onLandmark() call.
	// Combines the original PCA, the resliced PCA (if present), and the landmark
	// points into a single object that is flushed to disk on application close.
	QJsonObject m_landmarkJson;

	// Resliced volume produced by onReslice() (smart-pointer keeps it alive while displayed).
	vtkSmartPointer<vtkImageData> m_reslicedImage;

	// JSON cache of the PCA result computed on the original (pre-reslice) image.
	// Written once by setImage() when m_reslicedImage is null (i.e. original load).
	// Never overwritten by subsequent reslice calls, so callers can always
	// retrieve the starting-point PCA regardless of how many reslices have run.
	QJsonObject m_originalPcaJson;

	// JSON cache of the PCA result computed on the resliced image.
	// Written by onReslice() after setImage() returns for the resliced volume.
	QJsonObject m_reslicedPcaJson;

	// Serialises a PcaResult to a QJsonObject using the same field names
	// used in the landmark result JSON for consistency.
	static QJsonObject pcaResultToJson(const PcaResult& pca);

	// Builds the prototype output sidecar path from the crop image basename:
	//   <sidecar_directory>/<crop_basename>_prototype.json
	// Returns an empty string when m_sidecarPath or m_cropPath are not set.
	QString prototypeOutputPath() const;

	// Writes the accumulated JSON cache (originalPca, reslicedPca, landmarks)
	// to the prototype sidecar file.  Logs a warning and returns false on failure.
	// Does nothing and returns true when there is nothing worth writing yet.
	bool writePrototypeSidecar() const;

	// Removes all previously added PCA overlay actors from the VolumeView renderer.
	void clearPcaOverlay();
};
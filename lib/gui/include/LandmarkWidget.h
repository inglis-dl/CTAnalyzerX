#pragma once

#include <QWidget>
#include <QVector>
#include <QJsonArray>
#include <vtkSmartPointer.h>

namespace Ui { class LandmarkWidget; }

class LightboxWidget;
class SliceView;
class LandmarkHelper;
class QPushButton;
class QSlider;
class QLineEdit;
class QLabel;
class vtkPolyData;

/// Widget providing UI controls for landmark/landmark placement in a lightbox view.
/// Coordinates with LandmarkHelper to manage a single shared vtkSeedWidget across multiple SliceView instances.
class LandmarkWidget : public QWidget
{
	Q_OBJECT

public:
	explicit LandmarkWidget(QWidget* parent = nullptr);
	~LandmarkWidget() override;

	/// Install the Lightbox so the widget can coordinate landmarks across all views
	void setLightbox(LightboxWidget* lightbox);

	/// Programmatic load/save helpers (sidecar JSON format)
	QJsonArray currentLandmarksAsJson() const;
	void loadLandmarksFromJson(const QJsonArray& arr);

signals:
	/// Emitted when landmarks change (array of {x,y,z,defined})
	void landmarksChanged(const QJsonArray& landmarks);

	/// Emitted when all three landmarks have been defined (true) or not (false)
	void placingComplete(bool complete);

	/// Request save through workflow state machine
	void saveLandmarksRequested(const QJsonArray& landmarks);

public slots:
	/// UI slots
	void onDefineToggled(bool on);
	void onDeleteClicked();
	void onResetClicked();
	void onSaveClicked();

	/// Update slider/editor ranges when image extents change
	void updateExtents(int xMin, int xMax, int yMin, int yMax, int zMin, int zMax);

private:
	struct Fid {
		bool defined = false;
		double x = 0.0, y = 0.0, z = 0.0;
		QString label;
		QColor color;
	};

	void init();
	void updateUiFromCurrent();
	void updateVolumeRepresentation();
	void emitState();
	void updateControlStates();

	/// Connect focus/selection events from views to helper
	void setupViewConnections();

	LightboxWidget* m_lightbox = nullptr;
	LandmarkHelper* m_helper = nullptr; // Single shared helper
	QVector<Fid> m_fids; // Local model (size 3)

	// UI controls
	QPushButton* m_btnDefine = nullptr;
	QPushButton* m_btnSave = nullptr;
	QPushButton* m_btnDelete = nullptr;
	QPushButton* m_btnReset = nullptr;
	QSlider* m_sliders[3];      // X, Y, Z
	QLineEdit* m_edits[3];      // X, Y, Z
	QLabel* m_lblStatus = nullptr;

	// VTK: polydata for volume view representation
	vtkSmartPointer<vtkPolyData> m_fidPolyData;

	// The ui instance created by uic (resources/LandmarkWidget.ui -> ui_LandmarkWidget.h)
	Ui::LandmarkWidget* ui = nullptr;

private slots:
	/// Coordinator signals -> LandmarkWidget model updates
	void handleLandmarkPlaced(int index, double x, double y, double z);
	void handleLandmarkMoved(int index, double x, double y, double z);
	void handleLandmarkDeleted(int remainingCount);

	/// View selection changes -> activate coordinator on that view
	void handleViewSelected(SliceView* view);

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;
};

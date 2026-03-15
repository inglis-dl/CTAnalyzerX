#pragma once

#include <QColor>
#include <QMap>
#include <QObject>
#include <QVector>

#include <vtkSmartPointer.h>

class vtkObject;
class SliceView;
class vtkSeedWidget;
class vtkSeedRepresentation;
class vtkImageSlicePointPlacer;
class vtkEventQtSlotConnect;
class vtkPointHandleRepresentation2D;
class vtkLandmarkActor;

/// Coordinates a SINGLE vtkSeedWidget shared among multiple SliceView instances.
/// Dynamically switches the widget's interactor and updates handle visibility 
/// based on which slice each landmark intersects.
/// Maintains persistent shadow actors in all views for continuous visual feedback.
class LandmarkHelper : public QObject
{
	Q_OBJECT
		Q_PROPERTY(int maxLandmarks READ maxLandmarks WRITE setMaxLandmarks NOTIFY maxLandmarksChanged)
		Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)

public:
	struct Landmark {
		bool defined = false;
		double x = 0.0, y = 0.0, z = 0.0; // World coordinates
		QString label;
		QColor color = QColor(255, 0, 0);
	};

	explicit LandmarkHelper(QObject* parent = nullptr);
	~LandmarkHelper() override;

	/// Register slice views (must be called before enabling)
	void registerView(SliceView* view);
	void unregisterView(SliceView* view);

	/// Enable/disable landmark interaction (attaches to focused view)
	void setEnabled(bool on);
	bool isEnabled() const { return m_enabled; }

	/// Set which view currently has the seed widget
	void setActiveView(SliceView* view);
	SliceView* activeView() const { return m_activeView; }

	/// Landmark management
	int maxLandmarks() const { return m_maxLandmarks; }
	void setMaxLandmarks(int max);

	int landmarkCount() const;

	void setLandmarkWorldPosition(int idx, double x, double y, double z);
	bool getLandmarkWorldPosition(int idx, double& x, double& y, double& z) const;

	void reset();

	/// Set whether coordinator should respect view selection (default: true)
	void setRespectSelection(bool respect) { m_respectSelection = respect; }
	bool respectsSelection() const { return m_respectSelection; }

signals:
	void landmarkPlaced(int index, double x, double y, double z);
	void landmarkMoved(int index, double x, double y, double z);
	void landmarkDeleted(int remainingCount);
	void maxLandmarksChanged(int max);
	void enabledChanged(bool enabled);

protected:
	/// Update which handles are visible in each view based on slice intersection
	void updateHandleVisibility();

	/// Check if a world point intersects a view's current slice (within tolerance)
	bool landmarkIntersectsSlice(const Landmark& lm, SliceView* view) const;

	/// Landmark actor management
	void updateActorsForLandmark(int landmarkIdx);
	void removeActorsForLandmark(int landmarkIdx);
	vtkLandmarkActor* getLandmarkActor(SliceView* view, int landmarkIdx);

	/// Update landmark actor in a specific view only (more granular control)
	void updateActorInView(SliceView* view, int landmarkIdx);  // ? ADD THIS

private slots:
	void onSeedPlaced(vtkObject* sender, unsigned long, void*, void*);
	void onSeedMoved(vtkObject* sender, unsigned long, void*, void*);
	void onSeedDeleted(vtkObject* sender, unsigned long, void*, void*);

	/// Called when any view's slice changes
	void onViewSliceChanged(int sliceIndex);

	/// Called when a view is destroyed
	void onViewDestroyed(QObject* obj);

	/// React to view selection changes
	void onViewSelectionChanged(bool selected);

private:
	void setupSeedWidget();
	void attachWidgetToView(SliceView* view);
	void detachWidgetFromView();

	// Single shared seed widget
	vtkSmartPointer<vtkSeedWidget> m_seedWidget;
	vtkSmartPointer<vtkSeedRepresentation> m_seedRepresentation;
	vtkSmartPointer<vtkEventQtSlotConnect> m_qvtkConnection;

	QVector<SliceView*> m_views;
	SliceView* m_activeView = nullptr;

	QVector<Landmark> m_landmarks;
	int m_maxLandmarks = 3;
	bool m_enabled = false;
	bool m_updatingFromExternal = false;

	// Tolerance for slice intersection (in world units)
	double m_sliceIntersectionTolerance = 0.5;

	bool m_respectSelection = true; // Honor selection gating by default

	// landmark actors: view -> (landmarkIdx -> actor)
	QMap<SliceView*, QMap<int, vtkSmartPointer<vtkLandmarkActor>>> m_landmarkActors;
};

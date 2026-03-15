#include "LandmarkHelper.h"
#include "SliceView.h"
#include "vtkImageSlicePointPlacer.h"
#include "vtkLandmarkActor.h"

#include <QDebug>

#include <vtkCommand.h>
#include <vtkEventQtSlotConnect.h>
#include <vtkGlyphSource2D.h>
#include <vtkImageData.h>
#include <vtkPointHandleRepresentation2D.h>
#include <vtkPointPlacer.h>
#include <vtkProperty2D.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>
#include <vtkSeedRepresentation.h>
#include <vtkSeedWidget.h>

LandmarkHelper::LandmarkHelper(QObject* parent)
	: QObject(parent)
{
	m_landmarks.resize(m_maxLandmarks);
	m_qvtkConnection = vtkSmartPointer<vtkEventQtSlotConnect>::New();
	setupSeedWidget();
}

LandmarkHelper::~LandmarkHelper()
{
	if (m_seedWidget && m_seedWidget->GetEnabled()) {
		m_seedWidget->Off();
	}
	m_qvtkConnection->Disconnect();
}

void LandmarkHelper::setupSeedWidget()
{
	// Create representation
	m_seedRepresentation = vtkSmartPointer<vtkSeedRepresentation>::New();

	vtkNew<vtkGlyphSource2D> glyph;
	glyph->SetGlyphTypeToThickCross();
	glyph->SetRotationAngle(45);
	glyph->SetScale(20);
	glyph->Update();

	vtkNew<vtkPointHandleRepresentation2D> handle;
	handle->SetCursorShape(glyph->GetOutput());
	handle->GetProperty()->SetColor(1.0, 0.0, 0.0);
	m_seedRepresentation->SetHandleRepresentation(handle);

	// Create widget (interactor set dynamically)
	m_seedWidget = vtkSmartPointer<vtkSeedWidget>::New();
	m_seedWidget->SetRepresentation(m_seedRepresentation);
	m_seedWidget->KeyPressActivationOff();

	// Connect VTK events
	m_qvtkConnection->Connect(m_seedWidget, vtkCommand::PlacePointEvent,
							 this, SLOT(onSeedPlaced(vtkObject*, unsigned long, void*, void*)));

	m_qvtkConnection->Connect(m_seedWidget, vtkCommand::EndInteractionEvent,
							 this, SLOT(onSeedMoved(vtkObject*, unsigned long, void*, void*)));

	m_qvtkConnection->Connect(m_seedWidget, vtkCommand::DeletePointEvent,
							 this, SLOT(onSeedDeleted(vtkObject*, unsigned long, void*, void*)));
}

void LandmarkHelper::registerView(SliceView* view)
{
	if (!view || m_views.contains(view)) return;

	m_views.append(view);

	// CRITICAL: Initialize actor storage BEFORE any operations
	if (!m_landmarkActors.contains(view)) {
		m_landmarkActors[view] = QMap<int, vtkSmartPointer<vtkLandmarkActor>>();
	}

	// Connect to slice change to update visibility
	connect(view, &SliceView::sliceChanged,
			this, &LandmarkHelper::onViewSliceChanged,
			Qt::UniqueConnection);

	// Connect to selection changes
	connect(view, &SelectionFrameWidget::selectedChanged,
			this, &LandmarkHelper::onViewSelectionChanged,
			Qt::UniqueConnection);

	// Track destruction
	connect(view, &QObject::destroyed,
			this, &LandmarkHelper::onViewDestroyed);

	// Create actors for existing landmarks
	for (int i = 0; i < m_landmarks.size(); ++i) {
		if (m_landmarks[i].defined) {
			updateActorsForLandmark(i);
		}
	}
}

void LandmarkHelper::unregisterView(SliceView* view)
{
	if (!view) return;

	if (m_activeView == view) {
		detachWidgetFromView();
		m_activeView = nullptr;
	}

	// Remove all landmark actors from this view BEFORE removing from list
	if (m_landmarkActors.contains(view)) {
		auto& actorMap = m_landmarkActors[view];
		for (auto it = actorMap.begin(); it != actorMap.end(); ++it) {
			if (it.value() && view->renderer()) {
				view->renderer()->RemoveViewProp(it.value());
			}
		}
		m_landmarkActors.remove(view);
	}

	m_views.removeAll(view);
	disconnect(view, nullptr, this, nullptr);
}

void LandmarkHelper::setEnabled(bool on)
{
	if (m_enabled == on) return;

	m_enabled = on;

	if (on) {
		// Attach to active view if available
		if (m_activeView) {
			attachWidgetToView(m_activeView);
		}
		else if (!m_views.isEmpty()) {
			// Default to first view
			setActiveView(m_views.first());
		}
	}
	else {
		detachWidgetFromView();
	}

	emit enabledChanged(on);
}

void LandmarkHelper::onViewSelectionChanged(bool selected)
{
	if (!m_respectSelection) return; // Feature disabled
	if (!m_enabled) return; // Coordinator not active

	auto* view = qobject_cast<SliceView*>(sender());
	if (!view) return;

	if (selected) {
		// View was selected -> make it the active view for seed widget
		setActiveView(view);
	}
	else {
		// View was deselected
		if (m_activeView == view) {
			// If it was the active view, detach widget temporarily
			detachWidgetFromView();
			m_activeView = nullptr;

			// Optional: auto-switch to another selected view if available
			for (SliceView* v : m_views) {
				if (v != view && v->isSelected()) {
					setActiveView(v);
					break;
				}
			}
		}
	}
}

void LandmarkHelper::onViewSliceChanged(int sliceIndex)
{
	Q_UNUSED(sliceIndex);

	// When slice changes, update visibility of all landmarks in that view
	SliceView* view = qobject_cast<SliceView*>(sender());
	if (!view) return;

	// Update landmark actors in this specific view
	for (int i = 0; i < m_landmarks.size(); ++i) {
		if (m_landmarks[i].defined) {
			updateActorInView(view, i);
		}
	}

	// Also update seed widget handles if this is the active view
	if (view == m_activeView) {
		updateHandleVisibility();
	}
}

// LandmarkHelper.cpp
void LandmarkHelper::setActiveView(SliceView* view)
{
	// Allow explicit nullptr to detach widget
	if (view && m_activeView == view) return; // Only early-return if same non-null view

	if (m_respectSelection && view && !view->isSelected()) {
		qDebug() << "Cannot activate unselected view";
		return;
	}

	SliceView* previousView = m_activeView; // Store previous for landmark actor updates

	detachWidgetFromView(); // Always detach first

	if (view) {
		m_activeView = view;
		if (m_enabled) {
			attachWidgetToView(view);
		}
	}
	else {
		m_activeView = nullptr; // Allow explicit null to clear active view
	}

	// Update landmark actor visibility in both old and new active views
	// In the old view, landmark actors should now become visible
	// In the new view, landmark actors should be hidden (seed widget shows them)
	if (previousView) {
		for (int i = 0; i < m_landmarks.size(); ++i) {
			if (m_landmarks[i].defined) {
				updateActorInView(previousView, i);
			}
		}
	}

	if (m_activeView) {
		for (int i = 0; i < m_landmarks.size(); ++i) {
			if (m_landmarks[i].defined) {
				updateActorInView(m_activeView, i);
			}
		}
	}

	updateHandleVisibility();
}

// New helper: Update landmark actor in a specific view (more granular than updateActorsForLandmark)
void LandmarkHelper::updateActorInView	(SliceView* view, int landmarkIdx)
{
	if (!view || landmarkIdx < 0 || landmarkIdx >= m_landmarks.size()) return;
	if (!m_landmarks[landmarkIdx].defined) return;

	vtkLandmarkActor* actor = getLandmarkActor(view, landmarkIdx);
	if (!actor) return;

	const Landmark& lm = m_landmarks[landmarkIdx];

	// Update position
	double pos[3] = { lm.x, lm.y, lm.z };
	actor->SetWorldPosition(pos);

	// Update appearance
	actor->SetColor(lm.color.redF(), lm.color.greenF(), lm.color.blueF());

	// Use point placer-based visibility check
	bool visible = false;
	if (view != m_activeView) {
		visible = landmarkIntersectsSlice(lm, view);
	}

	actor->SetVisibility(visible);

	// Ensure viewport is updated
	if (view->renderer()) {
		actor->SetViewport(view->renderer());
	}

	// Request render
	if (view->renderWindow()) {
		view->renderWindow()->Render();
	}
}

bool LandmarkHelper::landmarkIntersectsSlice(const Landmark& lm, SliceView* view) const
{
	if (!lm.defined || !view) return false;

	// NEW: Leverage the view's point placer if available
	if (auto* placer = view->pointPlacer()) {
		double worldPos[3] = { lm.x, lm.y, lm.z };

		// Use the placer's validation to determine if point is on current slice
		// ValidateWorldPosition returns 1 if valid, 0 if not
		int valid = placer->ValidateWorldPosition(worldPos);
		return valid != 0;
	}

	// FALLBACK: Use manual calculation if no placer
	auto* image = view->imageData();
	if (!image) return false;

	const int w = static_cast<int>(view->viewOrientation());
	const int sliceIdx = view->getSliceIndex();
	const double* origin = image->GetOrigin();
	const double* spacing = image->GetSpacing();
	const double sliceWorldCoord = origin[w] + sliceIdx * spacing[w];

	double landmarkCoord;
	switch (w) {
		case 0: landmarkCoord = lm.x; break;
		case 1: landmarkCoord = lm.y; break;
		case 2:
		default: landmarkCoord = lm.z; break;
	}

	const double distance = std::abs(landmarkCoord - sliceWorldCoord);
	return distance <= m_sliceIntersectionTolerance;
}

void LandmarkHelper::setLandmarkWorldPosition(int idx, double x, double y, double z)
{
	if (idx < 0 || idx >= m_maxLandmarks) return;
	if (!m_seedWidget || !m_seedRepresentation) return;

	m_updatingFromExternal = true;

	// Create seeds up to idx
	while (m_seedRepresentation->GetNumberOfSeeds() <= idx) {
		if (m_seedRepresentation->GetNumberOfSeeds() >= m_maxLandmarks) break;
		m_seedWidget->CreateNewHandle();
	}

	if (idx < m_seedRepresentation->GetNumberOfSeeds()) {
		double pos[3] = { x, y, z };
		m_seedRepresentation->SetSeedWorldPosition(idx, pos);

		// Update model
		if (idx < m_landmarks.size()) {
			m_landmarks[idx].defined = true;
			m_landmarks[idx].x = x;
			m_landmarks[idx].y = y;
			m_landmarks[idx].z = z;
		}

		// Update landmark actors in all views
		updateActorsForLandmark(idx);
		updateHandleVisibility();
	}

	m_updatingFromExternal = false;
}

void LandmarkHelper::reset()
{
	if (!m_seedRepresentation) return;

	// Clear all seeds
	const int count = m_seedRepresentation->GetNumberOfSeeds();
	for (int i = count - 1; i >= 0; --i) {
		m_seedWidget->DeleteSeed(i);
	}

	// Clear model and remove all landmark actors
	for (int i = 0; i < m_landmarks.size(); ++i) {
		if (m_landmarks[i].defined) {
			m_landmarks[i].defined = false;
			removeActorsForLandmark(i);
		}
	}

	if (m_enabled && m_seedWidget) {
		m_seedWidget->RestartInteraction();
	}

	// Render all views to reflect the cleared state
	for (SliceView* view : m_views) {
		if (view && view->renderWindow()) {
			view->renderWindow()->Render();
		}
	}
}

void LandmarkHelper::onSeedPlaced(vtkObject*, unsigned long, void*, void*)
{
	if (m_updatingFromExternal || !m_seedRepresentation) return;

	int count = m_seedRepresentation->GetNumberOfSeeds();
	if (count > m_maxLandmarks) {
		m_seedWidget->DeleteSeed(count - 1);
		return;
	}

	const int idx = count - 1;
	double pos[3];
	m_seedRepresentation->GetSeedWorldPosition(idx, pos);

	// Update model
	if (idx < m_landmarks.size()) {
		m_landmarks[idx].defined = true;
		m_landmarks[idx].x = pos[0];
		m_landmarks[idx].y = pos[1];
		m_landmarks[idx].z = pos[2];

		// Set default label if not already set
		if (m_landmarks[idx].label.isEmpty()) {
			m_landmarks[idx].label = QString("Landmark %1").arg(idx + 1);
		}
	}

	emit landmarkPlaced(idx, pos[0], pos[1], pos[2]);

	// Create and update landmark actors in all views
	updateActorsForLandmark(idx);

	// Update visibility of seed widget handles
	updateHandleVisibility();

	if (count >= m_maxLandmarks) {
		m_seedWidget->CompleteInteraction();
	}
}

void LandmarkHelper::onSeedMoved(vtkObject*, unsigned long, void*, void*)
{
	if (m_updatingFromExternal || !m_seedRepresentation) return;

	int activeHandle = m_seedRepresentation->GetActiveHandle();
	if (activeHandle < 0 || activeHandle >= m_seedRepresentation->GetNumberOfSeeds()) {
		return;
	}

	double pos[3];
	m_seedRepresentation->GetSeedWorldPosition(activeHandle, pos);

	// Update model
	if (activeHandle < m_landmarks.size()) {
		m_landmarks[activeHandle].x = pos[0];
		m_landmarks[activeHandle].y = pos[1];
		m_landmarks[activeHandle].z = pos[2];
	}

	emit landmarkMoved(activeHandle, pos[0], pos[1], pos[2]);

	// Update landmark actors in all views with new position
	updateActorsForLandmark(activeHandle);

	// Update visibility of seed widget handles
	updateHandleVisibility();
}

void LandmarkHelper::onSeedDeleted(vtkObject*, unsigned long, void*, void*)
{
	if (m_updatingFromExternal || !m_seedRepresentation) return;

	const int remainingCount = m_seedRepresentation->GetNumberOfSeeds();

	// Mark landmarks beyond remainingCount as undefined and remove landmark actors
	for (int i = remainingCount; i < m_landmarks.size(); ++i) {
		if (m_landmarks[i].defined) {
			m_landmarks[i].defined = false;
			removeActorsForLandmark(i);
		}
	}

	emit landmarkDeleted(remainingCount);

	updateHandleVisibility();

	if (remainingCount < m_maxLandmarks) {
		m_seedWidget->RestartInteraction();
	}
}

// New method: Get or create landmark actor
vtkLandmarkActor* LandmarkHelper::getLandmarkActor(SliceView* view, int landmarkIdx)
{
	if (!view || landmarkIdx < 0 || landmarkIdx >= m_maxLandmarks) {
		return nullptr;
	}

	// Ensure map exists
	if (!m_landmarkActors.contains(view)) {
		m_landmarkActors[view] = QMap<int, vtkSmartPointer<vtkLandmarkActor>>();
	}

	auto& actorMap = m_landmarkActors[view];
	if (!actorMap.contains(landmarkIdx)) {
		// Create new shadow actor
		vtkSmartPointer<vtkLandmarkActor> actor =
			vtkSmartPointer<vtkLandmarkActor>::New();

		// Set viewport for coordinate transformation
		auto* renderer = view->renderer();
		if (renderer) {
			actor->SetViewport(renderer);
		}

		// DON'T set landmark properties here - let updateActorInView handle it
		// This ensures properties are always current

		// Add to renderer (initially invisible)
		actor->SetVisibility(false);
		if (renderer) {
			renderer->AddViewProp(actor);
		}

		actorMap[landmarkIdx] = actor;
	}

	return actorMap[landmarkIdx];
}

// New method: Update landmark actors for a landmark
void LandmarkHelper::updateActorsForLandmark(int landmarkIdx)
{
	if (landmarkIdx < 0 || landmarkIdx >= m_landmarks.size()) return;
	if (!m_landmarks[landmarkIdx].defined) return;

	const Landmark& lm = m_landmarks[landmarkIdx];
	double pos[3] = { lm.x, lm.y, lm.z };

	// Track which views need rendering
	QSet<SliceView*> viewsToRender;

	// Update each view's landmark actor
	for (SliceView* view : m_views) {
		if (!view) continue;

		vtkLandmarkActor* actor = getLandmarkActor(view, landmarkIdx);
		if (!actor) continue;

		// Update position
		actor->SetWorldPosition(pos);

		// Update color (in case it changed)
		actor->SetColor(lm.color.redF(), lm.color.greenF(), lm.color.blueF());

		// Determine visibility
		bool wasVisible = actor->GetVisibility() != 0;
		bool visible = false;
		if (view != m_activeView) {
			visible = landmarkIntersectsSlice(lm, view);
		}

		actor->SetVisibility(visible);

		// Only render if visibility changed or actor is visible
		if (visible || wasVisible != visible) {
			viewsToRender.insert(view);
		}
	}

	// Render only views that need it
	for (SliceView* view : viewsToRender) {
		if (view && view->renderWindow()) {
			view->renderWindow()->Render();
		}
	}
}

// New method: Remove landmark actors
void LandmarkHelper::removeActorsForLandmark(int landmarkIdx)
{
	// Remove actor from all views
	for (SliceView* view : m_views) {
		if (!view || !m_landmarkActors.contains(view)) continue;

		auto& actorMap = m_landmarkActors[view];
		if (actorMap.contains(landmarkIdx)) {
			vtkLandmarkActor* actor = actorMap[landmarkIdx];

			// CRITICAL: Check renderer exists before removing
			auto* renderer = view->renderer();
			if (actor && renderer) {
				renderer->RemoveViewProp(actor);
			}
			actorMap.remove(landmarkIdx);

			// Request render to reflect removal
			auto* renderWindow = view->renderWindow();
			if (renderWindow) {
				renderWindow->Render();
			}
		}
	}
}

void LandmarkHelper::attachWidgetToView(SliceView* view)
{
	if (!view || !m_seedWidget) return;

	auto* iren = view->renderWindow()->GetInteractor();
	auto* renderer = view->renderer();

	if (!iren || !renderer) return;

	// Configure widget for this view
	m_seedWidget->SetInteractor(iren);
	m_seedWidget->SetDefaultRenderer(renderer);
	m_seedWidget->SetCurrentRenderer(renderer);

	// Set point placer for this view
	if (auto* pointPlacer = view->pointPlacer()) {
		if (auto* handleRep = vtkPointHandleRepresentation2D::SafeDownCast(
			m_seedRepresentation->GetHandleRepresentation())) {
			handleRep->SetPointPlacer(static_cast<vtkPointPlacer*>(pointPlacer));
		}
	}

	m_seedWidget->On();

	// Restart or complete interaction based on landmark count
	if (landmarkCount() >= m_maxLandmarks) {
		m_seedWidget->CompleteInteraction();
	}
	else {
		m_seedWidget->RestartInteraction();
	}
}

void LandmarkHelper::detachWidgetFromView()
{
	if (m_seedWidget && m_seedWidget->GetEnabled()) {
		m_seedWidget->Off();
	}
}

void LandmarkHelper::updateHandleVisibility()
{
	if (!m_seedRepresentation) return;

	const int handleCount = m_seedRepresentation->GetNumberOfSeeds();

	// 1. Update seed widget handles (only visible in active view on correct slice)
	for (int i = 0; i < handleCount && i < m_landmarks.size(); ++i) {
		if (!m_landmarks[i].defined) continue;

		auto* handleRep = vtkPointHandleRepresentation2D::SafeDownCast(
			m_seedRepresentation->GetHandleRepresentation(i));

		if (!handleRep) continue;

		// Handle is visible only if landmark intersects active view's slice
		bool visible = false;
		if (m_activeView) {
			visible = landmarkIntersectsSlice(m_landmarks[i], m_activeView);
		}

		handleRep->SetVisibility(visible);
	}

	// 2. Update landmark actors in ALL views (including non-active)
	for (int i = 0; i < m_landmarks.size(); ++i) {
		if (m_landmarks[i].defined) {
			updateActorsForLandmark(i);
		}
	}

	// 3. Request render on active view for seed widget
	if (m_activeView && m_activeView->renderWindow()) {
		m_activeView->renderWindow()->Render();
	}
}

void LandmarkHelper::onViewDestroyed(QObject* obj)
{
	auto* view = static_cast<SliceView*>(obj);
	unregisterView(view);
}

void LandmarkHelper::setMaxLandmarks(int max)
{
	if (max < 0) max = 0;
	if (m_maxLandmarks == max) return;

	// If reducing max landmarks, remove excess
	if (max < m_maxLandmarks) {
		for (int i = max; i < m_landmarks.size(); ++i) {
			if (m_landmarks[i].defined) {
				m_landmarks[i].defined = false;
				removeActorsForLandmark(i);
			}
		}

		// Remove excess seeds from widget
		if (m_seedRepresentation) {
			const int seedCount = m_seedRepresentation->GetNumberOfSeeds();
			for (int i = seedCount - 1; i >= max; --i) {
				m_seedWidget->DeleteSeed(i);
			}
		}
	}

	m_maxLandmarks = max;
	m_landmarks.resize(max);

	emit maxLandmarksChanged(max);
}

int LandmarkHelper::landmarkCount() const
{
	int count = 0;
	for (const auto& lm : m_landmarks) {
		if (lm.defined) ++count;
	}
	return count;
}

bool LandmarkHelper::getLandmarkWorldPosition(int idx, double& x, double& y, double& z) const
{
	if (idx < 0 || idx >= m_landmarks.size()) return false;
	if (!m_landmarks[idx].defined) return false;

	x = m_landmarks[idx].x;
	y = m_landmarks[idx].y;
	z = m_landmarks[idx].z;
	return true;
}
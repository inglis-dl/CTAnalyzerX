#pragma once

#include "CollapsibleGroupBox.h"
#include "WorkflowStateMachine.h"

namespace Ui { class WorkflowPanelWidget; }

#include <QPointer>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

class ImageInfoWidget;
class LandmarkWidget;
class LightboxWidget;
class WindowLevelWidget;

class WorkflowPanelWidget : public QWidget
{
	Q_OBJECT

public:
	explicit WorkflowPanelWidget(QWidget* parent = nullptr);
	~WorkflowPanelWidget() override;

	// Helpers for controller to enable/disable workflow groups
	void setCroppingEnabled(bool on);
	bool isCroppingEnabled() const;

	void setLandmarkingEnabled(bool on);
	bool isLandmarkingEnabled() const;

	// Appearance group is logically always available; this is kept for API compatibility.
	void setWindowLevellingEnabled(bool on);
	bool isWindowLevellingEnabled() const;

	// Accessors for widgets defined in the .ui
	WindowLevelWidget* windowLevelWidget() const;
	ImageInfoWidget* imageInfoWidget() const;
	LandmarkWidget* landmarkWidget() const;

	// Let the panel own/drive real-time cropping updates to a LightboxWidget.
	// Panel does NOT take ownership of the lightbox; it merely connects its
	// cropping signal to the LightboxWidget's handler.
	void setLightboxWidget(LightboxWidget* lightbox);

	// Fine-grained control for cropping workflow
	void setSaveCroppedEnabled(bool on);

signals:
	// High-level workflow actions driven by the panel controls
	void defineCropRequested();
	void saveCroppedRequested(); // request to save cropped volume (user chooses path)

	// Forwarded cropping region (real-time updates from CropWidget)
	void croppingRegionChanged(int xMin, int xMax,
							   int yMin, int yMax,
							   int zMin, int zMax);

	void placeLandmarksRequested();

	void windowLevelAdjusted();

	void resetCropRequested();

public slots:
	void notifyWorkflowRestored(WorkflowStateMachine::State restoredState);

protected:
	void resizeEvent(QResizeEvent* event) override;

private:
	void init();
	void adjustGroupWidths();

	// Layout roots
	QVBoxLayout* m_rootLayout = nullptr;
	QScrollArea* m_scrollArea = nullptr;
	QWidget* m_scrollContent = nullptr;

	// Group boxes defined in the .ui
	CollapsibleGroupBox* m_grpImageInfo = nullptr;
	CollapsibleGroupBox* m_grpCrop = nullptr;
	CollapsibleGroupBox* m_grpLandmark = nullptr;
	CollapsibleGroupBox* m_grpWindowLevel = nullptr;

	// Panel-owned reference to Lightbox (not owner)
	QPointer<LightboxWidget> m_lightbox;

	// The ui instance created by uic (resources/WorkflowPanelWidget.ui -> ui_WorkflowPanelWidget.h)
	Ui::WorkflowPanelWidget* ui = nullptr;
};
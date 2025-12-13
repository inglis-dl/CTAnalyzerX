#pragma once

#include <QWidget>
#include <QPointer>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include "CollapsibleGroupBox.h"
#include "ImageProcessingStateMachine.h" // added for State

class WorkflowPanelWidget : public QWidget
{
	Q_OBJECT
public:
	explicit WorkflowPanelWidget(QWidget* parent = nullptr);

	// Helpers for controller to enable/disable workflow groups
	void setCroppingEnabled(bool on);
	bool isCroppingEnabled() const;

	void setRotationEnabled(bool on);
	bool isRotationEnabled() const;

	void setSegmentationEnabled(bool on);
	bool isSegmentationEnabled() const;

	void setFiducialsEnabled(bool on);
	bool isFiducialsEnabled() const;

	void setAppearanceEnabled(bool on);
	bool isAppearanceEnabled() const;

	// New: centralize expand/collapse + enable/disable policy driven by state machine
	// Accepts the ImageProcessingStateMachine::State and a flag indicating whether
	// a valid image is present. This method both enables/disables the workflow
	// groups and updates their collapsed/expanded visual state.
	void applyState(ImageProcessingStateMachine::State s, bool imagePresent);

	// Insert real widgets into the placeholders (ownership is NOT transferred;
	// the widget will be reparented to the placeholder area).
	void insertVolumeCroppingWidget(QWidget* widget);
	void insertFiducialsWidget(QWidget* widget);
	void insertVolumeRotationWidget(QWidget* widget);
	void insertSegmentationWidget(QWidget* widget);
	void insertAppearanceWidget(QWidget* widget);

	// Fine-grained control helpers for cropping workflow
	void setDefineCropEnabled(bool on);
	void setApplyCropEnabled(bool on);
	void setSaveCroppedEnabled(bool on);

signals:
	// High-level workflow actions driven by the panel controls
	void loadImageRequested();
	void defineCropRequested();
	void applyCropRequested();
	void loadCroppedRequested();
	void saveCroppedRequested(); // new: request to save cropped volume (user chooses path)

	void placeFiducialsRequested();
	void startInteractiveRotationRequested();
	void applyRotationRequested();

	void computeThresholdRequested();
	void previewThresholdRequested();
	void runSegmentationRequested();
	void saveSegmentRequested();

	// Appearance only (non-workflow)
	void windowLevelAdjusted();

private slots:
	// UI handlers for placeholder buttons
	void onLoadImageClicked();
	void onDefineCropClicked();
	void onApplyCropClicked();
	void onLoadCroppedClicked();
	void onSaveCroppedClicked();

	void onPlaceFiducialsClicked();
	void onStartInteractiveRotationClicked();
	void onApplyRotationClicked();

	void onComputeThresholdClicked();
	void onPreviewThresholdClicked();
	void onRunSegmentationClicked();
	void onSaveSegmentClicked();

private:
	// Helper to build group boxes
	CollapsibleGroupBox* makeGroup(const QString& title);

	// Layout roots
	QVBoxLayout* m_rootLayout = nullptr;
	QScrollArea* m_scrollArea = nullptr;
	QWidget* m_scrollContent = nullptr;

	// Groups + placeholder containers
	CollapsibleGroupBox* m_grpLoad = nullptr;
	QWidget* m_loadContainer = nullptr;
	QPushButton* m_btnLoadImage = nullptr;

	CollapsibleGroupBox* m_grpCropping = nullptr;
	QWidget* m_croppingContainer = nullptr;
	QPushButton* m_btnDefineCrop = nullptr;
	QPushButton* m_btnApplyCrop = nullptr;
	QPushButton* m_btnSaveCropped = nullptr; // new
	QPushButton* m_btnLoadCropped = nullptr;

	CollapsibleGroupBox* m_grpFiducials = nullptr;
	QWidget* m_fiducialsContainer = nullptr;
	QPushButton* m_btnPlaceFiducials = nullptr;

	CollapsibleGroupBox* m_grpRotation = nullptr;
	QWidget* m_rotationContainer = nullptr;
	QPushButton* m_btnStartInteractiveRotation = nullptr;
	QPushButton* m_btnApplyRotation = nullptr;

	CollapsibleGroupBox* m_grpSegmentation = nullptr;
	QWidget* m_segmentationContainer = nullptr;
	QPushButton* m_btnComputeThreshold = nullptr;
	QPushButton* m_btnPreviewThreshold = nullptr;
	QPushButton* m_btnRunSegmentation = nullptr;
	QPushButton* m_btnSaveSegment = nullptr;

	CollapsibleGroupBox* m_grpAppearance = nullptr;
	QWidget* m_appearanceContainer = nullptr;

	// Small informative labels
	QLabel* makePlaceholderLabel(const QString& text);

	// Keep pointers if caller inserted special widgets
	QPointer<QWidget> m_customCroppingWidget;
	QPointer<QWidget> m_customFiducialsWidget;
	QPointer<QWidget> m_customRotationWidget;
	QPointer<QWidget> m_customSegmentationWidget;
	QPointer<QWidget> m_customAppearanceWidget;

	// Recompute and apply group widths to fit the scroll area's viewport
	void adjustGroupWidths();

protected:
	void resizeEvent(QResizeEvent* event) override;
};
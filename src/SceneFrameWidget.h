#pragma once

#include "SelectionFrameWidget.h"

class vtkRenderWindow;
class QTimer;
class QAction;
class QMenu;
class QSettings;

class SceneFrameWidget : public SelectionFrameWidget
{
	Q_OBJECT
		Q_PROPERTY(SceneFrameWidget::ViewOrientation viewOrientation READ viewOrientation WRITE setViewOrientation NOTIFY viewOrientationChanged)
		Q_PROPERTY(bool allowReOrientation2D READ allowReOrientation2D WRITE setAllowReOrientation2D)
		Q_PROPERTY(bool allowReOrientation3D READ allowReOrientation3D WRITE setAllowReOrientation3D)
		Q_PROPERTY(bool allowVerticalViewFlipping READ allowVerticalViewFlipping WRITE setAllowVerticalViewFlipping)
		Q_PROPERTY(bool allowHorizontalViewFlipping READ allowHorizontalViewFlipping WRITE setAllowHorizontalViewFlipping)
		Q_PROPERTY(bool allowClockwiseViewRotation READ allowClockwiseViewRotation WRITE setAllowClockwiseViewRotation)
		Q_PROPERTY(bool allowCounterClockwiseViewRotation READ allowCounterClockwiseViewRotation WRITE setAllowCounterClockwiseViewRotation)
		// New runtime behavior toggles
		Q_PROPERTY(bool interactiveMode READ interactiveMode WRITE setInteractiveMode)
		Q_PROPERTY(bool useContextMenu READ useContextMenu WRITE setUseContextMenu)

public:
	enum ViewOrientation { VIEW_ORIENTATION_YZ = 0, VIEW_ORIENTATION_XZ = 1, VIEW_ORIENTATION_XY = 2 };
	Q_ENUM(ViewOrientation)

		explicit SceneFrameWidget(QWidget* parent = nullptr);
	~SceneFrameWidget() override = default;

	// Rendering entry point
	Q_INVOKABLE void render();

	// Orientation API (now strongly-typed)
	ViewOrientation viewOrientation() const { return m_viewOrientation; }
	virtual void setViewOrientation(ViewOrientation orientation);
	Q_INVOKABLE void setViewOrientationToXY() { setViewOrientation(VIEW_ORIENTATION_XY); }
	Q_INVOKABLE void setViewOrientationToYZ() { setViewOrientation(VIEW_ORIENTATION_YZ); }
	Q_INVOKABLE void setViewOrientationToXZ() { setViewOrientation(VIEW_ORIENTATION_XZ); }

	// Camera/view manipulation (override in derived classes as needed)
	Q_INVOKABLE virtual void flipHorizontal();
	Q_INVOKABLE virtual void flipVertical();
	Q_INVOKABLE virtual void flip(int on);
	Q_INVOKABLE virtual void rotateCamera(double degrees);
	Q_INVOKABLE virtual void orthogonalizeView();
	virtual void resetCamera() {} // subclasses implement if available

	// “Allow” flags akin to vtkKWSceneFrame
	bool allowReOrientation2D() const { return m_allowReOrientation2D; }
	void setAllowReOrientation2D(bool v) { m_allowReOrientation2D = v; updateActionEnableStates(); }

	bool allowReOrientation3D() const { return m_allowReOrientation3D; }
	void setAllowReOrientation3D(bool v) { m_allowReOrientation3D = v; updateActionEnableStates(); }

	bool allowVerticalViewFlipping() const { return m_allowVerticalViewFlipping; }
	void setAllowVerticalViewFlipping(bool v) { m_allowVerticalViewFlipping = v; updateActionEnableStates(); }

	bool allowHorizontalViewFlipping() const { return m_allowHorizontalViewFlipping; }
	void setAllowHorizontalViewFlipping(bool v) { m_allowHorizontalViewFlipping = v; updateActionEnableStates(); }

	bool allowClockwiseViewRotation() const { return m_allowClockwiseViewRotation; }
	void setAllowClockwiseViewRotation(bool v) { m_allowClockwiseViewRotation = v; updateActionEnableStates(); }

	bool allowCounterClockwiseViewRotation() const { return m_allowCounterClockwiseViewRotation; }
	void setAllowCounterClockwiseViewRotation(bool v) { m_allowCounterClockwiseViewRotation = v; updateActionEnableStates(); }

	// New toggles
	bool interactiveMode() const { return m_interactiveMode; }
	void setInteractiveMode(bool v) { m_interactiveMode = v; }

	bool useContextMenu() const { return m_useContextMenu; }
	void setUseContextMenu(bool v);

	// Menu/actions and shortcuts
	void createDefaultMenuAndActions();
	void wireShortcuts();

	// Persistence
	void saveState(QSettings& s, const QString& keyPrefix) const;
	void restoreState(QSettings& s, const QString& keyPrefix);

signals:
	void viewOrientationChanged(SceneFrameWidget::ViewOrientation);

protected:
	// Derived classes must return the render window so render() can function.
	virtual vtkRenderWindow* getRenderWindow() const { return nullptr; } // safe default

	// Helper to install the scene content into the SelectionFrameWidget body.
	void setSceneContent(QWidget* content) { setCentralWidget(content); }

	// Capability hints for enabling actions (subclasses may restrict)
	virtual bool canFlipHorizontal() const { return true; }
	virtual bool canFlipVertical() const { return true; }
	virtual bool canRotate() const { return true; }

	// Keep actions in sync with flags/capabilities
	void updateActionEnableStates();

	// Grant access to derived classes
	ViewOrientation  m_viewOrientation = VIEW_ORIENTATION_XY;

private:
	// Internal helpers
	void handleOrientationSelected(const QString& item);
	void showContextMenuAtCursor();

	// Map orientation <-> label
	QString orientationLabel(ViewOrientation orient) const;
	ViewOrientation labelToOrientation(const QString& label) const;

private:
	bool m_allowReOrientation2D = true;
	bool m_allowReOrientation3D = true;
	bool m_allowVerticalViewFlipping = true;
	bool m_allowHorizontalViewFlipping = true;
	bool m_allowClockwiseViewRotation = true;
	bool m_allowCounterClockwiseViewRotation = true;

	// New state
	bool m_interactiveMode = true;
	bool m_useContextMenu = true;

	// Actions and menu
	QAction* m_actXY = nullptr;
	QAction* m_actYZ = nullptr;
	QAction* m_actXZ = nullptr;
	QAction* m_actFlipH = nullptr;
	QAction* m_actFlipV = nullptr;
	QAction* m_actRotCw = nullptr;
	QAction* m_actRotCcw = nullptr;
	QAction* m_actReset = nullptr;
};

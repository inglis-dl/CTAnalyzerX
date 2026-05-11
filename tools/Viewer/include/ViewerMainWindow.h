#pragma once

#include <QMainWindow>
#include <QProgressBar>
#include <QPointer>
#include <QSplitter>

#include <vtkSmartPointer.h>

class ImageLoader;
class ImageInfoWidget;
class LightboxWidget;
class VolumePlanesWidget;
class WindowLevelWidget;
class vtkEventQtSlotConnect;
class vtkImageData;

//
// ViewerMainWindow
// ----------------
// Minimal standalone 3-D image viewer.
// Left panel: ImageInfoWidget + ScalarOpacityFunctionWidget
// Right panel: LightboxWidget (slice + volume views)
// No workflow, no state machine, no cropping, no landmarks.
//
class ViewerMainWindow : public QMainWindow
{
	Q_OBJECT

public:
	explicit ViewerMainWindow(QWidget* parent = nullptr);
	~ViewerMainWindow() override;

protected:
	void showEvent(QShowEvent* event) override;
	void closeEvent(QCloseEvent* event) override;
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;

private slots:
	void onActionOpen();
	void onActionScreenshot();
	void onActionExit();
	void onActionAbout();
	void onVtkStartEvent();
	void onVtkEndEvent();
	void onVtkProgressEvent();
	void showProgressStart();
	void showProgressValue(int percent);
	void showProgressEnd();

private:
	void buildUi();
	void buildMenus();
	void wireConnections();
	void openFile(const QString& filePath);
	void addToRecentFiles(const QString& filePath);
	void updateRecentFilesMenu();
	void readSettings();
	void writeSettings();

	// ── Widgets ──────────────────────────────────────────────────────────────
	LightboxWidget*              m_lightbox              = nullptr;
	QWidget*                     m_lightboxPlaceholder   = nullptr;
	QSplitter*                   m_splitter              = nullptr;
	ImageInfoWidget*             m_imageInfo             = nullptr;
	WindowLevelWidget*           m_windowLevel           = nullptr;
	VolumePlanesWidget*          m_volumePlanes          = nullptr;
	QProgressBar*                m_progressBar           = nullptr;
	QMenu*                       m_recentFilesMenu       = nullptr;

	// ── VTK / loader ─────────────────────────────────────────────────────────
	vtkSmartPointer<ImageLoader>            m_imageLoader;
	vtkSmartPointer<vtkEventQtSlotConnect>  m_vtkConnections;

	// ── State ─────────────────────────────────────────────────────────────────
	QStringList m_recentFiles;
};
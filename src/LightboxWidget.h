#ifndef LIGHTBOXWIDGET_H
#define LIGHTBOXWIDGET_H

#include <QWidget>
#include <QVTKOpenGLNativeWidget.h>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>

#include <QSlider>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QShortcut>
#include <QMenuBar>
#include <QAction>
#include <QPixmap>

class SliceView;
class VolumeView;

class LightBoxWidget : public QWidget {
	Q_OBJECT
public:
	explicit LightBoxWidget(QWidget* parent = nullptr);

	void setImageData(vtkImageData* image);
	void setAxialSlice(int index);
	void setSagittalSlice(int index);
	void setCoronalSlice(int index);
	QPixmap grabFramebuffer();

public slots:
	void updateCropping();


private:
	QGridLayout* layout;
	SliceView* axialView;
	SliceView* sagittalView;
	SliceView* coronalView;
	VolumeView* volumeView;

	QGroupBox* createCroppingControls();
	QSlider* axialMinSlider, * axialMaxSlider;
	QSlider* sagittalMinSlider, * sagittalMaxSlider;
	QSlider* coronalMinSlider, * coronalMaxSlider;

	QLabel* axialLabel, * sagittalLabel, * coronalLabel;
	QPushButton* presetFullVolume, * presetCenterROI, * presetReset;

	QCheckBox* slicePlaneToggle;
	QShortcut* shortcutFullVolume;
	QShortcut* shortcutCenterROI;
	QShortcut* shortcutResetROI;
	QLabel* shortcutHelpLabel;

	QMenuBar* menuBar;
	QAction* actionFullVolume;
	QAction* actionCenterROI;
	QAction* actionResetROI;
	QAction* actionToggleSlicePlanes;


	void setupViews();
	void connectSliceSynchronization();
	void connectCroppingControls();
};

#endif // LIGHTBOXWIDGET_H

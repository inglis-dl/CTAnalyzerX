#pragma once

#include <QWidget>
#include <memory>

class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QComboBox;

class vtkImageData;
class vtkAlgorithmOutput;

#include "ImageResliceHelper.h"

// Simple widget to control yaw/pitch/roll rotation (degrees) with optional live update.
class VolumeRotationWidget : public QWidget
{
	Q_OBJECT
	Q_PROPERTY(int downsampleFactor READ downsampleFactor WRITE setDownsampleFactor NOTIFY downsampleFactorChanged)
public:
	explicit VolumeRotationWidget(QWidget* parent = nullptr);

	double yaw() const;
	double pitch() const;
	double roll() const;
	bool liveUpdate() const;

	// Set input data for the internal reslice helper
	void SetInputData(vtkImageData* img);
	// Set input connection for the internal reslice helper
	void SetInputConnection(vtkAlgorithmOutput* port);

	// Downsample control (1 == none, 2, 4)
	int downsampleFactor() const;
	void setDownsampleFactor(int factor);

signals:
	// finished==true indicates user completed interaction (e.g., release/enter)
	void rotationChanged(double yaw, double pitch, double roll, bool finished);

	// Emitted when a resliced image is available (may be null if reslicing failed)
	void resliceReady(vtkImageData* image);
	// Alternative: emit output port when caller prefers pipeline connection
	void reslicePortReady(vtkAlgorithmOutput* port);

	void downsampleFactorChanged(int factor);

	// Emitted when the user clicked Apply and the reslice update completed
	void resliceApplied();

private slots:
	void onValueChanged(double);
	void onEditingFinished();
	void onResetClicked();
	void onApplyDownsampleClicked();

private:
	void updateReslice(bool finished);

	QDoubleSpinBox* m_yaw = nullptr;
	QDoubleSpinBox* m_pitch = nullptr;
	QDoubleSpinBox* m_roll = nullptr;
	QCheckBox* m_live = nullptr;
	QPushButton* m_reset = nullptr;
	bool m_inProgrammaticUpdate = false;

	// internal reslice helper to perform image reslicing based on current rotation
	std::unique_ptr<ImageResliceHelper> m_resliceHelper;

	// Downsample UI
	QComboBox* m_downsampleCombo = nullptr;
	QPushButton* m_applyDownsample = nullptr;
	int m_downsampleFactor = 1;
};

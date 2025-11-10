#pragma once

#include <QWidget>
#include <memory>

class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QComboBox;

class vtkImageData;
class vtkAlgorithmOutput;
class vtkTransform;
class vtkImageReslice;

#include <vtkSmartPointer.h>
#include <vtkImageChangeInformation.h>
#include <vtkTransform.h>
#include <vtkImageReslice.h>

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
	void setInputData(vtkImageData* img);
	// Set input connection for the internal reslice helper
	void setInputConnection(vtkAlgorithmOutput* port);

	// Provide access to the reslice output port without exposing the helper.
	// May return nullptr if no input / helper available.
	vtkAlgorithmOutput* getOutputPort();

	// Flattened/reslice control APIs (previously on ImageResliceHelper).
	// These let external code control output grid / downsample without exposing internals.
	void SetDownsampleFactor(int factor); // alias to setDownsampleFactor (keeps parity with helper)
	void SetOutputSpacing(const double sp[3]);
	void SetOutputOrigin(const double org[3]);
	void SetOutputExtent(const int extent[6]);
	void ResetOutputGridToInput();
	// Force the internal reslice to recompute (calls internal Update).
	void Update();
	// Convenience: access the internal reslice filter (rarely needed).
	vtkImageReslice* GetReslice();

	// Enable/disable operational behavior. When false the widget:
	// - ignores incoming pipeline/data,
	// - detaches any upstream VTK input,
	// - and prevents further internal reslice/update work.
	void setOperational(bool on);
	bool isOperational() const { return m_operational; }

	// Downsample control (1 == none, 2, 4)
	int downsampleFactor() const;
	void setDownsampleFactor(int factor);


signals:
	// finished==true indicates user completed interaction (e.g., release/enter)
	void rotationChanged(double yaw, double pitch, double roll, bool finished);

	// Emitted when a resliced image is available (may be null if reslicing failed)
	void resliceReady(vtkImageData* image);

	void downsampleFactorChanged(int factor);

	// Emitted when the user clicked Apply and the reslice update completed
	void resliceApplied();

private slots:
	void onValueChanged(double);
	void onEditingFinished();
	void onResetClicked();
	void onApplyDownsampleClicked();

private:
	// Apply pending parameters (called by Apply button)
	void applyPendingChanges();

	// recompute reslice given current m_resliceTransform + output grid
	void updateReslice(bool finished);

	QDoubleSpinBox* m_yaw = nullptr;
	QDoubleSpinBox* m_pitch = nullptr;
	QDoubleSpinBox* m_roll = nullptr;
	QCheckBox* m_live = nullptr;
	QPushButton* m_reset = nullptr;
	bool m_inProgrammaticUpdate = false;

	// Flattened reslice internals (formerly in ImageResliceHelper)
	vtkSmartPointer<vtkImageReslice> m_reslice;
	vtkSmartPointer<vtkTransform>    m_resliceTransform;
	// removed m_userTransform - we apply pending rotations directly to m_resliceTransform on Apply
	vtkSmartPointer<vtkImageChangeInformation> m_changeInfo;

	// explicit output grid overrides (optional)
	double m_outSpacing[3];
	double m_outOrigin[3];
	int    m_outExtent[6];

	// downsample factor used by reslice computation (integer >= 1)
	int m_resliceDownsample = 1;

	// Downsample UI
	QComboBox* m_downsampleCombo = nullptr;
	QPushButton* m_applyDownsample = nullptr;
	int m_downsampleFactor = 1;

	// pending UI-controlled values (only applied when user clicks Apply)
	double m_pendingYaw = 0.0;
	double m_pendingPitch = 0.0;
	double m_pendingRoll = 0.0;
	// pending physical offset applied to the changeInfo output origin (meters / same units as image origin)
	// This allows UI controls to shift the center of rotation by adjusting changeInfo's origin.
	double m_pendingCenterOffset[3] = { 0.0, 0.0, 0.0 };

	// Set pending center offset (physical units). Applied on Apply().
	void setPendingCenterOffset(double dx, double dy, double dz) {
		m_pendingCenterOffset[0] = dx;
		m_pendingCenterOffset[1] = dy;
		m_pendingCenterOffset[2] = dz;
	}

	// cached input center (physical coordinates) used when composing the reslice transform
	double m_inputCenter[3] = { 0.0, 0.0, 0.0 };
	bool   m_hasInputCenter = false;

	// operational guard: when false the widget will not run reslice/update work
	bool m_operational = true;

	// recompute output spacing/extent based on input + downsample
	void computeOutputGridFromInput();
};

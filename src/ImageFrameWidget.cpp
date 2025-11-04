#include "ImageFrameWidget.h"

#include <algorithm>
#include <cmath>

#include <vtkCamera.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkRenderWindowInteractor.h>

ImageFrameWidget::ImageFrameWidget(QWidget* parent)
	: SelectionFrameWidget(parent)
{
	setAllowClose(false);
	setAllowChangeTitle(false);
	setTitleBarVisible(true);
	setSelectionListVisible(true);

	// Create the shared render surface
	m_renderWindow = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
	m_renderer = vtkSmartPointer<vtkRenderer>::New();
	m_renderWindow->AddRenderer(m_renderer);

	// Reasonable defaults; derived classes may further customize
	initializeRendererDefaults();

	shiftScaleFilter = vtkSmartPointer<vtkImageShiftScale>::New();
	shiftScaleFilter->SetOutputScalarTypeToUnsignedShort();
	shiftScaleFilter->ClampOverflowOn();
}

ImageFrameWidget::~ImageFrameWidget() = default;

void ImageFrameWidget::initializeRendererDefaults()
{
	m_renderer->GradientBackgroundOn();
	double color[3] = { 0., 0., 0. };
	m_renderer->SetBackground(color);  // black (lower part of gradient)
	color[2] = 1.;
	m_renderer->SetBackground2(color);  // blue (upper part of gradient)
}

void ImageFrameWidget::resetCamera()
{
	m_renderer->ResetCamera();
}

void ImageFrameWidget::render()
{
	if (auto* rw = getRenderWindow()) {
		if (auto* grw = vtkGenericOpenGLRenderWindow::SafeDownCast(rw)) {
			if (!grw->GetReadyForRendering()) {
				return; // avoid rendering before a current context exists
			}
		}
		rw->Render();
	}
}

void ImageFrameWidget::setViewOrientation(ViewOrientation orient)
{
	if (m_viewOrientation == orient) return;
	m_viewOrientation = orient;
	emit viewOrientationChanged(m_viewOrientation);
}

void ImageFrameWidget::notifyViewOrientationChanged()
{
	emit viewOrientationChanged(m_viewOrientation);
}

vtkRenderWindow* ImageFrameWidget::getRenderWindow() const
{
	return this->m_renderWindow;
}

QString ImageFrameWidget::orientationLabel(ViewOrientation orient) const
{
	switch (orient) {
		case VIEW_ORIENTATION_XY: return QStringLiteral("XY");
		case VIEW_ORIENTATION_YZ: return QStringLiteral("YZ");
		case VIEW_ORIENTATION_XZ: return QStringLiteral("XZ");
		default: return QString();
	}
}

ImageFrameWidget::ViewOrientation ImageFrameWidget::labelToOrientation(const QString& label) const
{
	if (label == QLatin1String("XY")) return VIEW_ORIENTATION_XY;
	if (label == QLatin1String("YZ")) return VIEW_ORIENTATION_YZ;
	if (label == QLatin1String("XZ")) return VIEW_ORIENTATION_XZ;
	return m_viewOrientation; // no change on unknown label
}

void ImageFrameWidget::computeShiftScaleFromInput(vtkImageData* image)
{
	m_nativeScalarType = image->GetScalarType();
	double scalarRange[2] = { 0, 1 };
	image->GetScalarRange(scalarRange);
	// Guard against NaN/Inf and inverted ranges
	const double r0 = std::isfinite(scalarRange[0]) ? scalarRange[0] : 0.0;
	const double r1 = std::isfinite(scalarRange[1]) ? scalarRange[1] : 1.0;
	m_scalarRangeMin = std::min(r0, r1);
	m_scalarRangeMax = std::max(r0, r1);
	const double diff = m_scalarRangeMax - m_scalarRangeMin;

	// Default: keep as unsigned short
	m_scalarShift = 0.0;
	m_scalarScale = 1.0;

	switch (m_nativeScalarType) {
		case VTK_UNSIGNED_CHAR:
		case VTK_UNSIGNED_SHORT:
		m_scalarShift = 0.0;
		m_scalarScale = 1.0;
		break;
		case VTK_CHAR:
		case VTK_SIGNED_CHAR:
		m_scalarShift = 128.0; // map [-128,127] -> [0,255]
		m_scalarScale = 1.0;
		break;
		case VTK_SHORT:
		m_scalarShift = 32768.0; // map [-32768,32767] -> [0,65535]
		m_scalarScale = 1.0;
		break;
		default: {
			// For larger ranges or floating point: shift negatives, scale up to at most 16-bit range
			m_scalarShift = (m_scalarRangeMin < 0.0) ? -m_scalarRangeMin : 0.0;
			if (diff > 0.0) {
				// Preserve existing behavior: do not amplify if the range is already within 16-bit
				m_scalarScale = std::min(65535.0 / diff, 1.0);
			}
			else {
				m_scalarScale = 1.0;
			}
			break;
		}
	}

	// Program the shared filter
	shiftScaleFilter->SetOutputScalarTypeToUnsignedShort();
	shiftScaleFilter->SetShift(m_scalarShift);
	shiftScaleFilter->SetScale(m_scalarScale);
}

void ImageFrameWidget::onSelectionChanged(bool selected)
{
	// Optional feature: when enabled (default), only the selected frame is interactive
	if (!m_renderWindow) return;

	if (auto* iren = m_renderWindow->GetInteractor()) {
		if (restrictInteractionToSelection()) {
			if (selected) iren->Enable();
			else iren->Disable();
		}
		else {
			// Free mode: all frames interactive
			iren->Enable();
		}
	}
}

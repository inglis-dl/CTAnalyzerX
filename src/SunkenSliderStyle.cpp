#include "SunkenSliderStyle.h"
#include <QStyle>
#include <QPalette>

void SunkenSliderStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex* option,
										   QPainter* painter, const QWidget* widget) const
{
	if (control == CC_Slider) {
		const QStyleOptionSlider* slider = qstyleoption_cast<const QStyleOptionSlider*>(option);

		// Groove (trough): lighter gray, sunken bevel
		QRect grooveRect = subControlRect(CC_Slider, option, SC_SliderGroove, widget);
		QColor grooveBase(180, 180, 180); // lighter gray
		QColor grooveShadow(128, 128, 128); // shadow
		QColor grooveHighlight(235, 235, 235); // highlight

		painter->save();
		painter->setRenderHint(QPainter::Antialiasing);

		painter->setBrush(grooveBase);
		painter->setPen(Qt::NoPen);
		painter->drawRect(grooveRect);

		QPen pen;
		pen.setWidth(2);

		// Sunken bevel: shadow top/left, highlight bottom/right
		pen.setColor(grooveShadow);
		painter->setPen(pen);
		painter->drawLine(grooveRect.topLeft(), grooveRect.topRight());
		painter->drawLine(grooveRect.topLeft(), grooveRect.bottomLeft());

		pen.setColor(grooveHighlight);
		painter->setPen(pen);
		painter->drawLine(grooveRect.bottomLeft(), grooveRect.bottomRight());
		painter->drawLine(grooveRect.topRight(), grooveRect.bottomRight());

		painter->restore();

		// Handle: 2:1 rectangle, raised bevel, snap line, pressed effect
		if (option->subControls & SC_SliderHandle) {
			QRect handleRect = subControlRect(CC_Slider, option, SC_SliderHandle, widget);

			// Base colors
			QColor handleBase = option->palette.color(QPalette::Button);// (220, 220, 255); // light blue-gray
			QColor handleShadow(130, 130, 120); // shadow for bevel
			QColor handleHighlight(255, 255, 255); // highlight for bevel
			QColor handleBorder = option->palette.color(QPalette::Midlight);// (170, 170, 200); // border
			QColor snapLine(160, 160, 160); // gray for snap line

			// Pressed effect
			bool pressed = (option->activeSubControls & SC_SliderHandle) &&
				(option->state & State_Sunken);

			/*
			if (pressed) {
				// Darken base color when pressed
				handleBase = QColor(160, 160, 160); // slightly darker gray
			}
			*/

			painter->save();
			painter->setRenderHint(QPainter::Antialiasing, false);

			// Draw base
			painter->setBrush(handleBase);
			painter->setPen(QPen(handleBorder, 1));
			painter->drawRect(handleRect);

			QPen bevelPen;
			bevelPen.setWidth(2);

			if (!pressed) {
				// Raised bevel: highlight top/left, shadow bottom/right
				// highlight top/left
				bevelPen.setColor(handleHighlight);
				painter->setPen(bevelPen);
				painter->drawLine(handleRect.topLeft(), handleRect.topRight());
				painter->drawLine(handleRect.topLeft(), handleRect.bottomLeft());

				// shadow bottom/right
				bevelPen.setColor(handleShadow);
				painter->setPen(bevelPen);
				painter->drawLine(handleRect.bottomLeft(), handleRect.bottomRight());
				painter->drawLine(handleRect.topRight(), handleRect.bottomRight());
			}
			else {
				// Pressed bevel: shadow top/left, highlight bottom/right
				// shadow top/left
				bevelPen.setColor(handleShadow);
				painter->setPen(bevelPen);
				painter->drawLine(handleRect.topLeft(), handleRect.topRight());
				painter->drawLine(handleRect.topLeft(), handleRect.bottomLeft());

				// highlight bottom/right
				bevelPen.setColor(handleHighlight);
				painter->setPen(bevelPen);
				painter->drawLine(handleRect.bottomLeft(), handleRect.bottomRight());
				painter->drawLine(handleRect.topRight(), handleRect.bottomRight());
			}

			// Draw snap line (vertical, centered)
			painter->setPen(QPen(snapLine, 2));
			int snapX = handleRect.left() + handleRect.width() / 2;
			painter->drawLine(QPoint(snapX, handleRect.top() + 3), QPoint(snapX, handleRect.bottom() - 3));

			painter->restore();
		}
	}
	else {
		QProxyStyle::drawComplexControl(control, option, painter, widget);
	}
}

QRect SunkenSliderStyle::subControlRect(ComplexControl control, const QStyleOptionComplex* option,
										SubControl subControl, const QWidget* widget) const
{
	if (control == CC_Slider) {
		const QStyleOptionSlider* slider = qstyleoption_cast<const QStyleOptionSlider*>(option);
		if (subControl == SC_SliderGroove) {
			int margin = 14;
			int grooveHeight = 20; // increased height for deeper trough
			if (slider->orientation == Qt::Horizontal)
				return QRect(margin, slider->rect.height() / 2 - grooveHeight / 2,
							 slider->rect.width() - 2 * margin, grooveHeight);
			else
				return QRect(slider->rect.width() / 2 - grooveHeight / 2, margin,
							 grooveHeight, slider->rect.height() - 2 * margin);
		}
		if (subControl == SC_SliderHandle) {
			QRect grooveRect = subControlRect(control, option, SC_SliderGroove, widget);

			int handleLength = (slider->orientation == Qt::Horizontal) ? grooveRect.height() : grooveRect.width();
			int handleWidth = handleLength * 2;
			int handleHeight = handleLength;

			double percent = double(slider->sliderPosition - slider->minimum) /
				(slider->maximum - slider->minimum > 0 ? slider->maximum - slider->minimum : 1);

			if (slider->orientation == Qt::Horizontal) {
				int x = grooveRect.left() + int(percent * (grooveRect.width() - handleWidth));
				int y = grooveRect.top() + grooveRect.height() - handleHeight - 2;
				return QRect(x, y, handleWidth, handleHeight);
			}
			else {
				int y = grooveRect.top() + int(percent * (grooveRect.height() - handleHeight));
				int x = grooveRect.left() + grooveRect.width() - handleWidth - 2;
				return QRect(x, y, handleHeight, handleWidth); // vertical: swapped
			}
		}
	}
	return QProxyStyle::subControlRect(control, option, subControl, widget);
}

int SunkenSliderStyle::pixelMetric(PixelMetric metric, const QStyleOption* option, const QWidget* widget) const
{
	if (metric == PM_SliderLength) {
		return 24;
	}
	return QProxyStyle::pixelMetric(metric, option, widget);
}
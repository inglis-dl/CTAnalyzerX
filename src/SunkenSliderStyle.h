#pragma once

#include <QProxyStyle>
#include <QStyleOptionSlider>
#include <QPainter>

class SunkenSliderStyle : public QProxyStyle {
public:
	using QProxyStyle::QProxyStyle;

	void drawComplexControl(ComplexControl control, const QStyleOptionComplex* option,
							QPainter* painter, const QWidget* widget = nullptr) const override;

	QRect subControlRect(ComplexControl control, const QStyleOptionComplex* option,
						 SubControl subControl, const QWidget* widget = nullptr) const override;

	int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
					const QWidget* widget = nullptr) const override;
};

/*
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOptionSlider>
#include <QPainter>

class SunkenSliderStyle : public QProxyStyle {
public:
	SunkenSliderStyle(QStyle* baseStyle = nullptr)
		: QProxyStyle(baseStyle ? baseStyle : QStyleFactory::create("Fusion")) {
	}

	// Ensure 2:1 aspect ratio (width:height) for the handle across orientations.
	int pixelMetric(PixelMetric pm, const QStyleOption* option, const QWidget* widget) const override {
		const auto* so = qstyleoption_cast<const QStyleOptionSlider*>(option);
		// Base minor dimension for handle (height for horizontal, height for ratio calc; length for vertical)
		const int minor = 14;

		if (pm == PM_SliderLength) {
			if (so) {
				// Length is along the orientation: width for horizontal, height for vertical
				return (so->orientation == Qt::Horizontal) ? minor * 2 : minor;
			}
			// Fallback assumes horizontal defaults
			return minor * 2;
		}
		if (pm == PM_SliderThickness) {
			if (so) {
				// Thickness is perpendicular to the orientation: height for horizontal, width for vertical
				return (so->orientation == Qt::Horizontal) ? minor : minor * 2;
			}
			// Fallback assumes horizontal defaults
			return minor;
		}
		return QProxyStyle::pixelMetric(pm, option, widget);
	}

	void drawComplexControl(ComplexControl control,
							 const QStyleOptionComplex* option,
							 QPainter* painter,
							 const QWidget* widget) const override {
		if (control == CC_Slider) {
			const QStyleOptionSlider* sliderOpt = qstyleoption_cast<const QStyleOptionSlider*>(option);
			if (!sliderOpt) {
				QProxyStyle::drawComplexControl(control, option, painter, widget);
				return;
			}

			const bool horizontal = sliderOpt->orientation == Qt::Horizontal;
			const int handleThickness = pixelMetric(PM_SliderThickness, sliderOpt, widget);
			const int bevel = 2; // visual bevel thickness
			const int grooveThickness = qMax(6, handleThickness - 2 * bevel); // slightly smaller so handle looks inset

			painter->save();
			painter->setRenderHint(QPainter::Antialiasing, false);

			// Groove (trough) with sunken bevel
			QRect grooveRect = subControlRect(CC_Slider, sliderOpt, SC_SliderGroove, widget);
			grooveRect.setHeight(14);
			grooveRect.moveTop(sliderOpt->rect.center().y() - grooveRect.height() / 2);

			painter->setBrush(QColor("#c0c0c0")); // base gray
			painter->setPen(Qt::NoPen);
			painter->drawRect(grooveRect);

			// Outer bevel
			painter->setPen(QColor("#808080")); // dark top/left
			painter->drawLine(grooveRect.topLeft(), grooveRect.topRight());
			painter->drawLine(grooveRect.topLeft(), grooveRect.bottomLeft());

			painter->setPen(QColor("#e0e0e0")); // light bottom/right
			painter->drawLine(grooveRect.bottomLeft(), grooveRect.bottomRight());
			painter->drawLine(grooveRect.topRight(), grooveRect.bottomRight());

			// Inner bevel
			QRect innerRect = grooveRect.adjusted(1, 1, -1, -1);
			painter->setPen(QColor("#a0a0a0"));
			painter->drawLine(innerRect.topLeft(), innerRect.topRight());
			painter->drawLine(innerRect.topLeft(), innerRect.bottomLeft());

			painter->setPen(QColor("#f8f8f8"));
			painter->drawLine(innerRect.bottomLeft(), innerRect.bottomRight());
			painter->drawLine(innerRect.topRight(), innerRect.bottomRight());

			// Handle (slider tab)
			QRect handleRect = subControlRect(CC_Slider, sliderOpt, SC_SliderHandle, widget);
			// Do not change size to avoid clipping at the ends; only align vertically to the groove.
			handleRect.moveCenter(QPoint(handleRect.center().x(), grooveRect.center().y()));

			QColor handleFill = QColor("#dcdcdc");
			if (sliderOpt->state & QStyle::State_Sunken) {
				handleFill = QColor("#c8c8c8");
			}
			else if (sliderOpt->state & QStyle::State_MouseOver) {
				handleFill = QColor("#e8e8e8");
			}

			painter->setBrush(handleFill);
			painter->setPen(QColor("#606060"));
			painter->drawRect(handleRect);

			// Raised bevel on handle
			painter->setPen(QColor("#f8f8f8")); // top/left highlight
			painter->drawLine(handleRect.topLeft(), handleRect.topRight());
			painter->drawLine(handleRect.topLeft(), handleRect.bottomLeft());

			painter->setPen(QColor("#808080")); // bottom/right shadow
			painter->drawLine(handleRect.bottomLeft(), handleRect.bottomRight());
			painter->drawLine(handleRect.topRight(), handleRect.bottomRight());

			// Pill-break vertical line
			painter->setPen(QColor("#606060"));
			int midX = handleRect.center().x();
			painter->drawLine(midX, handleRect.top() + 2, midX, handleRect.bottom() - 2);

			painter->restore();
		}
		else {
			QProxyStyle::drawComplexControl(control, option, painter, widget);
		}
	}

	QSize sizeFromContents(ContentsType type, const QStyleOption* option,
							const QSize& size, const QWidget* widget) const override {
		if (type == CT_Slider) {
			// Ensure the widget reserves enough cross-axis space for the 2:1 handle.
			const auto* so = qstyleoption_cast<const QStyleOptionSlider*>(option);
			const int thickness = pixelMetric(PM_SliderThickness, option, widget);
			const int pad = 12; // room for bevels
			if (so && so->orientation == Qt::Horizontal) {
				return QSize(size.width(), qMax(size.height(), thickness + pad));
			}
			// Vertical or unknown orientation: ensure width fits the handle thickness
			return QSize(qMax(size.width(), thickness + pad), size.height());
		}
		return QProxyStyle::sizeFromContents(type, option, size, widget);
	}
};
*/

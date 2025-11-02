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

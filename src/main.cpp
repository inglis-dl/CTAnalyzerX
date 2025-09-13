#include <QApplication>
#include "MainWindow.h"
#include <QVTKOpenGLNativeWidget.h>
#include <QStyleFactory>

int main(int argc, char* argv[]) {

	// needed to ensure appropriate OpenGL context is created for VTK rendering.
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

	QApplication app(argc, argv);

	QApplication::setStyle(QStyleFactory::create("Fusion"));

	MainWindow window;
	window.show();
	return app.exec();
}
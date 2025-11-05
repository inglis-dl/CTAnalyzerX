#include <QApplication>
#include "MainWindow.h"
#include <QVTKOpenGLNativeWidget.h>
#include <QStyleFactory>
#include <QResource> // optional, quiets some compilers

#include <vtkAutoInit.h>
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);


int main(int argc, char* argv[]) {

	// needed to ensure appropriate OpenGL context is created for VTK rendering.
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

	QApplication app(argc, argv);

	// Ensure the .qrc named "resources" is initialized in the binary.
	// This must run before any use of :/ resource paths.
	Q_INIT_RESOURCE(resources);

	QApplication::setStyle(QStyleFactory::create("Fusion"));

	MainWindow window;
	window.show();
	return app.exec();
}
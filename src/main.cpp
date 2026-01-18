#include <QApplication>
#include "MainWindow.h"
#include "Logger.h"

#include <QVTKOpenGLNativeWidget.h>
#include <QStyleFactory>
#include <QResource> // optional, quiets some compilers

#include <vtkAutoInit.h>

// Explicitly initialize required VTK modules for static builds.
// Core OpenGL rendering backend:
VTK_MODULE_INIT(vtkRenderingOpenGL2);
// 2D context (charts, vtkContextView, vtkContextDevice2D):
VTK_MODULE_INIT(vtkRenderingContextOpenGL2);
// Volume rendering backend (you already had this one):
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
// Optional but commonly used:
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingFreeType);
// NOTE: Do NOT init vtkRenderingQt here unless you also link that module.
// VTK_MODULE_INIT(vtkRenderingQt);

int main(int argc, char* argv[]) {

	// needed to ensure appropriate OpenGL context is created for VTK rendering.
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

	QApplication app(argc, argv);

	Logger::install();
	// Ensure logger is uninstalled before VTK leak checks / app teardown
	QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
		Logger::uninstall();
	});

	// Ensure the .qrc named "resources" is initialized in the binary.
	// This must run before any use of :/ resource paths.
	Q_INIT_RESOURCE(resources);

	QApplication::setStyle(QStyleFactory::create("Fusion"));

	MainWindow window;
	window.show();
	return app.exec();
}
#include <QApplication>
#include "MainWindow.h"
#include "Logger.h"

#include <QVTKOpenGLNativeWidget.h>
#include <QStyleFactory>
#include <QResource> // optional, quiets some compilers
#include <QStyle>
#include <vtkAutoInit.h>

// Windows: force Per-Monitor V2 DPI awareness to prevent mixed/incorrect font scaling
#ifdef _WIN32
#include <windows.h>
#endif

// Explicitly initialize required VTK modules for static builds.
// Core OpenGL rendering backend:
VTK_MODULE_INIT(vtkRenderingOpenGL2);
// 2D context (charts, vtkContextView, vtkContextDevice2D):
VTK_MODULE_INIT(vtkRenderingContextOpenGL2);
// Volume rendering backend:
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);
// Optional but commonly used:
VTK_MODULE_INIT(vtkInteractionStyle);
VTK_MODULE_INIT(vtkRenderingFreeType);
// NOTE: Do NOT init vtkRenderingQt here unless linking to that module.
// VTK_MODULE_INIT(vtkRenderingQt);

int main(int argc, char* argv[]) {

#ifdef _WIN32
	// Must be called before any UI is created; sets true Per-Monitor DPI behavior.
	// On older Windows versions this may fail silently; that's OK.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

	// High-DPI: set policy BEFORE constructing QApplication to avoid mixed scaling
	QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
		Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

	// needed to ensure appropriate OpenGL context is created for VTK rendering.
	QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

	QApplication app(argc, argv);

/*
       Logger::install();
       // Ensure logger is uninstalled before VTK leak checks / app teardown
       QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
               Logger::uninstall();
       });
*/

	// Ensure the .qrc named "resources" is initialized in the binary.
	// This must run before any use of :/ resource paths.
	Q_INIT_RESOURCE(resources);

	// Set style early so all subsequent font queries reflect the final style.
	QApplication::setStyle(QStyleFactory::create("Fusion"));

	// Make widget fonts match menu fonts (your "correct" reference).
	// QMenu popups typically use a different, smaller font on Windows (e.g., Segoe UI 9).
	// We take that as the baseline and apply it application-wide.
	QFont menuFont = QApplication::font("QMenu");
	if (menuFont.family().isEmpty()) {
		menuFont = QFont(QStringLiteral("Segoe UI"), 9);
	}

	// Apply to all widgets (menus will already be using this font or their own platform font,
	// but the important part is shrinking the rest of the UI to match your "good" menu size).
	QApplication::setFont(menuFont);

	MainWindow window;
	window.show();
	return app.exec();
}
#include "PrototypeMainWindow.h"
#include "Logger.h"

#include <QVTKOpenGLNativeWidget.h>
#include <vtkAutoInit.h>

#include <QApplication>
#include <QMessageBox>
#include <QStringList>
#include <QStyleFactory>
#include <QSurfaceFormat>

#ifdef _WIN32
#include <windows.h>
#endif

VTK_MODULE_INIT(vtkRenderingOpenGL2);
VTK_MODULE_INIT(vtkRenderingContextOpenGL2);
VTK_MODULE_INIT(vtkRenderingVolumeOpenGL2);

int main(int argc, char* argv[])
{
#ifdef _WIN32
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

	QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
		Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

	QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

	QApplication app(argc, argv);

	// Set identity before Logger::install() so QStandardPaths points to
	// the expected app-local directory.
	QCoreApplication::setOrganizationName(QStringLiteral("CTAnalyzerX"));
	QCoreApplication::setApplicationName(QStringLiteral("CTAXPrototype"));

	// Logger installs Qt message handler + VTK output window routing.
	Logger::setChannel(QStringLiteral("Prototype"));
	Logger::setSingleSharedFile(true); // or false
	Logger::install();
	QObject::connect(&app, &QCoreApplication::aboutToQuit, []() { Logger::uninstall(); });

	Q_INIT_RESOURCE(resources);

	QApplication::setStyle(QStyleFactory::create("Fusion"));
	QFont menuFont = QApplication::font("QMenu");
	if (menuFont.family().isEmpty())
		menuFont = QFont(QStringLiteral("Segoe UI"), 9);
	QApplication::setFont(menuFont);

	try
	{
		PrototypeMainWindow w;
		w.resize(1200, 800);
		w.show();

		const QStringList args = QApplication::arguments();
		if (args.size() >= 2)
			w.loadFromSidecarAsync(args.at(1));

		return app.exec();
	}
	catch (const std::exception& ex)
	{
		Logger::uninstall();

		QMessageBox::critical(
			nullptr,
			QStringLiteral("CTAXPrototype - Fatal Error"),
			QString::fromStdString(ex.what()));
		return 1;
	}
}
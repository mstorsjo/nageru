#include <QApplication>
#include <QDesktopWidget>
#include <QSurfaceFormat>
#include <QtGui/QOpenGLContext>

#include "mainwindow.h"
#include "mixer.h"

int main(int argc, char *argv[])
{
	setenv("QT_XCB_GL_INTEGRATION", "xcb_egl", 0);

	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);
	QApplication app(argc, argv);

	QSurfaceFormat fmt;
	fmt.setDepthBufferSize(0);
	fmt.setStencilBufferSize(0);
	fmt.setProfile(QSurfaceFormat::CoreProfile);
	fmt.setMajorVersion(3);
	fmt.setMinorVersion(1);
	QSurfaceFormat::setDefaultFormat(fmt);

	MainWindow mainWindow;
	mainWindow.resize(mainWindow.sizeHint());
	mainWindow.show();

	int rc = app.exec();
	mixer_quit();
	return rc;
}

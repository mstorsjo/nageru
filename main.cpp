#include <epoxy/gl.h>
#include <QApplication>
#include <QDesktopWidget>
#include <QSurfaceFormat>
#include <QtGui/QOpenGLContext>
#include <QGLFormat>

#include "mainwindow.h"
#include "mixer.h"
#include "context.h"

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

	QGLFormat::setDefaultFormat(QGLFormat::fromSurfaceFormat(fmt));

	global_share_widget = new QGLWidget();

	MainWindow mainWindow;
	mainWindow.resize(QSize(1280, 720));
	mainWindow.show();

	int rc = app.exec();
	mixer_quit();
	return rc;
}

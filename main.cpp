#include <stdlib.h>
#include <epoxy/gl.h>

#include <QApplication>
#include <QCoreApplication>
#include <QGL>
#include <QSize>
#include <QSurfaceFormat>

#include "context.h"
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

	QGLFormat::setDefaultFormat(QGLFormat::fromSurfaceFormat(fmt));

	global_share_widget = new QGLWidget();

	MainWindow mainWindow;
	mainWindow.resize(QSize(1500, 670));
	mainWindow.show();

	int rc = app.exec();
	global_mixer->quit();
	delete global_mixer;
	return rc;
}

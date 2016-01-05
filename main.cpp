extern "C" {
#include <libavformat/avformat.h>
}
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <epoxy/gl.h>

#include <QApplication>
#include <QCoreApplication>
#include <QGL>
#include <QSize>
#include <QSurfaceFormat>

#include "context.h"
#include "flags.h"
#include "mainwindow.h"
#include "mixer.h"

int main(int argc, char *argv[])
{
	setenv("QT_XCB_GL_INTEGRATION", "xcb_egl", 0);
	setlinebuf(stdout);
	av_register_all();
	parse_flags(argc, argv);

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
	mainWindow.resize(QSize(1500, 810));
	mainWindow.show();

	app.installEventFilter(&mainWindow);  // For white balance color picking.

	int rc = app.exec();
	global_mixer->quit();
	mainWindow.mixer_shutting_down();
	delete global_mixer;
	return rc;
}

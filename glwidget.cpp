#include <qmetatype.h>
#include <qdatastream.h>
#include <qtextstream.h>
#include <qcursor.h>
#include <qcoreevent.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include "context.h"
#include "glwidget.h"
#include "mixer.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QThread>
#include <math.h>
#include <thread>

GLWidget::GLWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
}

GLWidget::~GLWidget()
{
}

void GLWidget::initializeGL()
{
	printf("egl context=%p\n", eglGetCurrentContext());
	//printf("threads: %p %p\n", QThread::currentThread(), qGuiApp->thread());

	QSurface *surface = create_surface(format());
	QSurface *surface2 = create_surface(format());
	QSurface *surface3 = create_surface(format());
	QSurface *surface4 = create_surface(format());
	start_mixer(surface, surface2, surface3, surface4);
}

void GLWidget::paintGL()
{
	glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

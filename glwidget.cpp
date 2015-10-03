#include "context.h"
#include "glwidget.h"
#include "mixer.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QThread>
#include <math.h>
#include <EGL/egl.h>
#include <GL/glx.h>
#include <thread>

GLWidget::GLWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
}

GLWidget::~GLWidget()
{
}

QSize GLWidget::minimumSizeHint() const
{
	return QSize(50, 50);
}

QSize GLWidget::sizeHint() const
{
	return QSize(400, 400);
}

void GLWidget::initializeGL()
{
	printf("egl=%p glx=%p\n", eglGetCurrentContext(), glXGetCurrentContext());
	//printf("threads: %p %p\n", QThread::currentThread(), qGuiApp->thread());

	QSurface *surface = create_surface(format());
	QSurface *surface2 = create_surface(format());
	QSurface *surface3 = create_surface(format());
	QSurface *surface4 = create_surface(format());
	std::thread([surface, surface2, surface3, surface4]{
		mixer_thread(surface, surface2, surface3, surface4);
	}).detach();
}

void GLWidget::paintGL()
{
	glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

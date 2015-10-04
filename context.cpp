#include <stdio.h>

#include <QGL>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QSurfaceFormat>

class QSurface;

QGLWidget *global_share_widget = nullptr;

QSurface *create_surface(const QSurfaceFormat &format)
{
	QOffscreenSurface *surface = new QOffscreenSurface;
	surface->setFormat(format);
//	QWindow *surface = new QWindow;
	surface->create();
	if (!surface->isValid()) {
		printf("ERROR: surface not valid!\n");
//		abort();
	}
	return surface;
}

QOpenGLContext *create_context()
{
	QOpenGLContext *context = new QOpenGLContext;
	context->setShareContext(global_share_widget->context()->contextHandle());
	context->create();
	return context;
}

bool make_current(QOpenGLContext *context, QSurface *surface)
{
	return context->makeCurrent(surface);
}

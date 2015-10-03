#include <stdio.h>
#include <stdlib.h>

#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QWindow>

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
	context->setShareContext(QOpenGLContext::globalShareContext());
	context->create();
	return context;
}

bool make_current(QOpenGLContext *context, QSurface *surface)
{
	return context->makeCurrent(surface);
}

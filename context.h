
// Needs to be in its own file because Qt and libepoxy seemingly don't coexist well
// within the same file.

class QSurface;
class QOpenGLContext;
class QSurfaceFormat;
class QGLWidget;

extern QGLWidget *global_share_widget;
QSurface *create_surface(const QSurfaceFormat &format);
QOpenGLContext *create_context(const QSurface *surface);
bool make_current(QOpenGLContext *context, QSurface *surface);

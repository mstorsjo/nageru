#ifndef GLWIDGET_H
#define GLWIDGET_H

#include <epoxy/gl.h>
#include <QGLWidget>

class QWidget;

namespace movit {
class ResourcePool;
}

// Note: We use the older QGLWidget instead of QOpenGLWidget as it is
// much faster (does not go through a separate offscreen rendering step).
//
// TODO: Consider if QOpenGLWindow could do what we want.
class GLWidget : public QGLWidget
{
	Q_OBJECT

public:
	GLWidget(QWidget *parent = 0);
	~GLWidget();

protected:
	void initializeGL() override;
	void resizeGL(int width, int height) override;
	void paintGL() override;

private:
	movit::ResourcePool *resource_pool;
	GLuint vao, program_num;
	GLuint position_vbo, texcoord_vbo;
};

#endif

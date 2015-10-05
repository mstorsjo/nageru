#ifndef GLWIDGET_H
#define GLWIDGET_H

#include <epoxy/gl.h>
#include <QGLWidget>

#include "mixer.h"

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

	void set_output(Mixer::Output output)
	{
		this->output = output;
	}

protected:
	void initializeGL() override;
	void resizeGL(int width, int height) override;
	void paintGL() override;

private:
	Mixer::Output output;
	movit::ResourcePool *resource_pool;
	GLuint vao, program_num;
	GLuint position_vbo, texcoord_vbo;
};

#endif

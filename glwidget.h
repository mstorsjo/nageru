#ifndef GLWIDGET_H
#define GLWIDGET_H

#include <QOpenGLWidget>

namespace movit {
class ResourcePool;
}

class GLWidget : public QOpenGLWidget
{
	Q_OBJECT

public:
	GLWidget(QWidget *parent = 0);
	~GLWidget();

protected:
	void initializeGL() override;
	void paintGL() override;

private:
	movit::ResourcePool *resource_pool;
	GLuint vao, program_num;
	GLuint position_vbo, texcoord_vbo;
};

#endif

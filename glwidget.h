#ifndef GLWIDGET_H
#define GLWIDGET_H

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLBuffer>
#include <QMatrix4x4>

class GLWidget : public QOpenGLWidget
{
	Q_OBJECT

public:
	GLWidget(QWidget *parent = 0);
	~GLWidget();

protected:
	void initializeGL() Q_DECL_OVERRIDE;
	void paintGL() Q_DECL_OVERRIDE;
};

#endif

#ifndef GLWIDGET_H
#define GLWIDGET_H

#include <epoxy/gl.h>
#include <QGLWidget>
#include <string>
#include <vector>

#include "mixer.h"
#include "qgl.h"
#include "qobjectdefs.h"

class QMouseEvent;
class QWidget;

namespace movit {

class ResourcePool;

}  // namespace movit

// Note: We use the older QGLWidget instead of QOpenGLWidget as it is
// much faster (does not go through a separate offscreen rendering step).
//
// TODO: Consider if QOpenGLWindow could do what we want.
class GLWidget : public QGLWidget
{
	Q_OBJECT

public:
	GLWidget(QWidget *parent = 0);

	void set_output(Mixer::Output output)
	{
		this->output = output;
	}

	void clean_context();

protected:
	void initializeGL() override;
	void resizeGL(int width, int height) override;
	void paintGL() override;
	void mousePressEvent(QMouseEvent *event) override;

signals:
	void clicked();
	void transition_names_updated(std::vector<std::string> transition_names);
	void resolution_updated(Mixer::Output output);

private slots:
	void show_context_menu(unsigned signal_num, const QPoint &pos);

private:
	Mixer::Output output;
	GLuint vao, program_num;
	GLuint position_vbo, texcoord_vbo;
	movit::ResourcePool *resource_pool = nullptr;
};

#endif

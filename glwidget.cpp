#include <qmetatype.h>  // Needs to come before egl.h.
#include <qdatastream.h>  // Needs to come before egl.h.
#include <qtextstream.h>  // Needs to come before egl.h.
#include <qcursor.h>  // Needs to come before egl.h.
#include <qcoreevent.h>  // Needs to come before egl.h.
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <QSurfaceFormat>

#include "glwidget.h"

#include <movit/resource_pool.h>
#include <stdio.h>

#include "context.h"
#include "mixer.h"
#include "ref_counted_gl_sync.h"

class QSurface;
class QWidget;

#undef Success
#include <movit/util.h>
#include <string>

GLWidget::GLWidget(QWidget *parent)
    : QGLWidget(parent, global_share_widget),
      resource_pool(new movit::ResourcePool)
{
}

GLWidget::~GLWidget()
{
}

void GLWidget::initializeGL()
{
	printf("egl context=%p\n", eglGetCurrentContext());
	//printf("threads: %p %p\n", QThread::currentThread(), qGuiApp->thread());

	GLWidget *t = this;
	global_mixer->set_frame_ready_fallback([t]{
		QMetaObject::invokeMethod(t, "update", Qt::AutoConnection);
	});

	QSurfaceFormat fmt = QGLFormat::toSurfaceFormat(format());
	QSurface *surface = create_surface(fmt);
	QSurface *surface2 = create_surface(fmt);
	QSurface *surface3 = create_surface(fmt);
	QSurface *surface4 = create_surface(fmt);
	global_mixer = new Mixer;
	global_mixer->start(surface, surface2, surface3, surface4);

	// Prepare the shaders to actually get the texture shown (ick).
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);

	std::string vert_shader =
		"#version 130 \n"
		"in vec2 position; \n"
		"in vec2 texcoord; \n"
		"out vec2 tc; \n"
		" \n"
		"void main() \n"
		"{ \n"
		"    gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0); \n"
		"    tc = texcoord; \n"
		"    tc.y = 1.0 - tc.y; \n"
		"} \n";
	std::string frag_shader =
		"#version 130 \n"
		"in vec2 tc; \n"
		"uniform sampler2D tex; \n"
		"void main() { \n"
		"    gl_FragColor = texture2D(tex, tc); \n"
		"} \n";
	program_num = resource_pool->compile_glsl_program(vert_shader, frag_shader);

	static const float vertices[] = {
		0.0f, 2.0f,
		0.0f, 0.0f,
		2.0f, 0.0f
	};
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	position_vbo = movit::fill_vertex_attribute(program_num, "position", 2, GL_FLOAT, sizeof(vertices), vertices);
	texcoord_vbo = movit::fill_vertex_attribute(program_num, "texcoord", 2, GL_FLOAT, sizeof(vertices), vertices);  // Same as vertices.

#if 0
	// Cleanup.
	cleanup_vertex_attribute(phases[0]->glsl_program_num, "position", position_vbo);
	cleanup_vertex_attribute(phases[0]->glsl_program_num, "texcoord", texcoord_vbo);

	glDeleteVertexArrays(1, &vao);
	nheck_error();
#endif
}

void GLWidget::resizeGL(int width, int height)
{
	glViewport(0, 0, width, height);
}

void GLWidget::paintGL()
{
	Mixer::DisplayFrame frame;
	if (!global_mixer->get_display_frame(&frame)) {
		glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		return;
	}

	glUseProgram(program_num);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, frame.texnum);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindVertexArray(vao);
	glWaitSync(frame.ready_fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}

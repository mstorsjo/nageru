#include <qmetatype.h>  // Needs to come before egl.h.
#include <qdatastream.h>  // Needs to come before egl.h.
#include <qtextstream.h>  // Needs to come before egl.h.
#include <qcursor.h>  // Needs to come before egl.h.
#include <qcoreevent.h>  // Needs to come before egl.h.
#include <qevent.h>  // Needs to come before egl.h.
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include <QSurfaceFormat>

#include "glwidget.h"

#include <movit/resource_pool.h>
#include <stdio.h>
#include <functional>
#include <mutex>

#include "context.h"
#include "effect_chain.h"
#include "flags.h"
#include "mainwindow.h"
#include "mixer.h"
#include "qnamespace.h"
#include "ref_counted_gl_sync.h"

class QMouseEvent;
class QWidget;

#undef Success
#include <movit/util.h>
#include <string>

using namespace std;

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
	static std::once_flag flag;
	std::call_once(flag, [this]{
		global_mixer = new Mixer(QGLFormat::toSurfaceFormat(format()), global_flags.num_cards);
		global_mainwindow->mixer_created(global_mixer);
		global_mixer->start();
	});
	global_mixer->set_frame_ready_callback(output, [this]{
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
		emit transition_names_updated(global_mixer->get_transition_names());
	});

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
}

void GLWidget::resizeGL(int width, int height)
{
	glViewport(0, 0, width, height);
}

void GLWidget::paintGL()
{
	Mixer::DisplayFrame frame;
	if (!global_mixer->get_display_frame(output, &frame)) {
		glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
		check_error();
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		check_error();
		return;
	}

	check_error();
	glWaitSync(frame.ready_fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
	check_error();
	frame.setup_chain();
	check_error();
	frame.chain->render_to_screen();
	check_error();
}

void GLWidget::mousePressEvent(QMouseEvent *event)
{
	emit clicked();
}

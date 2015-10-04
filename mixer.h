#ifndef _MIXER_H
#define _MIXER_H 1

#include <epoxy/gl.h>
#include <functional>

#include "ref_counted_gl_sync.h"

class QSurface;

void start_mixer(QSurface *surface, QSurface *surface2, QSurface *surface3, QSurface *surface4);
void mixer_quit();

enum Source {
	SOURCE_INPUT1,
	SOURCE_INPUT2,
	SOURCE_SBS,
};
void mixer_cut(Source source);

struct DisplayFrame {
	GLuint texnum;
	RefCountedGLsync ready_fence;  // Asserted when the texture is done rendering.
};
bool mixer_get_display_frame(DisplayFrame *frame);  // Implicitly frees the previous one if there's a new frame available.

typedef std::function<void()> new_frame_ready_callback_t;
void set_frame_ready_fallback(new_frame_ready_callback_t callback);

#endif  // !defined(_MIXER_H)

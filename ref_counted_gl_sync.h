#ifndef _REF_COUNTED_GL_SYNC_H
#define _REF_COUNTED_GL_SYNC_H 1

// A wrapper around GLsync (OpenGL fences) that is automatically refcounted.
// Useful since we sometimes want to use the same fence two entirely different
// places. (We could set two fences at the same time, but they are not an
// unlimited hardware resource, so it would be a bit wasteful.)

#include <GL/gl.h>
#include <memory>

typedef std::shared_ptr<__GLsync> RefCountedGLsyncBase;

class RefCountedGLsync : public RefCountedGLsyncBase {
public:
	RefCountedGLsync() {}

	RefCountedGLsync(GLenum condition, GLbitfield flags) 
		: RefCountedGLsyncBase(glFenceSync(condition, flags), glDeleteSync) {}
};

#endif  // !defined(_REF_COUNTED_GL_SYNC_H)

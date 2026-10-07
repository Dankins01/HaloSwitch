/*
SWITCH_GL.C

OpenGL ES for the guest, from mesa (nouveau) through SDL2's EGL context.
The guest's generated entry points (guest_gl.c) import hostgl_<function>,
resolved here with mesa's eglGetProcAddress, which returns core functions
too. The few GL calls this file makes itself go through pointers resolved
the same way, so the host links no GL library beyond EGL and mesa's glapi.

The helpers are those of port/android/host/host_gl.c; see there for why
buffer writes are unsynchronized and frames fenced.
*/

#include "switch_host.h"

#include <switch.h>

#include <string.h>

/* mesa's libEGL (the one EGL function used here, declared without
EGL/egl.h and its platform types) */
extern void (*eglGetProcAddress(const char *name))(void);

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLbitfield;
typedef unsigned char GLubyte;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
typedef unsigned long long GLuint64;
typedef struct __GLsync *GLsync;

#define GL_EXTENSIONS 0x1F03
#define GL_NUM_EXTENSIONS 0x821D
#define GL_ATOMIC_COUNTER_BUFFER 0x92C0
#define GL_ATOMIC_COUNTER_BUFFER_BINDING 0x92C1
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001

static const GLubyte *(*gl_get_string)(GLenum);
static const GLubyte *(*gl_get_stringi)(GLenum, GLuint);
static void (*gl_get_integerv)(GLenum, GLint *);
static void (*gl_bind_buffer)(GLenum, GLuint);
static void *(*gl_map_buffer_range)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
static unsigned char (*gl_unmap_buffer)(GLenum);
static void (*gl_buffer_sub_data)(GLenum, GLintptr, GLsizeiptr, const void *);
static GLsync (*gl_fence_sync)(GLenum, GLbitfield);
static void (*gl_delete_sync)(GLsync);
static GLenum (*gl_client_wait_sync)(GLsync, GLbitfield, GLuint64);

/* (eglGetProcAddress needs no context or display, so the guest's imports
resolve when the image loads, before SDL is up) */
void *host_gl_resolve(const char *name)
{
	return (void *)eglGetProcAddress(name);
}

#define BIND(pointer, name) (*(void **)&(pointer) = (void *)eglGetProcAddress(name))

void host_gl_bind_functions(void)
{
	BIND(gl_get_string, "glGetString");
	BIND(gl_get_stringi, "glGetStringi");
	BIND(gl_get_integerv, "glGetIntegerv");
	BIND(gl_bind_buffer, "glBindBuffer");
	BIND(gl_map_buffer_range, "glMapBufferRange");
	BIND(gl_unmap_buffer, "glUnmapBuffer");
	BIND(gl_buffer_sub_data, "glBufferSubData");
	BIND(gl_fence_sync, "glFenceSync");
	BIND(gl_delete_sync, "glDeleteSync");
	BIND(gl_client_wait_sync, "glClientWaitSync");
	if (gl_get_string)
	{
		host_logf(HOST_LOG_INFO, "GL: %s; %s; %s", (const char *)gl_get_string(0x1F00), (const char *)gl_get_string(0x1F01),
			(const char *)gl_get_string(0x1F02));
	}
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text = NULL;

	if (!size)
		return;
	buffer[0] = 0;
	if (index >= 0 && gl_get_stringi)
		text = gl_get_stringi(name, (GLuint)index);
	else if (index < 0 && gl_get_string)
		text = gl_get_string(name);
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	if (!gl_get_integerv || !gl_get_stringi)
		return 0;
	gl_get_integerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)gl_get_stringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

uint32_t host_gl_read_buffer_word(uint32_t buffer, uint32_t offset)
{
	uint32_t value = 0;
	GLint previous = 0;
	const void *mapping;

	gl_get_integerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &previous);
	gl_bind_buffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
	mapping = gl_map_buffer_range(GL_ATOMIC_COUNTER_BUFFER, offset, sizeof(value), GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(&value, mapping, sizeof(value));
		gl_unmap_buffer(GL_ATOMIC_COUNTER_BUFFER);
	}
	gl_bind_buffer(GL_ATOMIC_COUNTER_BUFFER, (GLuint)previous);
	return value;
}

#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(uint32_t slot)
{
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		gl_delete_sync(frame_fences[slot]);
	frame_fences[slot] = gl_fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

/* ticks spent waiting for the GPU to finish an earlier frame (the
watchdog's line in switch_main.c): the CPU idles there when the GPU is the
slower */
static volatile uint64_t gpu_wait_ticks;

uint64_t host_gl_wait_ticks(void)
{
	return gpu_wait_ticks;
}

void host_gl_wait_frame(uint32_t slot)
{
	uint64_t started;

	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	started = armGetSystemTick();
	gl_client_wait_sync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	__atomic_add_fetch(&gpu_wait_ticks, armGetSystemTick() - started, __ATOMIC_RELAXED);
	gl_delete_sync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping = gl_map_buffer_range(target, offset, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);

	if (!mapping)
	{
		gl_buffer_sub_data(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	gl_unmap_buffer(target);
}

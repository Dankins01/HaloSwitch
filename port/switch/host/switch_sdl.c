/*
SWITCH_SDL.C

The guest's SDL3 calls (port/android/guest/runtime/guest_sdl.c), served
with devkitPro's SDL2 for video and audio and libnx's pad API for
controllers.

The guest passes SDL3's values: its init flags, GL attributes, event
types and structures (sdl3_values.h, generated from SDL 3.4.16's
headers by tools/sdl3_values_probe.c). Where SDL2 numbers them the same
they pass through; where not (GL attributes, events) they are translated.
SDL3 functions report success as true, SDL2's as 0.

Controllers: the four players' pads, the first also taking the handheld
Joy-Con, are SDL3 gamepads 1 to 4. Their buttons are reported by
position, as the game's Xbox layout expects: the bottom face button (B on
a Switch controller) is SDL3's "south" (Xbox A, jump). A GameCube
controller is reported by its own layout instead (its big A jumps).
Joy-Con and Pro Controller ZL and ZR are switches, so their triggers read
fully pressed or released; a GameCube controller's triggers are analog.
Rumble goes to the controller's HD rumble (or a GameCube controller's
motor), and stops when the game stops asking for it.
*/

#include "switch_host.h"
#include "sdl3_values.h"

#include <switch.h>
#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- handles */

#define HANDLE_COUNT 64

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_audio,
};

struct handle
{
	int type;
	void *object;
};

static struct handle handles[HANDLE_COUNT];
static Mutex handle_lock;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	mutexLock(&handle_lock);
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			mutexUnlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			mutexUnlock(&handle_lock);
			return index;
		}
	}
	mutexUnlock(&handle_lock);
	host_logf(HOST_LOG_ERROR, "out of SDL handles");
	return 0;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	mutexLock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	mutexUnlock(&handle_lock);
	return object;
}

/* ---------- controllers */

#define PAD_COUNT 4

static PadState pads[PAD_COUNT];
static int pads_ready;
static int pad_connected[PAD_COUNT];

static void pads_initialize(void)
{
	if (pads_ready)
		return;
	padConfigureInput(PAD_COUNT, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadGc);
	padInitialize(&pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
	padInitialize(&pads[1], HidNpadIdType_No2);
	padInitialize(&pads[2], HidNpadIdType_No3);
	padInitialize(&pads[3], HidNpadIdType_No4);
	pads_ready = 1;
}

/* ---------- rumble

Each controller's vibration devices (two for a Pro Controller, a Joy-Con
pair or handheld Joy-Con, one for a single Joy-Con or a GameCube
controller) are set up for the controller id and style it has now, again
when they change. The game asks for 100 ms of rumble at a time and asks
again while it lasts (port/linux/src/xinput_sdl.c), so rumble stops when
its time runs out. */

struct rumble
{
	HidNpadIdType id;
	u32 style;
	HidVibrationDeviceHandle handles[2];
	int handle_count;
	int ready;
	int active;
	u64 end_tick;
};

static struct rumble rumbles[PAD_COUNT];

static HidNpadIdType pad_id(const PadState *pad, int index)
{
	int id;

	if (index == 0 && padIsHandheld(pad))
		return HidNpadIdType_Handheld;
	for (id = 0; id < 8; id++)
	{
		if (pad->active_id_mask & BIT(id))
			return (HidNpadIdType)id;
	}
	return (HidNpadIdType)index;
}

static u32 pad_style(const PadState *pad)
{
	static const u32 order[] = { HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadHandheld,
		HidNpadStyleTag_NpadJoyDual, HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight,
		HidNpadStyleTag_NpadGc };
	u32 styles = padGetStyleSet(pad);
	size_t index;

	for (index = 0; index < sizeof(order) / sizeof(order[0]); index++)
	{
		if (styles & order[index])
			return order[index];
	}
	return 0;
}

static int pad_is_gamecube(const PadState *pad)
{
	return pad_style(pad) == HidNpadStyleTag_NpadGc;
}

/* the controller's devices, for its id and style now; 0 if it has none */
static int rumble_prepare(int index)
{
	struct rumble *rumble = &rumbles[index];
	PadState *pad = &pads[index];
	HidNpadIdType id = pad_id(pad, index);
	u32 style = pad_style(pad);
	Result result;

	if (!style)
		return 0;
	if (rumble->ready && rumble->id == id && rumble->style == style)
		return rumble->handle_count;
	rumble->ready = 1;
	rumble->id = id;
	rumble->style = style;
	rumble->handle_count = (style & (HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld |
		HidNpadStyleTag_NpadJoyDual)) ? 2 : 1;
	result = hidInitializeVibrationDevices(rumble->handles, rumble->handle_count, id, (HidNpadStyleTag)style);
	if (R_FAILED(result))
	{
		host_logf(HOST_LOG_WARN, "controller %d: no rumble (0x%x)", index + 1, result);
		rumble->handle_count = 0;
	}
	else
	{
		host_logf(HOST_LOG_INFO, "controller %d: rumble on %d device(s), id %d, style 0x%x", index + 1,
			rumble->handle_count, (int)id, style);
	}
	return rumble->handle_count;
}

static void rumble_send(int index, uint32_t low, uint32_t high)
{
	struct rumble *rumble = &rumbles[index];
	int count = rumble_prepare(index), device;

	if (!count)
		return;
	if (rumble->style == HidNpadStyleTag_NpadGc)
	{
		/* one motor, on or off */
		hidSendVibrationGcErmCommand(rumble->handles[0], (low || high) ?
			HidVibrationGcErmCommand_Start : HidVibrationGcErmCommand_Stop);
		return;
	}
	{
		HidVibrationValue values[2];

		for (device = 0; device < count; device++)
		{
			/* the Xbox's heavy motor as the low band, its light one as the
			high band, on both sides */
			values[device].amp_low = (float)low / 65535.0f;
			values[device].freq_low = 160.0f;
			values[device].amp_high = (float)high / 65535.0f;
			values[device].freq_high = 320.0f;
		}
		{
			static int reported;
			Result result = hidSendVibrationValues(rumble->handles, values, count);

			if (R_FAILED(result) && !reported)
			{
				reported = 1;
				host_logf(HOST_LOG_WARN, "controller %d: rumble not sent (0x%x)", index + 1, result);
			}
		}
	}
}

/* stops rumble whose time has run out (pads_update) */
static void rumble_expire(void)
{
	u64 now = armGetSystemTick();
	int index;

	for (index = 0; index < PAD_COUNT; index++)
	{
		if (rumbles[index].active && now >= rumbles[index].end_tick)
		{
			rumbles[index].active = 0;
			rumble_send(index, 0, 0);
		}
	}
}

static void pads_update(void)
{
	int index;

	pads_initialize();
	for (index = 0; index < PAD_COUNT; index++)
		padUpdate(&pads[index]);
	rumble_expire();
}

/* gamepad ids (and handles) are 1 to 4 */
static PadState *pad_of(uint32_t id)
{
	if (id < 1 || id > PAD_COUNT || !pads_ready)
		return NULL;
	return &pads[id - 1];
}

/* ---------- events made here (gamepads), delivered before SDL's */

#define QUEUE_SIZE 16

static unsigned char queued_events[QUEUE_SIZE][S3_EVENT_SIZE];
static int queue_head, queue_count;

static void queue_event(uint32_t type, uint32_t which)
{
	unsigned char *event;
	uint64_t timestamp = armTicksToNs(armGetSystemTick());

	if (queue_count == QUEUE_SIZE)
		return;
	event = queued_events[(queue_head + queue_count++) % QUEUE_SIZE];
	memset(event, 0, S3_EVENT_SIZE);
	memcpy(event + S3_OFF_TYPE, &type, 4);
	memcpy(event + S3_OFF_TIMESTAMP, &timestamp, 8);
	memcpy(event + S3_OFF_GAMEPAD_WHICH, &which, 4);
}

static void check_connections(void)
{
	int index;

	for (index = 0; index < PAD_COUNT; index++)
	{
		int connected = padIsConnected(&pads[index]);

		if (connected != pad_connected[index])
		{
			pad_connected[index] = connected;
			queue_event(connected ? S3_EVENT_GAMEPAD_ADDED : S3_EVENT_GAMEPAD_REMOVED, (uint32_t)index + 1);
			host_logf(HOST_LOG_INFO, "controller %d %s", index + 1, connected ? "connected" : "disconnected");
		}
	}
}

/* ---------- general */

/* frames per audio callback: at least AUDIO_MINIMUM_FRAMES whatever the
game asks (512, 11 ms). Each callback passes between two threads here
(audio_callback, audio_thread), and at 11 ms one late handoff while the
game draws is a gap in the sound, heard as crackling. */
#define AUDIO_MINIMUM_FRAMES 2048
/* above the game's threads (0x2c; a lower number runs first) */
#define AUDIO_THREAD_PRIORITY 0x28

static int audio_frames = AUDIO_MINIMUM_FRAMES;

int host_sdl_init(uint32_t flags)
{
	Uint32 sdl2 = 0;

	if (flags & S3_INIT_AUDIO)
		sdl2 |= SDL_INIT_AUDIO;
	if (flags & S3_INIT_VIDEO)
		sdl2 |= SDL_INIT_VIDEO;
	if (flags & S3_INIT_EVENTS)
		sdl2 |= SDL_INIT_EVENTS;
	if (flags & (S3_INIT_GAMEPAD | S3_INIT_JOYSTICK))
	{
		pads_update();
		check_connections();
	}
	return SDL_InitSubSystem(sdl2) == 0;
}

int host_sdl_set_hint(const char *name, const char *value)
{
	if (name && value && !strcmp(name, "SDL_AUDIO_DEVICE_SAMPLE_FRAMES"))
	{
		int frames = atoi(value);

		if (frames >= 64 && frames <= 16384)
			audio_frames = frames < AUDIO_MINIMUM_FRAMES ? AUDIO_MINIMUM_FRAMES : frames;
		return 1;
	}
	return SDL_SetHint(name, value) ? 1 : 0;
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetError(), size);
}

void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetScancodeName((SDL_Scancode)scancode), size);
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	return (int32_t)SDL_GetScancodeFromName(name);
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)SDL_GetTicks64();
}

int64_t host_sdl_thread_id(void)
{
	return (int64_t)SDL_ThreadID();
}

/* ---------- video */

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	SDL_Window *window;

	(void)width;
	(void)height;
	(void)flags;
	/* one full-screen window, at the handheld resolution (docked, the
	console scales it) */
	window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720,
		SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
	if (!window)
		host_logf(HOST_LOG_ERROR, "SDL_CreateWindow: %s", SDL_GetError());
	return handle_new(_handle_window, window);
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
	if (object)
		SDL_GL_GetDrawableSize(object, width, height);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	(void)window;
	(void)enabled;
	return 1;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	SDL_GLattr translated;

	switch (attribute)
	{
	case S3_GL_RED_SIZE: translated = SDL_GL_RED_SIZE; break;
	case S3_GL_GREEN_SIZE: translated = SDL_GL_GREEN_SIZE; break;
	case S3_GL_BLUE_SIZE: translated = SDL_GL_BLUE_SIZE; break;
	case S3_GL_ALPHA_SIZE: translated = SDL_GL_ALPHA_SIZE; break;
	case S3_GL_BUFFER_SIZE: translated = SDL_GL_BUFFER_SIZE; break;
	case S3_GL_DOUBLEBUFFER: translated = SDL_GL_DOUBLEBUFFER; break;
	case S3_GL_DEPTH_SIZE: translated = SDL_GL_DEPTH_SIZE; break;
	case S3_GL_STENCIL_SIZE: translated = SDL_GL_STENCIL_SIZE; break;
	case S3_GL_MULTISAMPLEBUFFERS: translated = SDL_GL_MULTISAMPLEBUFFERS; break;
	case S3_GL_MULTISAMPLESAMPLES: translated = SDL_GL_MULTISAMPLESAMPLES; break;
	case S3_GL_CONTEXT_MAJOR_VERSION: translated = SDL_GL_CONTEXT_MAJOR_VERSION; break;
	case S3_GL_CONTEXT_MINOR_VERSION: translated = SDL_GL_CONTEXT_MINOR_VERSION; break;
	case S3_GL_CONTEXT_FLAGS:
		translated = SDL_GL_CONTEXT_FLAGS;
		/* no debug contexts: mesa's debug output is slow */
		value &= ~S3_GL_CONTEXT_DEBUG_FLAG;
		break;
	case S3_GL_CONTEXT_PROFILE_MASK:
		/* SDL2's profile values are SDL3's */
		translated = SDL_GL_CONTEXT_PROFILE_MASK;
		break;
	case S3_GL_FRAMEBUFFER_SRGB_CAPABLE: translated = SDL_GL_FRAMEBUFFER_SRGB_CAPABLE; break;
	default:
		host_logf(HOST_LOG_WARN, "SDL3 GL attribute %d is not passed on", attribute);
		return 1;
	}
	return SDL_GL_SetAttribute(translated, value) == 0;
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_GLContext context;

	if (!object)
		return 0;
	context = SDL_GL_CreateContext(object);
	if (!context)
	{
		host_logf(HOST_LOG_ERROR, "SDL_GL_CreateContext: %s", SDL_GetError());
		return 0;
	}
	host_gl_bind_functions();
	return handle_new(_handle_context, context);
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	return SDL_GL_MakeCurrent(handle_get(window, _handle_window), handle_get(context, _handle_context)) == 0;
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return SDL_GL_SetSwapInterval(interval) == 0;
}

int host_sdl_gl_swap_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	if (!object)
		return 0;
	{
		uint64_t started = armGetSystemTick();

		SDL_GL_SwapWindow(object);
		host_note_frame(started);
	}
	return 1;
}

/* ---------- events */

static int translate_event(const SDL_Event *in, unsigned char *out)
{
	uint32_t type;
	uint64_t timestamp = armTicksToNs(armGetSystemTick());

	switch (in->type)
	{
	case SDL_QUIT:
		type = S3_EVENT_QUIT;
		break;
	case SDL_WINDOWEVENT:
		switch (in->window.event)
		{
		case SDL_WINDOWEVENT_FOCUS_GAINED: type = S3_EVENT_WINDOW_FOCUS_GAINED; break;
		case SDL_WINDOWEVENT_FOCUS_LOST: type = S3_EVENT_WINDOW_FOCUS_LOST; break;
		case SDL_WINDOWEVENT_CLOSE: type = S3_EVENT_WINDOW_CLOSE_REQUESTED; break;
		default: return 0;
		}
		break;
	default:
		/* (keyboard and mouse: none on the Switch) */
		return 0;
	}
	memset(out, 0, S3_EVENT_SIZE);
	memcpy(out + S3_OFF_TYPE, &type, 4);
	memcpy(out + S3_OFF_TIMESTAMP, &timestamp, 8);
	return 1;
}

int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;

	if (queue_count)
	{
		memcpy(event, queued_events[queue_head], S3_EVENT_SIZE);
		queue_head = (queue_head + 1) % QUEUE_SIZE;
		queue_count--;
		return 1;
	}
	while (SDL_PollEvent(&host_event))
	{
		if (translate_event(&host_event, event))
			return 1;
	}
	/* the end of this frame's events: read the controllers for the next */
	pads_update();
	check_connections();
	return 0;
}

/* ---------- gamepads */

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, index;

	pads_update();
	for (index = 0; index < PAD_COUNT && count < capacity; index++)
	{
		pad_connected[index] = padIsConnected(&pads[index]);
		if (pad_connected[index])
			ids[count++] = (uint32_t)index + 1;
	}
	return count;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	pads_initialize();
	return pad_of(id) ? id : 0;
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return pad_of(id) ? id : 0;
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	PadState *pad = pad_of(gamepad);
	HidAnalogStickState stick;
	u64 buttons;

	if (!pad)
		return 0;
	buttons = padGetButtons(pad);
	switch (axis)
	{
	case S3_GAMEPAD_AXIS_LEFTX:
		return padGetStickPos(pad, 0).x;
	case S3_GAMEPAD_AXIS_LEFTY:
		/* SDL's y grows downwards */
		stick = padGetStickPos(pad, 0);
		return stick.y == -32768 ? 32767 : -stick.y;
	case S3_GAMEPAD_AXIS_RIGHTX:
		return padGetStickPos(pad, 1).x;
	case S3_GAMEPAD_AXIS_RIGHTY:
		stick = padGetStickPos(pad, 1);
		return stick.y == -32768 ? 32767 : -stick.y;
	case S3_GAMEPAD_AXIS_LEFT_TRIGGER:
	case S3_GAMEPAD_AXIS_RIGHT_TRIGGER:
	{
		int right = axis == S3_GAMEPAD_AXIS_RIGHT_TRIGGER;

		/* (a GameCube controller's are analog, 0 to 0x7fff, and click at
		the end of their travel: ZL or ZR then) */
		if (buttons & (right ? HidNpadButton_ZR : HidNpadButton_ZL))
			return 32767;
		if (pad_is_gamecube(pad))
		{
			u32 position = padGetGcTriggerPos(pad, right ? 1 : 0);

			return position > 32767 ? 32767 : (int)position;
		}
		return 0;
	}
	default:
		return 0;
	}
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	PadState *pad = pad_of(gamepad);
	u64 buttons, mask;

	if (!pad)
		return 0;
	buttons = padGetButtons(pad);
	if (pad_is_gamecube(pad))
	{
		/* a GameCube controller by its own layout: its big A is the main
		button (Xbox A, jump), B beside it (Xbox B, melee), X and Y as
		marked; Z is the right shoulder (Xbox black) */
		switch (button)
		{
		case S3_GAMEPAD_BUTTON_SOUTH: mask = HidNpadButton_A; break;
		case S3_GAMEPAD_BUTTON_EAST: mask = HidNpadButton_B; break;
		case S3_GAMEPAD_BUTTON_WEST: mask = HidNpadButton_X; break;
		case S3_GAMEPAD_BUTTON_NORTH: mask = HidNpadButton_Y; break;
		case S3_GAMEPAD_BUTTON_START: mask = HidNpadButton_Plus; break;
		case S3_GAMEPAD_BUTTON_RIGHT_SHOULDER: mask = HidNpadButton_R; break;
		case S3_GAMEPAD_BUTTON_DPAD_UP: mask = HidNpadButton_Up; break;
		case S3_GAMEPAD_BUTTON_DPAD_DOWN: mask = HidNpadButton_Down; break;
		case S3_GAMEPAD_BUTTON_DPAD_LEFT: mask = HidNpadButton_Left; break;
		case S3_GAMEPAD_BUTTON_DPAD_RIGHT: mask = HidNpadButton_Right; break;
		default: return 0;
		}
		return (buttons & mask) != 0;
	}
	switch (button)
	{
	/* by position (Xbox A is the bottom button) */
	case S3_GAMEPAD_BUTTON_SOUTH: mask = HidNpadButton_B; break;
	case S3_GAMEPAD_BUTTON_EAST: mask = HidNpadButton_A; break;
	case S3_GAMEPAD_BUTTON_WEST: mask = HidNpadButton_Y; break;
	case S3_GAMEPAD_BUTTON_NORTH: mask = HidNpadButton_X; break;
	case S3_GAMEPAD_BUTTON_BACK: mask = HidNpadButton_Minus; break;
	case S3_GAMEPAD_BUTTON_START: mask = HidNpadButton_Plus; break;
	case S3_GAMEPAD_BUTTON_LEFT_STICK: mask = HidNpadButton_StickL; break;
	case S3_GAMEPAD_BUTTON_RIGHT_STICK: mask = HidNpadButton_StickR; break;
	case S3_GAMEPAD_BUTTON_LEFT_SHOULDER: mask = HidNpadButton_L; break;
	case S3_GAMEPAD_BUTTON_RIGHT_SHOULDER: mask = HidNpadButton_R; break;
	case S3_GAMEPAD_BUTTON_DPAD_UP: mask = HidNpadButton_Up; break;
	case S3_GAMEPAD_BUTTON_DPAD_DOWN: mask = HidNpadButton_Down; break;
	case S3_GAMEPAD_BUTTON_DPAD_LEFT: mask = HidNpadButton_Left; break;
	case S3_GAMEPAD_BUTTON_DPAD_RIGHT: mask = HidNpadButton_Right; break;
	default: return 0;
	}
	return (buttons & mask) != 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	return pad_of(gamepad) ? S3_GAMEPAD_TYPE_STANDARD : S3_GAMEPAD_TYPE_UNKNOWN;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	int index;

	if (!pad_of(gamepad))
		return 0;
	index = (int)gamepad - 1;
	if (low || high)
	{
		static int reported;

		if (!reported)
		{
			bool permitted = true;

			reported = 1;
			hidIsVibrationPermitted(&permitted);
			host_logf(HOST_LOG_INFO, "the game asks controller %d to rumble (%u, %u); the console %s vibration",
				index + 1, low, high, permitted ? "allows" : "has turned off");
		}
	}
	rumble_send(index, low, high);
	if (low || high)
	{
		rumbles[index].active = 1;
		rumbles[index].end_tick = armGetSystemTick() + armNsToTicks((u64)milliseconds * 1000000ULL);
	}
	else
	{
		rumbles[index].active = 0;
	}
	return 1;
}

/* ---------- audio

SDL3's stream callback asks for `additional` more bytes; SDL2's device
callback asks for exactly `len`. SDL2 calls it on its own audio thread,
whose stack is not in guest memory, so it hands each request to a thread
that can run guest code (audio_thread) and waits. What the guest puts into
the stream collects in the binding's buffer; the SDL2 callback takes `len`
bytes from it and keeps the rest for the next call. */

struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	SDL_AudioDeviceID device;
	unsigned char silence;
	Mutex lock;
	CondVar requested;
	CondVar done;
	int pending;
	int additional;
	int total;
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
};

static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length <= 0)
		return 1;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	svcSetThreadPriority(CUR_THREAD_HANDLE, AUDIO_THREAD_PRIORITY);
	mutexLock(&binding->lock);
	for (;;)
	{
		int additional, total;

		while (!binding->pending)
			condvarWait(&binding->requested, &binding->lock);
		additional = binding->additional;
		total = binding->total;
		mutexUnlock(&binding->lock);
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional, (uint32_t)total);
		mutexLock(&binding->lock);
		binding->pending = 0;
		condvarWakeAll(&binding->done);
	}
	return NULL;
}

static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int length)
{
	struct audio_binding *binding = userdata;
	static int raised;
	int taken;

	/* SDL's own audio thread, likewise above the game's */
	if (!raised)
	{
		raised = 1;
		svcSetThreadPriority(CUR_THREAD_HANDLE, AUDIO_THREAD_PRIORITY);
	}
	mutexLock(&binding->lock);
	if (binding->buffer_length < length)
	{
		binding->additional = length - binding->buffer_length;
		binding->total = length;
		binding->pending = 1;
		condvarWakeAll(&binding->requested);
		while (binding->pending)
			condvarWait(&binding->done, &binding->lock);
	}
	taken = binding->buffer_length < length ? binding->buffer_length : length;
	memcpy(stream, binding->buffer, (size_t)taken);
	if (taken < length)
		memset(stream + taken, binding->silence, (size_t)(length - taken));
	memmove(binding->buffer, binding->buffer + taken, (size_t)(binding->buffer_length - taken));
	binding->buffer_length -= taken;
	mutexUnlock(&binding->lock);
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	const unsigned char *guest_spec = spec;
	struct audio_binding *binding = calloc(1, sizeof(*binding));
	SDL_AudioSpec wanted, obtained;
	uint32_t format;
	int32_t channels, frequency;

	(void)device;
	if (!binding)
		return 0;
	memcpy(&format, guest_spec + S3_OFF_SPEC_FORMAT, 4);
	memcpy(&channels, guest_spec + S3_OFF_SPEC_CHANNELS, 4);
	memcpy(&frequency, guest_spec + S3_OFF_SPEC_FREQ, 4);
	binding->callback = callback;
	binding->userdata = userdata;
	mutexInit(&binding->lock);
	condvarInit(&binding->requested);
	condvarInit(&binding->done);

	SDL_zero(wanted);
	/* SDL2's format values are SDL3's (AUDIO_F32LSB is SDL_AUDIO_F32LE) */
	wanted.format = (SDL_AudioFormat)format;
	wanted.channels = (Uint8)channels;
	wanted.freq = frequency;
	wanted.samples = (Uint16)audio_frames;
	wanted.callback = callback ? audio_callback : NULL;
	wanted.userdata = binding;
	/* no changes allowed: SDL converts to what the device takes */
	binding->device = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
	if (!binding->device)
	{
		host_logf(HOST_LOG_ERROR, "SDL_OpenAudioDevice: %s", SDL_GetError());
		free(binding);
		return 0;
	}
	binding->silence = obtained.silence;
	host_logf(HOST_LOG_INFO, "audio: %d Hz, %d channels, format 0x%x, %d frames",
		frequency, channels, format, obtained.samples);
	/* the device starts paused, so no callback runs before this */
	binding->handle = handle_new(_handle_audio, binding);
	if (callback && host_native_thread_create(audio_thread, binding, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	struct audio_binding *binding = handle_get(stream, _handle_audio);
	int result;

	if (!binding)
		return 0;
	if (!binding->callback)
		return SDL_QueueAudio(binding->device, data, (Uint32)length) == 0;
	/* (during a callback the lock is free: audio_callback waits on a
	condition, which releases it) */
	mutexLock(&binding->lock);
	result = audio_keep(binding, data, length);
	mutexUnlock(&binding->lock);
	return result;
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	struct audio_binding *binding = handle_get(stream, _handle_audio);

	if (!binding)
		return 0;
	SDL_PauseAudioDevice(binding->device, 0);
	return 1;
}

/* ---------- the clipboard, notices, messages */

int host_sdl_set_clipboard_text(const char *text)
{
	return SDL_SetClipboardText(text) == 0;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	char *text = SDL_GetClipboardText();

	SDL_strlcpy(buffer, text ? text : "", size);
	SDL_free(text);
}

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	host_logf(HOST_LOG_INFO, "notice: %s", message);
	return 1;
}

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	(void)flags;
	host_logf(HOST_LOG_WARN, "%s: %s", title, message);
	return host_show_error(message);
}

/* ---------- Android's storage paths (the guest's platform layer asks) */

void host_android_path(int which, char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", which ? SWITCH_SAVE_ROOT : SWITCH_DATA_ROOT);
}

#include <stdlib.h>
#include <string.h>
#include "libretro.h"
#include "system.h"
#include "util.h"
#include "vdp.h"
#include "render.h"
#include "io.h"
#include "genesis.h"
#include "sms.h"

static retro_environment_t retro_environment;
RETRO_API void retro_set_environment(retro_environment_t re)
{
	retro_environment = re;
#	define input_descriptor_macro(pad_num) \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Z" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "C" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Mode" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" }, \

	static const struct retro_input_descriptor desc[] = {
		input_descriptor_macro(0)
		input_descriptor_macro(1)
		input_descriptor_macro(2)
		input_descriptor_macro(3)
		input_descriptor_macro(4)
		input_descriptor_macro(5)
		input_descriptor_macro(6)
		input_descriptor_macro(7)
		{ 0 },
	};

	re(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
}

static retro_video_refresh_t retro_video_refresh;
RETRO_API void retro_set_video_refresh(retro_video_refresh_t rvf)
{
	retro_video_refresh = rvf;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t ras)
{
}

static retro_audio_sample_batch_t retro_audio_sample_batch;
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t rasb)
{
	retro_audio_sample_batch = rasb;
}

static retro_input_poll_t retro_input_poll;
RETRO_API void retro_set_input_poll(retro_input_poll_t rip)
{
	retro_input_poll = rip;
}

static retro_input_state_t retro_input_state;
RETRO_API void retro_set_input_state(retro_input_state_t ris)
{
	retro_input_state = ris;
}

int headless = 0;
int exit_after = 0;
int z80_enabled = 1;
char *save_filename;
tern_node *config;
uint8_t use_native_states = 1;
system_header *current_system;
system_media media;

RETRO_API void retro_init(void)
{
	render_audio_initialized(RENDER_AUDIO_S16, 53693175 / (7 * 6 * 4), 2, 4, sizeof(int16_t));
#ifndef ENABLE_DEBUG_MSG
	// Disable debug message to hide BYOG context
	disable_stdout_messages();
#endif
}

RETRO_API void retro_deinit(void)
{
	if (current_system) {
		retro_unload_game();
	}
}

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	info->library_name = "BlastEm";
	info->library_version = "0.6.3-pre"; //TODO: share this with blastem.c
	info->valid_extensions = "md|gen|sms|bin|rom";
	info->need_fullpath = 0;
	info->block_extract = 0;
}

static vid_std video_standard;
static uint32_t last_width, last_height;
static uint32_t overscan_top, overscan_bot, overscan_left, overscan_right;
static void update_overscan(void)
{
	uint8_t overscan;
	if (retro_environment(RETRO_ENVIRONMENT_GET_OVERSCAN, &overscan) && overscan) {
		overscan_top = overscan_bot = overscan_left = overscan_right = 0;
	} else {
		if (video_standard == VID_NTSC) {
			overscan_top = 11;
			overscan_bot = 8;
			overscan_left = 13;
			overscan_right = 14;
		} else {
			overscan_top = 30;
			overscan_bot = 24;
			overscan_left = 13;
			overscan_right = 14;
		}
	}
}

static float get_aspect_ratio(void)
{
	float aspect_width = LINEBUF_SIZE - overscan_left - overscan_right;
	float aspect_height = (video_standard == VID_NTSC ? 243 : 294) - overscan_top - overscan_bot;
	return aspect_width / aspect_height;
}

static int32_t sample_rate;
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	update_overscan();
	last_width = LINEBUF_SIZE;
	info->geometry.base_width = info->geometry.max_width = LINEBUF_SIZE - (overscan_left + overscan_right);
	info->geometry.base_height = (video_standard == VID_NTSC ? 243 : 294) - (overscan_top + overscan_bot);
	last_height = info->geometry.base_height;
	info->geometry.max_height = info->geometry.base_height * 2;
	info->geometry.aspect_ratio = get_aspect_ratio();
	double master_clock = video_standard == VID_NTSC ? 53693175 : 53203395;
	double lines = video_standard == VID_NTSC ? 262 : 313;
	info->timing.fps = master_clock / (3420.0 * lines);
	info->timing.sample_rate = master_clock / (7 * 6 * 24); //sample rate of YM2612
	sample_rate = info->timing.sample_rate;
	render_audio_initialized(RENDER_AUDIO_S16, info->timing.sample_rate, 2, 4, sizeof(int16_t));
	//force adjustment of resampling parameters since target sample rate may have changed slightly
	current_system->set_speed_percent(current_system, 100);
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

/* Resets the current game. */
RETRO_API void retro_reset(void)
{
	current_system->soft_reset(current_system);
}

/* Runs the game for one video frame.
 * During retro_run(), input_poll callback must be called at least once.
 *
 * If a frame is not rendered for reasons where a game "dropped" a frame,
 * this still counts as a frame, and retro_run() should explicitly dupe
 * a frame if GET_CAN_DUPE returns true.
 * In this case, the video callback can take a NULL argument for data.
 */
static uint8_t started;
RETRO_API void retro_run(void)
{
	if (started) {
		current_system->resume_context(current_system);
	} else {
		current_system->start_context(current_system, NULL);
		started = 1;
	}
}

/* Returns the amount of data the implementation requires to serialize
 * internal state (save states).
 * Between calls to retro_load_game() and retro_unload_game(), the
 * returned size is never allowed to be larger than a previous returned
 * value, to ensure that the frontend can allocate a save state buffer once.
 */
RETRO_API size_t retro_serialize_size(void)
{
	return SERIALIZE_DEFAULT_SIZE;
}

/* Serializes internal state. If failed, or size is lower than
 * retro_serialize_size(), it should return false, true otherwise. */
RETRO_API bool retro_serialize(void *data, size_t size)
{
	size_t actual_size;
	uint8_t *tmp = current_system->serialize(current_system, &actual_size);
	if (actual_size > size) {
		free(tmp);
		return 0;
	}
	memcpy(data, tmp, actual_size);
	free(tmp);
	return 1;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
	current_system->deserialize(current_system, (uint8_t *)data, size);
	return 1;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

/* Loads a game. */
static system_type stype;
RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	if (game->path) {
		media.dir = path_dirname(game->path);
		media.name = basename_no_extension(game->path);
		media.extension = path_extension(game->path);
	}
	media.buffer = malloc(nearest_pow2(game->size));
	memcpy(media.buffer, game->data, game->size);
	media.size = game->size;
	stype = detect_system_type(&media);
	current_system = alloc_config_system(stype, &media, 0, 0);
	
	unsigned format = RETRO_PIXEL_FORMAT_XRGB8888;
	retro_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
	
	return current_system != NULL;
}

/* Loads a "special" kind of game. Should not be used,
 * except in extreme cases. */
RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
	return retro_load_game(info);
}

/* Unloads a currently loaded game. */
RETRO_API void retro_unload_game(void)
{
	free(media.dir);
	free(media.name);
	free(media.extension);
	media.dir = media.name = media.extension = NULL;
	//buffer is freed by the context
	media.buffer = NULL;
	current_system->free_context(current_system);
	current_system = NULL;
}

/* Gets region of game. */
RETRO_API unsigned retro_get_region(void)
{
	return video_standard == VID_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

/* Gets region of memory. */
RETRO_API void *retro_get_memory_data(unsigned id)
{
	switch (id) {
	case RETRO_MEMORY_SYSTEM_RAM:
		switch (stype) {
		case SYSTEM_GENESIS: {
			genesis_context *gen = (genesis_context *)current_system;
			return (uint8_t *)gen->work_ram;
		}
#ifndef NO_Z80
		case SYSTEM_SMS: {
			sms_context *sms = (sms_context *)current_system;
			return sms->ram;
		}
#endif
		}
		break;
	case RETRO_MEMORY_SAVE_RAM:
		if (stype == SYSTEM_GENESIS) {
			genesis_context *gen = (genesis_context *)current_system;
			if (gen->save_type != SAVE_NONE)
				return gen->save_storage;
		}
		break;
	default:
		break;
	}
	return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	switch (id) {
	case RETRO_MEMORY_SYSTEM_RAM:
		switch (stype) {
		case SYSTEM_GENESIS:
			return RAM_WORDS * sizeof(uint16_t);
#ifndef NO_Z80
		case SYSTEM_SMS:
			return SMS_RAM_SIZE;
#endif
		}
		break;
	case RETRO_MEMORY_SAVE_RAM:
		if (stype == SYSTEM_GENESIS) {
			genesis_context *gen = (genesis_context *)current_system;
			if (gen->save_type != SAVE_NONE)
				return gen->save_size;
		}
		break;
	default:
		break;
	}
	return 0;
}

//blastem render backend API implementation
uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return r << 16 | g << 8 | b;
}

uint8_t render_create_window(char *caption, uint32_t width, uint32_t height, window_close_handler close_handler)
{
	//not supported in lib build
	return 0;
}

void render_destroy_window(uint8_t which)
{
	//not supported in lib build
}

static uint32_t fb[LINEBUF_SIZE * 294 * 2];
static uint8_t last_fb;
uint32_t *render_get_framebuffer(uint8_t which, int *pitch)
{
	*pitch = LINEBUF_SIZE * sizeof(uint32_t);
	if (which != last_fb) {
		*pitch = *pitch * 2;
	}

	if (which) {
		return fb + LINEBUF_SIZE;
	} else {
		return fb;
	}
}

void render_framebuffer_updated(uint8_t which, int width)
{
	unsigned height = (video_standard == VID_NTSC ? 243 : 294) - (overscan_top + overscan_bot);
	width -= (overscan_left + overscan_right);
	unsigned base_height = height;
	if (which != last_fb) {
		height *= 2;
		last_fb = which;
	}
	if (width != last_width || height != last_height) {
		struct retro_game_geometry geometry = {
			.base_width = width,
			.base_height = height,
			.max_width = width,
			.max_height = height * 2,
			.aspect_ratio = get_aspect_ratio()
		};
		retro_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
		last_width = width;
		last_height = height;
	}
	retro_video_refresh(fb + overscan_left + LINEBUF_SIZE * overscan_top, width, height, LINEBUF_SIZE * sizeof(uint32_t));
	system_request_exit(current_system, 0);
}

uint8_t render_get_active_framebuffer(void)
{
	return 0;
}

void render_set_video_standard(vid_std std)
{
	video_standard = std;
}

int render_fullscreen(void)
{
	return 1;
}

uint32_t render_overscan_top()
{
	return overscan_top;
}

uint32_t render_overscan_bot()
{
	return overscan_bot;
}

void process_events()
{
#define MAX_PLAYER 12
	static int16_t prev_state[MAX_PLAYER][RETRO_DEVICE_ID_JOYPAD_L2];
	static const uint8_t map[] = {
		BUTTON_A, BUTTON_X, BUTTON_MODE, BUTTON_START, DPAD_UP, DPAD_DOWN,
		DPAD_LEFT, DPAD_RIGHT, BUTTON_B, BUTTON_Y, BUTTON_Z, BUTTON_C
	};
	//TODO: handle other input device types
	//TODO: handle more than 2 ports when appropriate
	retro_input_poll();
	for (int port = 0; port < MAX_PLAYER; port++)
	{
		for (int id = RETRO_DEVICE_ID_JOYPAD_B; id < RETRO_DEVICE_ID_JOYPAD_L2; id++)
		{
			int16_t new_state = retro_input_state(port, RETRO_DEVICE_JOYPAD, 0, id);
			if (new_state != prev_state[port][id]) {
				if (new_state) {
					current_system->gamepad_down(current_system, port + 1, map[id]);
				} else {
					current_system->gamepad_up(current_system, port + 1, map[id]);
				}
				prev_state[port][id] = new_state;
			}
		}
	}
}

void render_errorbox(char *title, char *message)
{
}
void render_warnbox(char *title, char *message)
{
}
void render_infobox(char *title, char *message)
{
}

uint8_t render_is_audio_sync(void)
{
	//whether this is true depends on the libretro frontend implementation
	//but the sync to audio path works better here
	return 1;
}

uint8_t render_should_release_on_exit(void)
{
	return 0;
}

void render_buffer_consumed(audio_source *src)
{
}

void *render_new_audio_opaque(void)
{
	return NULL;
}

void render_free_audio_opaque(void *opaque)
{
}

void render_lock_audio(void)
{
}

void render_unlock_audio()
{
}

uint32_t render_min_buffered(void)
{
	//not actually used in the sync to audio path
	return 4;
}

uint32_t render_audio_syncs_per_sec(void)
{
	return 0;
}

void render_audio_created(audio_source *src)
{
}

void render_do_audio_ready(audio_source *src)
{
	int16_t *tmp = src->front;
	src->front = src->back;
	src->back = tmp;
	src->front_populated = 1;
	src->buffer_pos = 0;
	if (all_sources_ready()) {
		int16_t buffer[8];
		int min_remaining_out;
		mix_and_convert((uint8_t *)buffer, sizeof(buffer), &min_remaining_out);
		retro_audio_sample_batch(buffer, sizeof(buffer)/(2*sizeof(*buffer)));
	}
}

void render_source_paused(audio_source *src, uint8_t remaining_sources)
{
}

void render_source_resumed(audio_source *src)
{
}

void render_set_external_sync(uint8_t ext_sync_on)
{
}

void bindings_set_mouse_mode(uint8_t mode)
{
}

void bindings_release_capture(void)
{
}

void bindings_reacquire_capture(void)
{
}

static const char rom_db_data[] = " \
# Column 3 (USA) \n\
8e52a5d0adbff3b2a15f32e9299b4ffdf35f5541 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Column 3 (USA) \n\
2e850c2b737098b9926ac0fc9b8b2116fc5aa48a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-23).md\n\
1e6f3ba839d23f7ba3e01d144b6c1c635207fc7d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL 98 (USA).md\n\
27ea0133251e96c091b5a96024eb099cdca21e40 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Wimbledon Championship Tennis (USA) (Beta).md\n\
402bdc507647d861ee7bb80599f528d3d5aeaf0f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Tiny Toon Adventures - ACME All-Stars (USA, Korea).md\n\
d64736a69fca430fc6a84a60335add0c765feb71 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-02-22).md\n\
188fa0adc8662d7a8eeac1f174811f794e081552 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Wimbledon (Europe).md\n\
6b7d8aa9d4b9d10c26dc079ab78e11766982cef2 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Dragon - The Bruce Lee Story (Europe).md\n\
d6fb86d73e1abc7b7f1aecf77a52fa3f759aedb1 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Street Racer (Europe).md\n\
95aa250ea47d14d60da9f0fed5b1aad1ff2c1862 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-15).md\n\
2fdb8879d50d963d984c280cab2e279b9479081f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-08).md\n\
f8689fd01dedced28ca417bf7ff8a9eea9973c3d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Troy Aikman NFL Football (USA).md\n\
820efb4a4d3d29036911d9077bb6c0a4ce7f36d4 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Lost Vikings, The (USA) (October, 1995).md\n\
1434551cec6f7a9ca6f7f9a8bb4aad4d89b0e3fb {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-06-14).md\n\
923ce9034f37167b65aec97e160f6fe34ea2da33 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NCAA Final Four Basketball (USA).md\n\
29021b8c3bbcc62606c692a3de90d4e7a71b6361 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-12-22).md\n\
35f7436fa15591234edcb6fe72da24d091963d30 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-12-09).md\n\
2c34500bf06bbac610e8fca45db48382e32c8807 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-02-02).md\n\
3a5a95a79b1b2da0b35e8cde02d8645fe474fdde {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-04-19).md\n\
9961db2c46f0189c419da2e335e6ca974eaa5379 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Olympic Gold (Europe) (En,Fr,De,Es,It,Nl,Pt,Sv).md\n\
55702a7dee0cd2092a751b19c04d81694b0c0d0f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '94 (USA).md\n\
e2b5290d656219636e2422fcf93424ae602c4d29 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL Quarterback Club 96 (USA, Europe).md\n\
2948419532a6079404f05348bc4bbf2dd989622d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-24).md\n\
eb99f0e1cac800f94743fde873448e56edb46333 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Pepenga Pengo (Japan).md\n\
c88c30d9e1fb6fb3a8aadde047158a3683bb6b1a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# International Superstar Soccer Deluxe (Europe, Brazil).md\n\
ccc60352b43f8c3d536267dd05a8f2c0f3b73df6 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL Football '94 Starring Joe Montana (USA).md\n\
ad0150d0c0cabe1ab05e3d6ac4eb7aa4b38a467c {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA).md\n\
a3db8661e160e07b09bca03ba0d20ba4e80a4c59 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-17).md\n\
839de31449298d0970bde787381a5e331e1fd31f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# J. League Pro Striker (Japan).md\n\
d3dcc24e50373234988061d3ef56c16d28e580ad {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-03) (Alt 1).md\n\
0b8e79d7fb38ec8816dc610ef4aee33cabbf08f3 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-29) (Alt 1).md\n\
400fd9a69a383468abec66032401d0ab6d8888fd {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Jam (USA, Europe) (Beta) (April, 1993).md\n\
1aad8b146ec1eb2a9823878fbfddc02efe3f6ad9 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Lost Vikings, The (Europe) (Beta).md\n\
375eaa9845692db4fdbd0b51985aa0892a8fe425 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour Championship Tennis (USA) (Beta) (1994-08-02).md\n\
3e29c757cedf2334d830f4375242c066f83e0d36 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Ultimate Soccer (Europe) (En,Fr,De,Es,It).md\n\
91781d0561f84de0d304221bbc26f4035f62010f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (January, 1994).md\n\
d3f4f2f5e165738bde6c6011c3d68322c27d97ed {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Mega Bomberman (Europe, Korea).md\n\
01f76e2f719bdae5f21ff0e5a1ac1262c2def279 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Ultimate Mortal Kombat 3 (Europe).md\n\
044bfdb3761df7c4d54a25898353fabcd3f604a3 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Dino Dini's Soccer (Europe).md\n\
49d4a654dd2f393e43a363ed171e73cd4c8ff4f4 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Olympic Gold (Japan, USA, Korea) (En,Fr,De,Es,It,Nl,Pt,Sv) (Rev A).md\n\
ce640bb979fcb285729b58de4da421c5493ef5e2 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour Championship Tennis (USA) (Beta) (1994-07-23).md\n\
88005e79f325e20c804e04a7a310a6d19b7f7cce {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Micro Machines - Military (Europe) (Beta) (1995).md\n\
d59c0db1728d017363fe3209b6bf8c981da75ae1 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour Championship Tennis (USA) (Beta) (1994-07-19).md\n\
e0e94be5c1f76465151cf6c6357d67ba68170676 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Yu Yu Hakusho - Makyou Toitsusen (Japan).md\n\
89a70d374a3f3e2dc5b430e6d444bea243193b74 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Lost Vikings, The (USA) (November, 1993).md\n\
f00464c111b57c8b23698760cbd377c3c8cfe712 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-06-01).md\n\
99e5ec2705fac1566e47fd813d6cf5b5e7f7daf4 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Prime Time NFL Starring Deion Sanders (USA).md\n\
7d7a5a920ac30831556b58caac66f4a8dde1632a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-18).md\n\
d3a4c99d46f3506821137779226ae09edfae0760 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Pele II - World Tournament Soccer (USA, Europe).md\n\
96498065545ed122aa29471d762a2ae1362b2dea {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL Football '94 (Japan).md\n\
a530a93d124ccecfbf54f0534b6d3269026e5988 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-09).md\n\
8e706299b04efc4b0e5e0b9b693c816cb8ccda72 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NCAA Football (USA).md\n\
227e3c650d01c35a80de8a3ef9b18f96c07ecd38 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Fever Pitch Soccer (Europe) (En,Fr,De,Es,It).md\n\
e3489b80a4b21049170fedee7630111773fe592c {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Wayne Gretzky and the NHLPA All-Stars (USA, Europe).md\n\
0b068f684e206139bcd592daba4613cbf634dd56 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Ultimate Soccer (Europe) (En,Fr,De,Es,It) (Beta).md\n\
892747034876a7f587bbf0e3f21c9f35237bb1ea {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Micro Machines (USA) (Rev 1).md\n\
0d4d023a1f9dc8b794bd60bf6e70465b712ffded {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour Championship Tennis (USA) (Beta) (1994-08-08).md\n\
2cade1465fd5a835523b688bb675f67a7012e67d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Barkley Shut Up and Jam! (USA, Europe).md\n\
fc60a682412b4f7f851c5eb7f6ae68fcee3d2dd1 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Wimbledon Championship Tennis (Japan).md\n\
73a6dad0bedc5552459e8a74a9b8ae242ad5a78e {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-26).md\n\
a82cfecf5a384f77f592251100d3447c5fa1e1c7 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-24).md\n\
6d50cc35d2da2d4ec8f3f090e9866747c07164f3 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-11-18).md\n\
492776fc2659091d435a79588efcdd8e06d3cd0b {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Yu Yu Hakusho - Makyou Toitsusen (Japan) (Beta) (June, 1994) (Sega Channel).md\n\
2ef59e9825d66d98443eff5cfb1f557689731309 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship II (USA).md\n\
9609f9934a80dba183dab603ae07f445f02b919d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Mortal Kombat 3 (Europe).md\n\
7f555d647972fee4e86b66e840848e91082f9c2d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Sega Sports 1 (Europe).md\n\
ff03bb2aced48c82de0ddfb048b114ec84daf16a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Mega Bomberman (USA).md\n\
89495a8acbcb0ddcadfe5b7bada50f4d9efd0ddd {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-04).md\n\
c07558f34b7480e063eb60126fc33fba36ae3daa {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Worms (Europe) (Beta).bin\n\
5a11867fdf41c9e065e6ba88668d6a48bcbc476c {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# J. League Pro Striker Perfect (Japan).md\n\
7cfd8c9119d0565ee9a7708dc46bb34dd3258e37 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Super Skidmarks (Europe) (Beta) (1995-09-22).md\n\
2d4312874a5b5aa0031daad21b602b0dec6e0810 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Pro Striker - Final Stage (Japan).md\n\
74e4a3ac4b93e25ace6ec8c3818e0df2390cffa2 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# WWF RAW (World).md\n\
0971dc7edbdd1d19e1da153394e3d16a6a67cfd0 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-29).md\n\
27af9bbaa449c38395afb8f29b2626056a4ae891 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour Championship Tennis (USA) (Beta) (1994-08-05).md\n\
9fae94e52de4bde42ac45f0e3d3964ccd094b375 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# General Chaos (USA, Europe).md\n\
aea1dfa67b0e583a3d367a67499948998cb92f56 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-07).md\n\
ddf2b988bc035a0736ed3a1d7c8e6aa8cd2625f8 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-02-01).md\n\
3dbc1a80005eb6783feeb4d3604d382d1cf688bc {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-05-25).md\n\
e2fb264a11e08d57acf2756688880cd6fc353aba {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Lost Vikings, The (Europe) (En,Fr,De).md\n\
c977a21d287187c3931202b3501063d71fcaf714 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Slam (USA).md\n\
0dbbe740b14077fe8648955f7e17965ea25f382a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour Championship Tennis (USA).md\n\
1ccd027cac63ee56b24a54a84706646d22d0b610 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# J. League Pro Striker (Japan) (Rev A).md\n\
1e437182fab2980156b101a53623c3c2f27c3a6c {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Micro Machines (Europe) (Alt 2).md\n\
56a8844c376f2e79e92cf128681fa3fef81c36d6 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-12-15).md\n\
c3f88b334af683e8ac98cafafa9abf4dfe65a4b7 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-11) (Alt 1).md\n\
91babf6f0d86e19e82c3f045d01e99ea9fe0253c {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-08).md\n\
3a60332ee684ff8accd96aef404346e66e267b6f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-27).md\n\
32104d3585dcbf190904accdb9528f0b7105eb4b {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA).md\n\
327249456f96ccb3e9758c0162c5f3e3f389072f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour (Europe).md\n\
c5fe0fe967369e9d9e855fd3c7826c8f583c49e3 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-12-02).md\n\
42c55adad0249bb09350d1ac7c9bfb737ed091c8 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-05).md\n\
f25cbabd3f6284387e66eaa72fa4124f3768121e {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-27) (Alt 1).md\n\
90c246dcb8ccea0f30ae5582b610721fc802f937 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour Championship Tennis (USA) (Beta) (1994-09-08).md\n\
c2e277d1cf4fa9def71014dc7cf6ebe34d521281 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Mortal Kombat 3 (USA).md\n\
55cdcba77f7fcd9994e748524d40c98089344160 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-12-29).md\n\
2604a03c1dc59538a82e32bcc6f8a995bd8af609 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-18).md\n\
58c88d26baffd0f68c2b5d95284323ba99db9b5a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-06-07).md\n\
334daa4f48dea4d85145fcd1bfb03f522532a9ae {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Jam (USA, Europe) (Rev 1).md\n\
55f2b26a932c69b2c7cb4f24f56b43f24f113a7c {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-27).md\n\
b3301897af0590ab7c8cc2b2028b40192012aa65 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-02).md\n\
83c70177164e418b988ed9144d9d12c0e7052c3b {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-09) (Alt 1).md\n\
7b2654e7828989cc776b2645d635271d951f671f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Olympic Gold (Japan, USA) (En,Fr,De,Es,It,Nl,Pt,Sv).md\n\
f9febd976d98545dee35c10b69755908d6929fd4 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Barkley Shut Up and Jam 2 (USA) (Beta).md\n\
a9f9fa95106d2e1d00800386ba3d56e1483c83db {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Puzzle &amp; Action - Ichidanto-R (Japan).md\n\
2e43ea1870dd3352e3c153373507554d97d51edf {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-05-06).md\n\
ee673500aef188ca7cf086fb1cf616b58896fdcb {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# General Chaos Daikonsen (Japan).md\n\
9f205fc916df523ec84e0defbd0f1400a495cf8a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-05).md\n\
7f453ca6499263054e2c649b508811b46d6edf4f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-10).md\n\
c55ff8f24d99c57c23ad1ef9fe43a7a142dcdc32 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-05-03).md\n\
d9db6ecb032fd88443d0575b01e61cb4aeea5703 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-12) (Alt 1).md\n\
7cfb0e06cc41d11ec51ca5f1959a5616010892ad {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-01).md\n\
ea1126f9cade3ec680dd029af10a5268cd5afa72 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Hang Time (USA).md\n\
bff36de7e0ca875b1fab84928f00999b48ff8f02 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-04-18).md\n\
ea19e141c64cc4abc6e7d7eea7bbb6783569a05a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Micro Machines 2 - Turbo Tournament (Europe) (Putative Beta).md\n\
cb5fb33212592809639b37c2babd72a7953fa102 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Gauntlet IV (USA, Europe) (Beta).md\n\
d28e22207121f0e2980dd409b4fb24f9fb8967ae {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Cup USA 94 (USA, Europe, Korea) (En,Fr,De,Es,It,Nl,Pt,Sv).md\n\
af0e8fada3db7e746aef2c0070deb19602c6d32a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Worms (USA) (Proto) (June, 1995).md\n\
23127e9b3a98eea13fb97bed2d8e206adb495d97 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-02-28).md\n\
f633f6ef930426a12c34958d3485c815a82a2276 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '94 (USA) (Beta) (1994-01-16).md\n\
da6fcc1d7069e315797dd40a89e21963ab766b9e {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-02-23).md\n\
ee4251c32961dc003a78bedbf42b231a31cc0acf {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-06-20).md\n\
4b30eea2fb1187cf3c9150f9dee5b5b9571c76f5 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Dragon - The Bruce Lee Story (USA).md\n\
01b45b9865282124253264c5f2e3d3338858ff92 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Tiny Toon Adventures - ACME All-Stars (Europe).md\n\
2672018d9e005a9a3b5006fa8f61e08f2d1909aa {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-06-08).md\n\
1b69304213ef1732c0b9b2f059179a7cf18b2c75 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-22).md\n\
d1a48818a982025b0833c06a584382122d1ccfb2 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Gauntlet IV (USA, Europe).md\n\
26c26ee2bb9571d51537d9328a5fd2a91b4e9dc1 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-12-24).md\n\
248e67cad67118a1449de308bac0437641bda3ec {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Jam (Japan).md\n\
2a88b2e1ecf115fa6246397d829448b755a5385e {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-05) (Alt 1).md\n\
787f8093d8a5c55ee9dcaa49925752169527fa62 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-12-30).md\n\
4ddbe2f458915db0da45fa490d653f2d94ec1263 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# International Rugby (Europe).md\n\
8cea50f668fbfba7dd10244a001172c1e648c352 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-11).md\n\
2e3e044bd6061002444bdc4bbe5de1f8c8417ce6 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Micro Machines (USA).md\n\
39b25dbfa7a80aebdefc19ac2df4bc75b934bbda {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-28) (Alt 1).md\n\
dd1256efa397a56d461e2bc7aec9f72aff9b04fb {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-04-13).md\n\
2eb0daad82caff6bcefb438297a2d701c99173c5 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL Sports Talk Football '93 Starring Joe Montana (USA, Europe).md\n\
3c97396cb4cc566013d06f055384105983582067 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-14).md\n\
9226eb23e1a91856300b310cb2b8263a832ba231 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-30).md\n\
ab90daf9791fa7347fb8f040e27f91b6bae46e1e {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Wimbledon Championship Tennis (USA).md\n\
db9083fd257d7cb718a010bdc525980f254e2c66 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '94 (USA) (Beta) (1994-01-04).md\n\
af63f96417a189f5061f81ba0354d4e38b0b7d76 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-05-20).md\n\
0b26c5748e976a64c02864e1934f2b50f6953cba {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Jam (USA, Europe).md\n\
99c5bc57fdea7f9df0cd8dec54160b162342344d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-11-23).md\n\
0eee2e7296e3eed5dbf85954ce14a53622ae3d64 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-06-15).md\n\
c9d010a0ffccecc2c01412daf64bf1b0eaf5055e {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-30).md\n\
ac31599d964a2b8ad69eebc47db2947c82768e98 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Hang Time (Europe).md\n\
4594ba338a07dd79639c11b5b96c7f1a6e283d0c {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Street Racer (Europe) (Beta) (1995-03-21).md\n\
674e3c5185923a98bd8e9ae9bc0409d0c6503432 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-12).md\n\
9f6a2ea386d383aee3be06d6b74fda67b1ebd960 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-06).md\n\
59c548514bee4be7a5e6e3483ac3122d410a7e4e {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-09-09).md\n\
2ba79a9a638894abbcd0702ba380b68ccaebb50f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ESPN Sunday Night NFL (USA).md\n\
03f8c8805ebd4313c8a7d76b34121339bad33f89 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-12).md\n\
8a7320033449cc9972b2db25752150fe4c2a1fab {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-31).md\n\
fd345d92799224475f0107751512adf584899c6b {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Worms (Europe).md\n\
1a15447a4a791c02b6ad0a609f788d39fe6c3aa6 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ESPN Sunday Night NFL (USA) (Beta).md\n\
8eeace9ef641d806afc1eaeafa244a076123b118 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Gauntlet (Japan) (En,Ja) (Beta).md\n\
347209b0d09270d0a492622e7961ba7febdef99b {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# ATP Tour Championship Tennis (USA) (Beta) (1994-05-09).md\n\
dbaa2f60df5811026539d1f4c6ad50b596b1356a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-03).md\n\
170aa426472cfeb3de1a6b98ed825f435b60b1a5 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-06-03).md\n\
1e155744a1c089cd2332c27cdad48e7f243c2fc8 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-03).md\n\
b50be710436a3cb1f7644fdfac5d5098cd9dbb2b {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# J. League Pro Striker 2 (Japan).md\n\
924f1ae3d90bec7326a5531cd1d598cdeba30d36 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-05-31).md\n\
157b65be9d946c46f77a90e4a5847fa41f2692b9 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Barkley Shut Up and Jam 2 (USA).md\n\
b13f13ccc1a21dacd295f30c66695bf97bbeff8d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-22).md\n\
f219274a65c31d76a2d6633b7e7cf65462850f47 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-01).md\n\
e7a8421a83195a71a4cf129e853d532b6114b1f6 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-17) (Alt 1).md\n\
bb949355b764a48015edc4fbbf0f89d4c7400c31 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Micro Machines (Europe) (Alt 1).md\n\
bbcf8a40e7bfe09225fdc8fc3f22f8b8cc710d06 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL Sports Talk Football '93 Starring Joe Montana (USA, Europe) (Rev A).md\n\
978350db75e2f31e286242cd33b9f8a6671ea4ab {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Jam - Tournament Edition (World).md\n\
ddbf09c5e6ed5d528ef5ec816129a332c685f103 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA).md\n\
f6ce0b826e028599942957729d72c7a8955c5e35 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-21).md\n\
591288688956cec3d0aca3dd099b3e0985ca947a {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-04).md\n\
ba98b890ca71447bbd7620526f3277e9e9de10fa {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Party Quiz Mega Q (Japan).md\n\
3f82b2f028345b6fc84801fa38d70475645d6aa2 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-05-11).md\n\
b16e953b695148b8284f87be566774379c4c2453 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Gauntlet (Japan) (En,Ja).md\n\
30208982dd1f50634943d894e7458a556127f8e4 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-04-29).md\n\
11e333a326ea71f77b816add76defc8f2846710d {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-05-17).md\n\
01ed2026a930383d926d71192b2a8f9417dfb245 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# College Football's National Championship (USA) (Beta) (1994-06-18).md\n\
164e3fc32aa295b0d87e1508dd5fe75f9a7cadb9 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (USA) (Beta) (1994-03-25).md\n\
4e64d9aa93107f9553e9406d8f0f1a998ea5aabd {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL Quarterback Club (World).md\n\
5c8b6eb2934cb97e35af36efd70c39dae899048f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe).md\n\
34e2df219e09c24c95c588a37d2a2c5e15814d68 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Micro Machines (Europe).md\n\
6c822ab42a37657aa2a4e70e44b201f8e8365a98 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Mega Bomberman - Special 8-Player-Demo (Europe) (Proto).md\n\
10ac1cb23245b53357e7253f5db49395492b89f7 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1994-12-31).md\n\
ba08a3f042f96b4b3bb889bbacb6a5e13b114f0c {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# From TV Animation Slam Dunk - Kyougou Makkou Taiketsu! (Japan).md\n\
7161a12c2a477cff8e29fa51403eea12c03180c7 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# Ultimate Mortal Kombat 3 (USA).md\n\
bf2da4a7ae7aa428b0b316581f65b280dc3ba356 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL '95 (USA) (Beta) (1994-08-30).md\n\
c42a2170449e4e26beaa59cc4909e505be751aa9 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# World Championship Soccer II (Europe).md\n\
5f5a31a3e8e3b56aa0a3acdb8049246160679a8f {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NFL Football '94 (Japan) (1993-11-04) (Sega Channel).md\n\
ed1f1e1568819df43c110314da37fcf1b0f41b78 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Action '95 Starring David Robinson (USA, Europe) (Beta) (1995-01-28).md\n\
f1affb6e01ca23b67aee9e1c1767f9ec13849823 {\n\
	device_overrides {\n\
		2 sega_multi666X.2\n\
	}\n\
}\n\
# NBA Showdown '94 (USA, Europe).md\n\
3134a3cb63115d2e16e63a76c2708cdaecab83e4 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# PGA Tour 96 (USA, Europe).md\n\
abc2a8d773724cd8fb1aeae483f5ca72f47e77fa {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# PGA European Tour (USA, Europe).md\n\
640615be6891a8457d94bb81b0e8e1fa7c5119a8 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA 2003 (Unknown) (Unl) (Pirate).md\n\
8dc04a0f137323ec95d6c7dfa9cb03fa572bdc51 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# IMG International Tour Tennis (USA, Europe).md\n\
1f1b410d17b39851785dee3eee332fff489db395 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# College Football USA 96 (USA).md\n\
3079bdc5f2d29dcf3798f899a3098736cdc2cd88 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# PGA Tour Golf II (Japan).md\n\
a5896f2f019530929194a6d80828d18b859b9174 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NBA Live 97 (USA, Europe).md\n\
1671451ab4ab6991e13db70671054c0f2c652a95 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# John Madden Football '93 (USA, Europe).md\n\
59d2352ecb31bc1305128a9d8df894a3bfd684cf {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NHL 98 (USA).md\n\
6771e9b660bde010cf28656cafb70f69249a3591 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA Soccer 95 (USA, Europe) (En,Fr,De,Es).md\n\
586f9d0f218cf6bb3388a8610b44b6ebb9538fb5 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NHL 97 (USA, Europe).md\n\
8a90d7921ab8380c0abb0b5515a6b9f96ca6023c {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NBA Live 95 (USA, Europe).md\n\
f86bc9601751ac94119ab2f3ecce2029d5678f01 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Australian Rugby League (Europe).md\n\
b54754180f22d52fc56bab3aeb7a1edd64c13fef {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# John Madden Football - Pro Football (Japan).md\n\
c4cf3681d86861a823fa3e7ffe0cd451fbafcee6 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA Soccer 2000 (Russia) (En,Fr,De,Es,It,Sv) (Unl) (Pirate).md\n\
ed29312dbd9574514c06e5701e24c6474ed84898 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Madden NFL 95 (USA, Europe).md\n\
41cde6211da87a8e61e2ffd42cef5de588f9b9fc {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Madden NFL 97 (USA, Europe).md\n\
63544d2a0230be102f2558c03a74855fc712b865 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NBA Live 96 (USA, Europe).md\n\
5fca106c839d3dea11cbf6842d1d7650db06ca72 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Triple Play 96 (USA).md\n\
bf967bba7ce2cdf1c8b536cf7e681795b51b08a2 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NBA Live 98 (USA).md\n\
89b98867c7393371a364de58ba6955e0798fa10f {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# John Madden Football '92 (USA, Europe).md\n\
1a4c1dcc2de5018142a770f753ff42667b83e5be {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# College Football USA 96 (USA) (Beta) (1995-06-21).md\n\
5438ba1baa3400c73905e25fdfe9d504c3604d5a {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Bill Walsh College Football (USA, Europe).md\n\
2bbb454900ac99172a2d72d1e6f96a96b8d6840b {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Rugby World Cup 95 (USA, Europe) (En,Fr,It).md\n\
9b435c82b612e23cb512efaebf4d35b203339e44 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NBA Live 95 (Korea).md\n\
115c2e8cfa6bc45767ba47efc00aede424a6de66 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA Soccer 96 (USA, Europe) (En,Fr,De,Es,It,Sv).md\n\
a7fcfe478b368d7d33bcbca65245f5faed9a1e07 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Triple Play - Gold Edition (USA).md\n\
791afd91483a332ec5ca3384ee791252767f2f67 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# PGA Tour Golf (USA, Europe) (PGA07).md\n\
73935bfbdf63d3400284a16e464286b7630964aa {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# John Madden Football (USA, Europe) (1990-11-07) (Sega Channel).md\n\
4a8fca0212497d446f6a5f829dc5748ed1456b87 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Madden NFL '94 (USA, Europe).md\n\
856d68d3e8589df3452096434feef823684d11eb {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# PGA Tour Golf (USA, Europe) (PGA09).md\n\
1b173a1b674a6d5bdcd539c4fe8fe984586c3a8a {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA 98 - Road to World Cup (Europe) (En,Fr,Es,It,Sv).md\n\
6613f13da5494aaaba3222ed5e730ec9ce3c09a7 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Elitserien 96 (Sweden).md\n\
085fb8e6f0d2ff0f399de5c57eb13d9c9325dbae {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA International Soccer (USA, Europe) (En,Fr,De,Es).md\n\
1cbef8c4541311b84d7388365d12a93a1f712dc4 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# John Madden Football - Championship Edition (USA).md\n\
417acacb9f9ef90d08b3cfb81972a9d8b56f4293 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NHL 96 (USA, Europe).md\n\
7204633fbb9966ac637e7966d02ba15c5acdee6b {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Elitserien 95 (Sweden).md\n\
5f2c8303099ce13fe1e5760b7ef598a2967bfa8d {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# College Football USA 97 (USA).md\n\
9b93035ecdc2b6f0815281764ef647f2de039e7b {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA Soccer 95 (Korea) (En,Fr,De,Es).md\n\
ad1202a2e4166f8266d5633b8c5beb59c6cbd005 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# PGA Tour Golf III (USA, Europe).md\n\
702707efcbfe229f6e190f2b6c71b6f53ae9ec36 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# John Madden Football (USA, Europe).md\n\
10682f1763711b281542fcd5e192e1633809dc75 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# PGA Tour Golf II (USA, Europe) (Rev A).md\n\
12d5236a4ff23c5b1e4f452b3abd3d48e6e55314 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Bill Walsh College Football 95 (USA).md\n\
2ae000f45474b3cdedd08eeca7f5e195959ba689 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Madden NFL 96 (USA, Europe).md\n\
35a4241eed51f10de2e63c843f162ce5d92c70a2 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# NHL 95 (USA, Europe).md\n\
09e87b076aa4cd6f057a1d65bb50fd889b509b44 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Coach K College Basketball (USA).md\n\
8ff5d7a7fcc47f030a3ea69f4534d9c892f58ce2 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Madden NFL 98 (USA).md\n\
761e0903798a8d0ad9e7ab72e6d2762fc9d366d2 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA Soccer 97 (USA, Europe) (En,Fr,De,Es,It,Sv).md\n\
2d91cb1c50586723f877cb25a37b5ebcd70d8bcc {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA International Soccer (Japan) (En,Ja).md\n\
3464e97d14f4ddae6b15e1246d25e68d063861fc {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# FIFA Soccer 99 (Unknown) (Unl) (Pirate).md\n\
2c8c1dc0aaa711e3ab3fe0d74b79184f33127350 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# PGA Tour Golf II (USA, Europe).md\n\
cab753b958b336dab731407bd46429de16c6919f {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
# Australian Rugby League (Europe) (Beta).md\n\
bb84be50eb4d31dbe601058bf9ea1adaaec1a1b1 {\n\
	device_overrides {\n\
		1 ea_multi_a3\n\
		2 ea_multi_b\n\
	}\n\
}\n\
";

char *read_bundled_file(char *name, uint32_t *sizeret)
{
	if (!strcmp(name, "rom.db")) {
		*sizeret = strlen(rom_db_data);
		char *ret = malloc(*sizeret+1);
		memcpy(ret, rom_db_data, *sizeret + 1);
		return ret;
	}
	return NULL;
}

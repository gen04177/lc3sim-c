#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include "font8x8.h"

#include "libretro.h"
#include "../vm.h"

#define VIDEO_WIDTH  256
#define VIDEO_HEIGHT 256
#define BYTES_PER_PIXEL 4

#define FB_COLS 40
#define FB_ROWS 30
#define CHAR_W  8
#define CHAR_H  8

static char textbuf[FB_ROWS][FB_COLS];
static int cursor_x = 0;
static int cursor_y = 0;

static uint32_t *framebuffer;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;

static void lc3_putchar(uint16_t v)
{
    unsigned char c = v & 0xFF;

    if (c == '\r') {
        cursor_x = 0;
        return;
    }

    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    }
    else if (c >= 32 && c < 127) {
        textbuf[cursor_y][cursor_x++] = c;

        if (cursor_x >= FB_COLS) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (cursor_y >= FB_ROWS) {
        memmove(textbuf, textbuf[1], (FB_ROWS - 1) * FB_COLS);
        memset(textbuf[FB_ROWS - 1], 0, FB_COLS);
        cursor_y = FB_ROWS - 1;
    }
}

static char get_key_from_keyboard(void)
{
    for (int i = RETROK_a; i <= RETROK_z; i++)
        if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, i))
            return 'a' + (i - RETROK_a);

    if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_RETURN))
        return '\n';

    if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, RETROK_BACKSPACE))
        return '\b';

    return 0;
}

static char get_key_from_gamepad(void)
{
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
        return 'a';
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
        return 'b';
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))
        return 'x';
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
        return 'y';

    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
        return 'u';
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
        return 'd';
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
        return 'l';
    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
        return 'r';

    if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))
        return '\n';

    return 0;
}

uint16_t lc3_getchar(void)
{
    static bool waiting_for_release = false;

    input_poll_cb();

    for (unsigned i = 0; i < 256; ++i) {
        if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, i)) {
            if (!waiting_for_release) {
                waiting_for_release = true;
                return i;
            }
            else {
                return 0xFFFF;
            }
        }
    }

    struct { unsigned id; char c; } buttons[] = {
        { RETRO_DEVICE_ID_JOYPAD_A, 'a' },
        { RETRO_DEVICE_ID_JOYPAD_B, 'b' },
        { RETRO_DEVICE_ID_JOYPAD_X, 'x' },
        { RETRO_DEVICE_ID_JOYPAD_Y, 'y' },
        { RETRO_DEVICE_ID_JOYPAD_UP, 'u' },
        { RETRO_DEVICE_ID_JOYPAD_DOWN, 'd' },
        { RETRO_DEVICE_ID_JOYPAD_LEFT, 'l' },
        { RETRO_DEVICE_ID_JOYPAD_RIGHT, 'r' },
        { RETRO_DEVICE_ID_JOYPAD_START, '\n' }
    };

    for (unsigned i = 0; i < sizeof(buttons)/sizeof(buttons[0]); ++i) {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, buttons[i].id)) {
            if (!waiting_for_release) {
                waiting_for_release = true;
                return buttons[i].c;
            }
            else {
                return 0xFFFF;
            }
        }
    }

    waiting_for_release = false;
    return 0xFFFF;
}


static vm_ctx vm = NULL;
static bool vm_halted = false;

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static void fallback_log(enum retro_log_level level,
                         const char *fmt, ...)
{
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

void retro_init(void)
{
    framebuffer = calloc(VIDEO_WIDTH * VIDEO_HEIGHT,
                          sizeof(uint32_t));
}

void retro_deinit(void)
{
    free(framebuffer);
    framebuffer = NULL;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "lc3sim";
    info->library_version  = "0.1";
    info->need_fullpath    = true;
    info->valid_extensions = "obj";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));

    info->geometry.base_width   = VIDEO_WIDTH;
    info->geometry.base_height  = VIDEO_HEIGHT;
    info->geometry.max_width    = VIDEO_WIDTH;
    info->geometry.max_height   = VIDEO_HEIGHT;
    info->geometry.aspect_ratio = 1.0f;

    info->timing.fps            = 60.0;
    info->timing.sample_rate    = 44100.0;
}

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    struct retro_log_callback log;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;
    else
        log_cb = fallback_log;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

bool retro_load_game(const struct retro_game_info *info)
{
    if (!info || !info->path)
        return false;

    log_cb(RETRO_LOG_INFO, "Loading %s\n", info->path);

    memset(textbuf, 0, sizeof(textbuf));
    cursor_x = cursor_y = 0;

    vm = vm_create();
    vm_load_os(vm);

    vm_putchar_cb = lc3_putchar;
	vm_getchar_cb = lc3_getchar;

    vm_load_result r = vm_load_file(vm, info->path);
    if (r != VM_LOAD_SUCCESS)
    {
        log_cb(RETRO_LOG_ERROR, "Failed to load .obj\n");
        vm_destroy(vm);
        vm = NULL;
        vm_putchar_cb = NULL;
        return false;
    }

    vm_halted = false;

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        log_cb(RETRO_LOG_ERROR, "Pixel format not supported\n");
        return false;
    }

    return true;
}

void retro_unload_game(void)
{
    if (vm)
    {
        vm_destroy(vm);
        vm = NULL;
    }
}

void retro_reset(void)
{
    if (!vm)
        return;

    vm_destroy(vm);
    vm = vm_create();
    vm_load_os(vm);
    vm_halted = false;
}

void retro_run(void)
{
    memset(framebuffer, 0,
           VIDEO_WIDTH * VIDEO_HEIGHT * sizeof(uint32_t));

    if (!vm || vm_halted)
    {
        video_cb(framebuffer,
                 VIDEO_WIDTH,
                 VIDEO_HEIGHT,
                 VIDEO_WIDTH * BYTES_PER_PIXEL);
        return;
    }

    input_poll_cb();

    for (int i = 0; i < 1000; i++)
    {
        vm_run_result r = vm_step(vm);
        if (r != VM_RUN_SUCCESS)
        {
            vm_halted = true;
            log_cb(RETRO_LOG_INFO, "LC3SIM: HALT\n");
            break;
        }
    }

    for (int y = 0; y < FB_ROWS; y++) {
    for (int x = 0; x < FB_COLS; x++) {
        unsigned char c = textbuf[y][x];
        if (!c) continue;

        const uint8_t *glyph = font8x8[c];
        for (int gy = 0; gy < CHAR_H; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < CHAR_W; gx++) {
		if (row & (1 << gx)) {
    int px = x * CHAR_W + gx;
    int py = y * CHAR_H + gy;
    framebuffer[py * VIDEO_WIDTH + px] = 0xFFFFFFFF;
}
            }
        }
    }
}

    video_cb(framebuffer,
             VIDEO_WIDTH,
             VIDEO_HEIGHT,
             VIDEO_WIDTH * BYTES_PER_PIXEL);
}


unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *d, size_t s) { (void)d; (void)s; return false; }
bool retro_unserialize(const void *d, size_t s) { (void)d; (void)s; return false; }

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }

size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned i, bool e, const char *c)
{
    (void)i; (void)e; (void)c;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   return false;
}


/**
 * MojoZork; a simple, just-for-fun implementation of Infocom's Z-Machine.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#include <assert.h>

#include "libretro.h"

#define MOJOZORK_LIBRETRO 1
#include "mojozork.c"
#include "mojozork-libretro-font.h"

// Touch input should work on any platform, but on several desktop platforms
//  this RETRO_DEVICE_POINTER just gives you mouse input atm, so rather
//  than risk the confusion, we turn off touch support if you aren't on
//  Android for now.
#if defined(__ANDROID__)
#define IGNORE_TOUCH_INPUT 0
#else
#define IGNORE_TOUCH_INPUT 1
#endif

#define TEST_TOUCH_WITH_MOUSE 0
#if TEST_TOUCH_WITH_MOUSE
static int16_t scale_mouse_to_touch_coords(const int16_t m, const int16_t maxsize)
{
    float normalized = ((((float)m) / ((float)maxsize)) * 2.0f) - 1.0f;  // make it -1.0f to 1.0f
    if (normalized < -1.0f) { normalized = -1.0f; } else if (normalized > 1.0f) { normalized = 1.0f; }  // just in case.
    return (int16_t) (normalized * 0x7fff);
}
#endif


// !!! FIXME: almost _all_ of our serialization state bulk is the
// !!! FIXME:  scrollback buffer (at about 71 bytes per line, 5000 lines of
// !!! FIXME:  scrollback is ~355 kilobytes). A more efficient serialization
// !!! FIXME:  here could go a long way. Maybe compress it, since it'll
// !!! FIXME:  crunch down to almost nothing, and have a set buffer size that
// !!! FIXME:  will likely hold it all that's much smaller than the
// !!! FIXME:  uncompressed data?
// !!! FIXME:
// !!! FIXME: ...but perhaps this bulk isn't a big deal when this isn't
// !!! FIXME:  exactly a thing that benefits from frame-by-frame snapshots.

#define FRAMEBUFFER_WIDTH 640
#define FRAMEBUFFER_HEIGHT 480
#define VIRTUAL_KEYBOARD_HEIGHT 105
#define FRAMEBUFFER_PIXELS (FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)
#define TERMINAL_CHAR_WIDTH (LIBRETROFONT_WIDTH / 256)
#define TERMINAL_CHAR_HEIGHT LIBRETROFONT_HEIGHT
#define TERMINAL_WIDTH (FRAMEBUFFER_WIDTH / TERMINAL_CHAR_WIDTH)
#define TERMINAL_HEIGHT (FRAMEBUFFER_HEIGHT / TERMINAL_CHAR_HEIGHT)
#define VIDEO_WIDTH (TERMINAL_WIDTH * TERMINAL_CHAR_WIDTH)
#define VIDEO_HEIGHT (TERMINAL_HEIGHT * TERMINAL_CHAR_HEIGHT)
#define VIDEO_X_OFFSET ((FRAMEBUFFER_WIDTH - VIDEO_WIDTH) / 2)
#define VIDEO_Y_OFFSET ((FRAMEBUFFER_HEIGHT - VIDEO_HEIGHT) / 2)
#define SCROLLBACK_LINES 5000

static const uint16_t glyph_color[2] = { 0x0000, 0xFFFF };

// these are indexes into glyph_color; values outside the array are transparent (not drawn).
#define MOUSE_CURSOR_WIDTH 10
#define MOUSE_CURSOR_HEIGHT 16
static const uint8_t mouse_cursor[] = {
    9, 1, 9, 9, 9, 9, 9, 9, 9, 9,
    1, 0, 1, 9, 9, 9, 9, 9, 9, 9,
    1, 0, 0, 1, 9, 9, 9, 9, 9, 9,
    1, 0, 0, 0, 1, 9, 9, 9, 9, 9,
    1, 0, 0, 0, 0, 1, 9, 9, 9, 9,
    1, 0, 0, 0, 0, 0, 1, 9, 9, 9,
    1, 0, 0, 0, 0, 0, 0, 1, 9, 9,
    1, 0, 0, 0, 0, 0, 0, 0, 1, 9,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 1,
    1, 0, 0, 0, 0, 0, 1, 1, 1, 9,
    1, 0, 0, 1, 0, 0, 1, 9, 9, 9,
    1, 0, 1, 9, 1, 0, 0, 1, 9, 9,
    9, 1, 9, 9, 1, 0, 0, 1, 9, 9,
    9, 9, 9, 9, 9, 1, 0, 0, 1, 9,
    9, 9, 9, 9, 9, 1, 0, 0, 1, 9,
    9, 9, 9, 9, 9, 9, 1, 1, 9, 9,
};

typedef struct
{
    const char *sym;
    enum retro_key key;
    int x;
    int y;
    int w;
    int h;
} VirtualKeyboardKey;

static uint16_t frame_buffer[FRAMEBUFFER_PIXELS];
static struct retro_log_callback logging;
static retro_log_printf_t log_cb;
static float last_aspect;
static float last_sample_rate;
static char scrollback[SCROLLBACK_LINES][TERMINAL_WIDTH];
static int32_t scrollback_read_pos = SCROLLBACK_LINES - TERMINAL_HEIGHT;
static int32_t scrollback_count = TERMINAL_HEIGHT;
static int32_t cursor_position = TERMINAL_WIDTH * (TERMINAL_HEIGHT-1);
static int32_t terminal_word_start = -1;
static int32_t virtual_keyboard_height = 0;
static bool virtual_keyboard_enabled = false;
static uint16_t virtual_keyboard_image[VIRTUAL_KEYBOARD_HEIGHT * FRAMEBUFFER_WIDTH];
static const VirtualKeyboardKey *virtual_keyboard_key_highlighted = NULL;
static bool virtual_keyboard_key_pressed = false;
static char status_bar[TERMINAL_WIDTH + 1];
static char upper_window[TERMINAL_HEIGHT * TERMINAL_WIDTH];
static int32_t upper_window_cursor_position = 0;
static bool frontend_supports_frame_dupe = false;

static const VirtualKeyboardKey virtual_keyboard_keys[5][11] = {
    {
        { "1", RETROK_1, 136, 2, 23, 17 },
        { "2", RETROK_2, 168, 2, 23, 17 },
        { "3", RETROK_3, 200, 2, 23, 17 },
        { "4", RETROK_4, 232, 2, 23, 17 },
        { "5", RETROK_5, 264, 2, 23, 17 },
        { "6", RETROK_6, 296, 2, 23, 17 },
        { "7", RETROK_7, 328, 2, 23, 17 },
        { "8", RETROK_8, 360, 2, 23, 17 },
        { "9", RETROK_9, 392, 2, 23, 17 },
        { "0", RETROK_0, 424, 2, 23, 17 },
        { "del", RETROK_BACKSPACE, 459, 2, 55, 17 }
    },
    {
        { "q", RETROK_q, 136, 23, 23, 17 },
        { "w", RETROK_w, 168, 23, 23, 17 },
        { "e", RETROK_e, 200, 23, 23, 17 },
        { "r", RETROK_r, 232, 23, 23, 17 },
        { "t", RETROK_t, 264, 23, 23, 17 },
        { "y", RETROK_y, 296, 23, 23, 17 },
        { "u", RETROK_u, 328, 23, 23, 17 },
        { "i", RETROK_i, 360, 23, 23, 17 },
        { "o", RETROK_o, 392, 23, 23, 17 },
        { "p", RETROK_p, 424, 23, 23, 17 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 }
    },
    {
        { "a", RETROK_a, 136, 44, 23, 17 },
        { "s", RETROK_s, 168, 44, 23, 17 },
        { "d", RETROK_d, 200, 44, 23, 17 },
        { "f", RETROK_f, 232, 44, 23, 17 },
        { "g", RETROK_g, 264, 44, 23, 17 },
        { "h", RETROK_h, 296, 44, 23, 17 },
        { "j", RETROK_j, 328, 44, 23, 17 },
        { "k", RETROK_k, 360, 44, 23, 17 },
        { "l", RETROK_l, 392, 44, 23, 17 },
        { ";", RETROK_SEMICOLON, 424, 44, 23, 17 },
        { "enter", RETROK_RETURN, 459, 44, 55, 17 }
    },
    {
        { "z", RETROK_z, 136, 65, 23, 17 },
        { "x", RETROK_x, 168, 65, 23, 17 },
        { "c", RETROK_c, 200, 65, 23, 17 },
        { "v", RETROK_v, 232, 65, 23, 17 },
        { "b", RETROK_b, 264, 65, 23, 17 },
        { "n", RETROK_n, 296, 65, 23, 17 },
        { "m", RETROK_m, 328, 65, 23, 17 },
        { ",", RETROK_COMMA, 360, 65, 23, 17 },
        { ".", RETROK_PERIOD, 392, 65, 23, 17 },
        { "?", RETROK_QUESTION, 424, 65, 23, 17 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 }
    },
    {
        { "space", RETROK_SPACE, 200, 86, 183, 17 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 },
        { NULL, RETROK_UNKNOWN, 0, 0, 0, 0 }
    }
};

typedef enum
{
    CURRENTINPUTDEV_KEYBOARD,
    CURRENTINPUTDEV_MOUSE,
    CURRENTINPUTDEV_CONTROLLER,
    CURRENTINPUTDEV_TOUCH
} CurrentInputDevice;

static CurrentInputDevice current_input_device = CURRENTINPUTDEV_KEYBOARD;
static int32_t mouse_x = -1;
static int32_t mouse_y = -1;
static int32_t touch_scrolling = -1;
static int32_t previous_touch_x = 0;
static int32_t previous_touch_y = 0;
static bool mouse_button_down = false;
static bool touch_pressed = false;

typedef struct
{
    bool up;
    bool down;
    bool left;
    bool right;
    bool a;
    bool b;
    bool x;
    bool y;
    bool select;
    bool start;
    bool l1;
    bool l2;
    bool l3;
    bool r1;
    bool r2;
    bool r3;
} ControllerState;

// controller_x and _y are positions _on the virtual keyboard_, so when you go
//  down to the spacebar and then back up, you end up back on the key you started on.
//  These are not coordinates in screen space!
static int32_t controller_x = 0;
static int32_t controller_y = 0;
static int last_controller_direction = 0;
static ControllerState current_controller_state;


static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}


static retro_environment_t environ_cb;

static void init_virtual_keyboard_image(void)
{
    for (int i = 0; i < (sizeof (virtual_keyboard_image) / sizeof (virtual_keyboard_image[0])); i++) {
        virtual_keyboard_image[i] = 0x001F;
    }

    for (int keyy = 0; keyy < (sizeof (virtual_keyboard_keys) / sizeof (virtual_keyboard_keys[0])); keyy++) {
        for (int keyx = 0; keyx < (sizeof (virtual_keyboard_keys[0]) / sizeof (virtual_keyboard_keys[0][0])); keyx++) {
            const VirtualKeyboardKey *k = &virtual_keyboard_keys[keyy][keyx];
            if (k->sym == NULL) {
                continue;
            }
            const char *sym = k->sym;
            const size_t slen = strlen(sym);
            uint16_t *dst = virtual_keyboard_image + ((FRAMEBUFFER_WIDTH * k->y) + k->x);
            const int h = k->h;
            const int w = k->w;
            for (int y = 0; y < h; y++) {
                memset(dst, '\0', w * sizeof (uint16_t));
                dst += FRAMEBUFFER_WIDTH;
            }

            dst = (virtual_keyboard_image + ((FRAMEBUFFER_WIDTH * k->y) + k->x));
            dst += ((h - TERMINAL_CHAR_HEIGHT) / 2) * FRAMEBUFFER_WIDTH;
            dst += (w - (slen * TERMINAL_CHAR_WIDTH)) / 2;

            for (int fy = 0; fy < TERMINAL_CHAR_HEIGHT; fy++) {
                uint16_t *next_dst = dst + FRAMEBUFFER_WIDTH;
                for (int x = 0; x < slen; x++) {
                    const uint32_t ch = (uint32_t) (unsigned char) sym[x];
                    const uint8_t *glyph = &libretrofont_data[(ch * TERMINAL_CHAR_WIDTH) + (LIBRETROFONT_WIDTH * fy)];
                    for (int fx = 0; fx < TERMINAL_CHAR_WIDTH; fx++) {
                        *(dst++) = glyph_color[glyph[fx]];
                    }
                }
                dst = next_dst;
            }
        }
    }
}

void retro_init(void)
{
    init_virtual_keyboard_image();
}

void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    //log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
}

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof (*info));
    info->library_name     = "mojozork";
    info->library_version  = "0.2";
    info->need_fullpath    = false;
    info->valid_extensions = "dat|z1|z3";
 }

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    const float aspect                = ((float) FRAMEBUFFER_WIDTH) / ((float) FRAMEBUFFER_HEIGHT);
    const float sampling_rate         = 30000.0f;

    info->geometry.base_width   = FRAMEBUFFER_WIDTH;
    info->geometry.base_height  = FRAMEBUFFER_HEIGHT;
    info->geometry.max_width    = FRAMEBUFFER_WIDTH;
    info->geometry.max_height   = FRAMEBUFFER_HEIGHT;
    info->geometry.aspect_ratio = aspect;

    last_aspect                 = aspect;
    last_sample_rate            = sampling_rate;
}


static void RETRO_CALLCONV frame_time_callback(retro_usec_t usec);
static void RETRO_CALLCONV keyboard_callback(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers);

void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    else
        log_cb = fallback_log;

    static const struct retro_controller_description controller = { "Generic game controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) };
    static const struct retro_controller_info ports[] = {
        { &controller, 1 },
        { NULL, 0 },
    };

    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *) ports);

    struct retro_frame_time_callback ftcb;
    ftcb.callback = frame_time_callback;
    ftcb.reference = 1000000 / 15;  // !!! FIXME: 15?
    cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, (void *) &ftcb);

    struct retro_keyboard_callback kbcb;
    kbcb.callback = keyboard_callback;
    cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, (void *) &kbcb);

    frontend_supports_frame_dupe = false;
    cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &frontend_supports_frame_dupe);
}

void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }

static void check_variables(void) {}

static void writestr(const char *str);

static jmp_buf jmpbuf;

static void step_zmachine(void)
{
    if (setjmp(jmpbuf) == 0) {  // if non-zero, ZMachine called GState->die() during runInstruction.
        const int initial_quit_state = GState->quit;
        GState->step_completed = initial_quit_state;
        while (!GState->step_completed) {
            runInstruction();
        }

        if (GState->quit && (initial_quit_state == 0)) {
            writestr("\n\n*** GAME HAS ENDED ***\n");
        }
    }
}

static void enable_virtual_keyboard(const bool enable)
{
    if (virtual_keyboard_enabled != enable) {
        virtual_keyboard_enabled = enable;
        virtual_keyboard_key_highlighted = NULL;
        virtual_keyboard_key_pressed = false;
    }
}

static void update_frame_buffer(void)
{
    // we draw from the bottom to the top, so if we have a partial row of characters, we can
    //  just stop drawing once we hit the top.
    const char *terminal_buffer = scrollback[scrollback_read_pos];
    const char *term_src = terminal_buffer + (TERMINAL_WIDTH * (TERMINAL_HEIGHT - 1));
    uint16_t *orig_dst = frame_buffer + ((VIDEO_Y_OFFSET * FRAMEBUFFER_WIDTH) + VIDEO_X_OFFSET);
    uint16_t *end_dst = orig_dst + (FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    uint16_t *dst = end_dst - FRAMEBUFFER_WIDTH;

    memset(frame_buffer, '\0', sizeof (frame_buffer));

    if (GState->status_bar_enabled) {
        orig_dst += FRAMEBUFFER_WIDTH * TERMINAL_CHAR_HEIGHT;
    }

    orig_dst += FRAMEBUFFER_WIDTH * TERMINAL_CHAR_HEIGHT * GState->upper_window_line_count;

    if (virtual_keyboard_height > 0) {
        dst -= FRAMEBUFFER_WIDTH * (virtual_keyboard_height - 1);  // minus one because dst was already set to write at the start of the last row.
        memcpy(dst, virtual_keyboard_image, FRAMEBUFFER_WIDTH * virtual_keyboard_height * sizeof (uint16_t));
    }

    // !!! FIXME: move this to a subroutine
    for (int y = TERMINAL_HEIGHT - 1; y >= 1; y--) {
        for (int fy = TERMINAL_CHAR_HEIGHT - 1; (fy >= 0) && (dst > orig_dst); fy--) {
            uint16_t *next_dst = dst - FRAMEBUFFER_WIDTH;  // draw rows bottom to top.
            for (int x = 0; x < TERMINAL_WIDTH; x++) {
                const uint32_t ch = (uint32_t) (unsigned char) term_src[x];
                const uint8_t *glyph = &libretrofont_data[(ch * TERMINAL_CHAR_WIDTH) + (LIBRETROFONT_WIDTH * fy)];
                for (int fx = 0; fx < TERMINAL_CHAR_WIDTH; fx++) {
                    *(dst++) = glyph_color[glyph[fx]];
                }
            }
            dst = next_dst;
        }

        term_src -= TERMINAL_WIDTH;  // draw rows bottom to top.
    }

    // !!! FIXME: move this to a subroutine
    if (GState->upper_window_line_count) {
        const char *term_src = upper_window + (TERMINAL_WIDTH * (GState->upper_window_line_count - 1));
        orig_dst = frame_buffer + ((VIDEO_Y_OFFSET * FRAMEBUFFER_WIDTH) + VIDEO_X_OFFSET);
        if (GState->status_bar_enabled) {
            orig_dst += FRAMEBUFFER_WIDTH * TERMINAL_CHAR_HEIGHT;
        }
        for (int y = TERMINAL_HEIGHT - 1; y >= 1; y--) {
            for (int fy = TERMINAL_CHAR_HEIGHT - 1; (fy >= 0) && (dst > orig_dst); fy--) {
                uint16_t *next_dst = dst - FRAMEBUFFER_WIDTH;  // draw rows bottom to top.
                for (int x = 0; x < TERMINAL_WIDTH; x++) {
                    const uint32_t ch = (uint32_t) (unsigned char) term_src[x];
                    const uint8_t *glyph = &libretrofont_data[(ch * TERMINAL_CHAR_WIDTH) + (LIBRETROFONT_WIDTH * fy)];
                    for (int fx = 0; fx < TERMINAL_CHAR_WIDTH; fx++) {
                        *(dst++) = glyph_color[glyph[fx]];
                    }
                }
                dst = next_dst;
            }

            term_src -= TERMINAL_WIDTH;  // draw rows bottom to top.
        }
    }

    // draw the status bar.
    if (GState->status_bar_enabled) {
        dst = orig_dst;
        orig_dst -= FRAMEBUFFER_WIDTH * TERMINAL_CHAR_HEIGHT;
        for (int fy = TERMINAL_CHAR_HEIGHT - 1; (fy >= 0) && (dst > orig_dst); fy--) {
            uint16_t *next_dst = dst - FRAMEBUFFER_WIDTH;  // draw rows bottom to top.
            for (int x = 0; x < TERMINAL_WIDTH; x++) {
                const uint32_t ch = (uint32_t) (unsigned char) GState->status_bar[x];
                const uint8_t *glyph = &libretrofont_data[(ch * TERMINAL_CHAR_WIDTH) + (LIBRETROFONT_WIDTH * fy)];
                for (int fx = 0; fx < TERMINAL_CHAR_WIDTH; fx++) {
                    *(dst++) = glyph_color[glyph[fx] ? 0 : 1];
                }
            }
            dst = next_dst;
        }
    }

    // fully displayed? add highlighting.
    if (virtual_keyboard_height == VIRTUAL_KEYBOARD_HEIGHT) {
        if (virtual_keyboard_key_highlighted) {
            const int w = virtual_keyboard_key_highlighted->w;
            const int h = virtual_keyboard_key_highlighted->h;
            const uint16_t color = virtual_keyboard_key_pressed ? 0x07E0 : 0xFFFF;

            uint16_t *orig_ptr = (end_dst - (FRAMEBUFFER_WIDTH * (virtual_keyboard_height))) + ((FRAMEBUFFER_WIDTH * virtual_keyboard_key_highlighted->y) + virtual_keyboard_key_highlighted->x);
            uint16_t *ptr;
            uint16_t *ptr2;

            ptr = orig_ptr;
            ptr2 = ptr + (FRAMEBUFFER_WIDTH * (h - 1));
            for (int i = 0; i < w; i++) {
                *(ptr++) = color;
                *(ptr2++) = color;
            }

            ptr = orig_ptr;
            ptr2 = ptr + (w - 1);
            for (int i = 0; i < h; i++) {
                *ptr = color;
                *ptr2 = color;
                ptr += FRAMEBUFFER_WIDTH;
                ptr2 += FRAMEBUFFER_WIDTH;
            }
        }
    }

    if ((current_input_device == CURRENTINPUTDEV_MOUSE) || TEST_TOUCH_WITH_MOUSE) {
        int maxy = FRAMEBUFFER_HEIGHT - mouse_y;
        if (maxy > MOUSE_CURSOR_HEIGHT) { maxy = MOUSE_CURSOR_HEIGHT; }
        int maxx = FRAMEBUFFER_WIDTH - mouse_x;
        if (maxx > MOUSE_CURSOR_WIDTH) { maxx = MOUSE_CURSOR_WIDTH; }
        const uint8_t *cursor = mouse_cursor;
        dst = frame_buffer + (mouse_y * FRAMEBUFFER_WIDTH) + mouse_x;
        for (int y = 0; y < maxy; y++) {
            const uint8_t *next_cursor = cursor + MOUSE_CURSOR_WIDTH;
            uint16_t *next_dst = dst + FRAMEBUFFER_WIDTH;
            for (int x = 0; x < maxx; x++) {
                const int pixel = (int) *(cursor++);
                if (pixel < (sizeof (glyph_color) - 1)) {
                    *dst = glyph_color[pixel];
                }
                dst++;
            }
            cursor = next_cursor;
            dst = next_dst;
        }
    }
}

static retro_usec_t prev_runtime_usecs = 0;
static retro_usec_t runtime_usecs = 0;

static void RETRO_CALLCONV frame_time_callback(retro_usec_t usec)
{
    prev_runtime_usecs = runtime_usecs;
    runtime_usecs += usec;
}

static bool must_update_frame_buffer = true;
static uint8 *next_inputbuf = NULL;   // where to write the next input for this player.
static uint8 next_inputbuflen = 0;
static uint16 next_operands[2];  // to save off the READ operands for later.
static int next_inputbuf_pos = 0;
static bool input_ready = 0;

static void scroll_back_page(void)
{
    int32_t offset = TERMINAL_HEIGHT - GState->upper_window_line_count;
    if (GState->status_bar_enabled) {
        offset--;
    }
    scrollback_read_pos -= offset - (virtual_keyboard_height / TERMINAL_CHAR_HEIGHT);
    if (scrollback_read_pos < (SCROLLBACK_LINES - scrollback_count)) {
        scrollback_read_pos = SCROLLBACK_LINES - scrollback_count;
    }
    must_update_frame_buffer = true;
}

static void scroll_forward_page(void)
{
    const int maxpos = SCROLLBACK_LINES - TERMINAL_HEIGHT;
    int32_t offset = TERMINAL_HEIGHT - GState->upper_window_line_count;
    if (GState->status_bar_enabled) {
        offset--;
    }
    scrollback_read_pos += offset - (virtual_keyboard_height / TERMINAL_CHAR_HEIGHT);
    if (scrollback_read_pos > maxpos) {
        scrollback_read_pos = maxpos;
    }
    must_update_frame_buffer = true;
}

static void scroll_back_line(void)
{
    if (scrollback_read_pos > (SCROLLBACK_LINES - scrollback_count)) {
        scrollback_read_pos--;
        must_update_frame_buffer = true;
    }
}

static void scroll_forward_line(void)
{
    const int maxpos = SCROLLBACK_LINES - TERMINAL_HEIGHT;
    if (scrollback_read_pos < maxpos) {
        scrollback_read_pos++;
        must_update_frame_buffer = true;
    }
}

static bool handle_keypress(const bool down, const unsigned keycode, const uint32_t ch)
{
    char *terminal_buffer = scrollback[SCROLLBACK_LINES - TERMINAL_HEIGHT];
    if (!down) {  // don't care about keyup events.
        return false;
    } else if (keycode == RETROK_UP) {
        scroll_back_line();
        return true;
    } else if (keycode == RETROK_DOWN) {
        scroll_forward_line();
        return true;
    } else if (keycode == RETROK_PAGEUP) {
        scroll_back_page();
        return true;
    } else if (keycode == RETROK_PAGEDOWN) {
        scroll_forward_page();
        return true;
    } else if (input_ready) {  // drop keys if we have a buffer of input waiting to process. You'd have to type _really_ fast to get bit by this, though!
        return false;
    } else if (GState && GState->quit) {
        return false;  // ignore input if game has ended.
    } else if (keycode == RETROK_BACKSPACE) {
        if (next_inputbuf_pos > 0) {
            next_inputbuf_pos--;
            terminal_buffer[cursor_position--] = (char) ' ';
            scrollback_read_pos = SCROLLBACK_LINES - TERMINAL_HEIGHT;  // if we are in scrollback, snap back to the present time.
        }
        return true;
    } else if ((keycode == RETROK_KP_ENTER) || (keycode == RETROK_RETURN)) {
        input_ready = true;
        next_inputbuf[next_inputbuf_pos++] = (uint8_t) '\0';
        scrollback_read_pos = SCROLLBACK_LINES - TERMINAL_HEIGHT;  // if we are in scrollback, snap back to the present time.
        return true;
    } else if ((ch < 32) || (ch >= 127)) {  // basic ASCII only, sorry.
        return false;
    } else if ((next_inputbuf_pos >= next_inputbuflen) || (cursor_position >= ((TERMINAL_WIDTH * TERMINAL_HEIGHT) - 2))) {
        return false;  // drop this, they're typing too much.
    }

    scrollback_read_pos = SCROLLBACK_LINES - TERMINAL_HEIGHT;  // if we are in scrollback, snap back to the present time.
    next_inputbuf[next_inputbuf_pos++] = (uint8_t) ch;
    terminal_buffer[cursor_position++] = (char) ch;
    return true;
}

static void handle_mouse_wheel(const int16_t x, const int16_t y, const int16_t wheelup, const int16_t wheeldown)
{
    current_input_device = CURRENTINPUTDEV_MOUSE;
    enable_virtual_keyboard(true);

    if (y < (FRAMEBUFFER_HEIGHT - virtual_keyboard_height)) {  // only deal with scrolling if in the terminal, not the keyboard.
        const int16_t direction = wheeldown - wheelup;  // zero == motion cancelled out, negative == scroll back, positive == scroll forward.
        if (direction < 0) {
            scroll_back_line();
        } else if (direction > 0) {
            scroll_forward_line();
        }
    }
}

static void update_virtual_key_under_position(const int x, const int y)
{
    if (!virtual_keyboard_enabled || (virtual_keyboard_height != VIRTUAL_KEYBOARD_HEIGHT)) {
        virtual_keyboard_key_highlighted = NULL;
        virtual_keyboard_key_pressed = false;
        return;
    } else if (virtual_keyboard_key_pressed) {
        return;  // don't change highlighting if something is pressed down, even if we move off it.
    }

    const int16_t yoffset = (FRAMEBUFFER_HEIGHT - virtual_keyboard_height);
    for (int keyy = 0; keyy < (sizeof (virtual_keyboard_keys) / sizeof (virtual_keyboard_keys[0])); keyy++) {
        for (int keyx = 0; keyx < (sizeof (virtual_keyboard_keys[0]) / sizeof (virtual_keyboard_keys[0][0])); keyx++) {
            const VirtualKeyboardKey *k = &virtual_keyboard_keys[keyy][keyx];
            const int realy = k->y + yoffset;
            if ( (x >= k->x) && (x < (k->x + k->w)) && (y >= realy) && (y < (realy + k->h)) ) {
                if (virtual_keyboard_key_highlighted != k) {
                    virtual_keyboard_key_highlighted = k;
                    must_update_frame_buffer = true;
                }
                return;
            }
        }
    }
    virtual_keyboard_key_highlighted = NULL;
    must_update_frame_buffer = true;
}

static void update_virtual_key_under_mouse_cursor(void)
{
    if (current_input_device == CURRENTINPUTDEV_MOUSE) {  // don't touch this if we aren't using a mouse.
        update_virtual_key_under_position(mouse_x, mouse_y);
    }
}

static void handle_mouse_press(const int16_t x, const int16_t y, const bool pressed)
{
    if (mouse_button_down == pressed) {
        return;  // nothing's changed.
    }

    mouse_button_down = pressed;
    must_update_frame_buffer = true;

    if (pressed) {  // mouse presses claim the current input.
        current_input_device = CURRENTINPUTDEV_MOUSE;
        enable_virtual_keyboard(true);
    } else if (current_input_device != CURRENTINPUTDEV_MOUSE) {
        return;  // might be a mouseup _after_ something else claimed the current input.
    }

    // only process input on the virtual keyboard when it's fully exposed, not when disabled or sliding in/out.
    if (!virtual_keyboard_enabled || (virtual_keyboard_height != VIRTUAL_KEYBOARD_HEIGHT)) {
        return;
    }

    if (pressed) {
        if (virtual_keyboard_key_highlighted) {
            virtual_keyboard_key_pressed = true;
        }
    } else {
        if (virtual_keyboard_key_highlighted && virtual_keyboard_key_pressed) {
            const int16_t yoffset = (FRAMEBUFFER_HEIGHT - virtual_keyboard_height);
            const VirtualKeyboardKey *k = virtual_keyboard_key_highlighted;
            const int realy = k->y + yoffset;
            if ( (x >= k->x) && (x < (k->x + k->w)) && (y >= realy) && (y < (realy + k->h)) ) {  // still in the pressed key when letting go? Click.
                //log_cb(RETRO_LOG_INFO, "Mouse released on '%s'.\n", k->sym);
                handle_keypress(true, k->key, ((((int) k->key) >= 32) && (((int) k->key) < 127)) ? (char) k->key : 0);
                handle_keypress(false, k->key, ((((int) k->key) >= 32) && (((int) k->key) < 127)) ? (char) k->key : 0);
            }
            virtual_keyboard_key_pressed = false;
            update_virtual_key_under_mouse_cursor();
        }
    }
}

static void handle_touch(const int16_t touchx, const int16_t touchy, const bool pressed)
{
    // adjust position to screen coordinates...
    const int32_t x = pressed ? ((int32_t) ((((((float) touchx) / ((float) 0x7FFF)) + 1.0f) / 2.0f) * FRAMEBUFFER_WIDTH)) : previous_touch_x;
    const int32_t y = pressed ? ((int32_t) ((((((float) touchy) / ((float) 0x7FFF)) + 1.0f) / 2.0f) * FRAMEBUFFER_HEIGHT)) : previous_touch_y;

    previous_touch_x = x;
    previous_touch_y = y;

    if (touch_pressed && pressed) {  // already touching, maybe treat it as a scrolling operation?
        if (!virtual_keyboard_enabled || (virtual_keyboard_height != VIRTUAL_KEYBOARD_HEIGHT)) {
            return;  // ignore scrolling if the keyboard is still sliding in.
        }

        // only allow scrolling if touch started in game area and not the virtual keyboard.
        if ((touch_scrolling == -1) && (y < (FRAMEBUFFER_HEIGHT - VIRTUAL_KEYBOARD_HEIGHT))) {
            touch_scrolling = y;
        }

        if (touch_scrolling == -1) {
            return;  // not a scroll operation, we're done here.
        }

        int32_t difference = touch_scrolling - ((int32_t) y);
        const int32_t difference_threshold = TERMINAL_CHAR_HEIGHT;
        if (difference < 0) {  // touch is moving down the screen.
            while (difference < -difference_threshold) {
                scroll_back_line();
                touch_scrolling += difference_threshold;
                difference += difference_threshold;
            }
        } else if (difference > 0) {  // touch is moving up the screen.
            while (difference > difference_threshold) {
                scroll_forward_line();
                touch_scrolling -= difference_threshold;
                difference -= difference_threshold;
            }
        }
        return;  // we're done here for now.
    }

    // scrolling is processed, now see if there's activity on the virtual keyboard.

    if (touch_pressed == pressed) {
        return;  // nothing's changed.
    }

    touch_pressed = pressed;
    touch_scrolling = -1;   // reset this because it's either a new touch or a released touch.

    if (pressed) {  // touches claim the current input.
        current_input_device = CURRENTINPUTDEV_TOUCH;
        enable_virtual_keyboard(true);
    } else if (current_input_device != CURRENTINPUTDEV_TOUCH) {
        return;  // might be a release _after_ something else claimed the current input.
    }

    // only process input on the virtual keyboard when it's fully exposed, not when disabled or sliding in/out.
    if (!virtual_keyboard_enabled || (virtual_keyboard_height != VIRTUAL_KEYBOARD_HEIGHT)) {
        return;
    }

    if (pressed) {
        update_virtual_key_under_position(x, y);
        if (virtual_keyboard_key_highlighted) {
            //log_cb(RETRO_LOG_INFO, "Touch pressed on '%s'.\n", virtual_keyboard_key_highlighted->sym);
            virtual_keyboard_key_pressed = true;
        }
    } else {
        if (virtual_keyboard_key_highlighted && virtual_keyboard_key_pressed) {
            const int16_t yoffset = (FRAMEBUFFER_HEIGHT - virtual_keyboard_height);
            const VirtualKeyboardKey *k = virtual_keyboard_key_highlighted;
            const int realy = k->y + yoffset;
            if ( (x >= k->x) && (x < (k->x + k->w)) && (y >= realy) && (y < (realy + k->h)) ) {  // still in the pressed key when letting go? Click.
                //log_cb(RETRO_LOG_INFO, "Touch released on '%s'.\n", k->sym);
                handle_keypress(true, k->key, ((((int) k->key) >= 32) && (((int) k->key) < 127)) ? (char) k->key : 0);
                handle_keypress(false, k->key, ((((int) k->key) >= 32) && (((int) k->key) < 127)) ? (char) k->key : 0);
            }
            virtual_keyboard_key_pressed = false;
            virtual_keyboard_key_highlighted = NULL;
            must_update_frame_buffer = true;
        }
    }
}

static bool process_keyboard_callback(bool down, unsigned keycode, uint32_t ch)
{
    current_input_device = CURRENTINPUTDEV_KEYBOARD;
    enable_virtual_keyboard(false);

    // RetroArch on several platforms will not return a character code, only a keycode,
    //  so in that case, we treat the keycode as a value from a US keyboard. It's not
    //  a perfect solution, but it's all we can do.
    if ( (ch == 0) && ((keycode >= 32) && (keycode <= 126)) ) {
        ch = (uint32_t) keycode;  // the keycodes map to US ASCII anyhow.
    }

    return handle_keypress(down, keycode, ch);
}

static void RETRO_CALLCONV keyboard_callback(bool down, unsigned keycode, uint32_t ch, uint16_t key_modifiers)
{
    must_update_frame_buffer = process_keyboard_callback(down, keycode, ch);
}

static void query_controller_state(ControllerState *state)
{
    state->up = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP) != 0;
    state->down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN) != 0;
    state->left = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT) != 0;
    state->right = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT) != 0;
    state->a = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) != 0;
    state->b = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B) != 0;
    state->x = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X) != 0;
    state->y = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y) != 0;
    state->select = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT) != 0;
    state->start = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) != 0;
    state->l1 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L) != 0;
    state->l2 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) != 0;
    state->l3 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3) != 0;
    state->r1 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R) != 0;
    state->r2 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) != 0;
    state->r3 = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3) != 0;
}

static void handle_controller_input(void)
{
    ControllerState newstate;
    query_controller_state(&newstate);

    bool has_new_input = false;
    ControllerState new_press;

    #define CHECK_IF_NEW_PRESS(field) \
        if ((new_press.field = (newstate.field && !current_controller_state.field)) != false) { \
            /*log_cb(RETRO_LOG_INFO, "Controller input '" #field "' newly pressed!\n");*/ \
            has_new_input = true; \
        }
    CHECK_IF_NEW_PRESS(up);
    CHECK_IF_NEW_PRESS(down);
    CHECK_IF_NEW_PRESS(left);
    CHECK_IF_NEW_PRESS(right);
    CHECK_IF_NEW_PRESS(a);
    CHECK_IF_NEW_PRESS(b);
    CHECK_IF_NEW_PRESS(x);
    CHECK_IF_NEW_PRESS(y);
    CHECK_IF_NEW_PRESS(select);
    CHECK_IF_NEW_PRESS(start);
    CHECK_IF_NEW_PRESS(l1);
    CHECK_IF_NEW_PRESS(l2);
    CHECK_IF_NEW_PRESS(l3);
    CHECK_IF_NEW_PRESS(r1);
    CHECK_IF_NEW_PRESS(r2);
    CHECK_IF_NEW_PRESS(r3);
    #undef CHECK_IF_NEW_PRESS

    if (has_new_input) {
        current_input_device = CURRENTINPUTDEV_CONTROLLER;
        enable_virtual_keyboard(true);
    }

    memcpy(&current_controller_state, &newstate, sizeof (newstate));

    if (current_input_device != CURRENTINPUTDEV_CONTROLLER) {
        return;  // ignore the rest of this if we aren't using a controller.
    }


    // this is all subject to change.

    if (new_press.l1) {  // scroll forward
        scroll_back_line();
    } else if (new_press.l2) {  // scroll back
        scroll_back_page();
    } else if (new_press.r1) {  // scroll forward
        scroll_forward_line();
    } else if (new_press.r2) {  // scroll back
        scroll_forward_page();
    }

    // only process input on the virtual keyboard when it's fully exposed, not when disabled or sliding in/out.
    if (!virtual_keyboard_enabled || (virtual_keyboard_height != VIRTUAL_KEYBOARD_HEIGHT)) {
        return;
    }

    if (!virtual_keyboard_key_pressed) {  // can't move to a new key or use a shortcut while one is pressed.
        if (new_press.up) {
            last_controller_direction = -1;
            if (controller_y == 0) {
                controller_y = (sizeof (virtual_keyboard_keys) / sizeof (virtual_keyboard_keys[0])) - 1;
            } else {
                controller_y--;
            }
        } else if (new_press.down) {
            last_controller_direction = -1;
            if (controller_y == ((sizeof (virtual_keyboard_keys) / sizeof (virtual_keyboard_keys[0])) - 1)) {
                controller_y = 0;
            } else {
                controller_y++;
            }
        } else if (new_press.left) {
            last_controller_direction = -1;
            if (controller_x == 0) {
                controller_x = (sizeof (virtual_keyboard_keys[0]) / sizeof (virtual_keyboard_keys[0][0])) - 1;
            } else {
                controller_x--;
            }
        } else if (new_press.right) {
            last_controller_direction = 1;
            if (controller_x == ((sizeof (virtual_keyboard_keys[0]) / sizeof (virtual_keyboard_keys[0][0])) - 1)) {
                controller_x = 0;
            } else {
                controller_x++;
            }
        }

        int adjusted_x = controller_x;
        while (virtual_keyboard_keys[controller_y][adjusted_x].sym == NULL) {
            if ((last_controller_direction < 0) && (adjusted_x == 0)) {
                adjusted_x = (sizeof (virtual_keyboard_keys[0]) / sizeof (virtual_keyboard_keys[0][0])) - 1;
            } else if ((last_controller_direction > 0) && (adjusted_x == ((sizeof (virtual_keyboard_keys[0]) / sizeof (virtual_keyboard_keys[0][0])) - 1))) {
                adjusted_x = 0;
            } else {
                adjusted_x += last_controller_direction;
            }
        }

        // only allow one of these events per-frame to avoid confusion.

        if (virtual_keyboard_key_highlighted != &virtual_keyboard_keys[controller_y][adjusted_x]) {
            virtual_keyboard_key_highlighted = &virtual_keyboard_keys[controller_y][adjusted_x];
            must_update_frame_buffer = true;
        } else if (new_press.b) {  // press the virtual key.
            const VirtualKeyboardKey *k = virtual_keyboard_key_highlighted;
            handle_keypress(true, k->key, ((((int) k->key) >= 32) && (((int) k->key) < 127)) ? (char) k->key : 0);
            handle_keypress(false, k->key, ((((int) k->key) >= 32) && (((int) k->key) < 127)) ? (char) k->key : 0);
            virtual_keyboard_key_pressed = true;
            must_update_frame_buffer = true;
        } else if (new_press.y) {   // treat this as backspace on the keyboard.
            handle_keypress(true, RETROK_BACKSPACE, 0);
            handle_keypress(false, RETROK_BACKSPACE, 0);
            must_update_frame_buffer = true;
        } else if (new_press.a) {   // treat this as space on the keyboard.
            handle_keypress(true, RETROK_SPACE, ' ');
            handle_keypress(false, RETROK_SPACE, ' ');
            must_update_frame_buffer = true;
        } else if (new_press.x) {   // treat this as ENTER on the keyboard.
            handle_keypress(true, RETROK_RETURN, 0);
            handle_keypress(false, RETROK_RETURN, 0);
            must_update_frame_buffer = true;
        }
    } else {
        if (!newstate.b) {
            virtual_keyboard_key_pressed = false;
            must_update_frame_buffer = true;
        }
    }
}


static void writestr_mojozork_libretro(const char *str, const uintptr slen);

static int update_input(void)  // returns non-zero if the screen changed.
{
    input_poll_cb();

    static bool first_run = true;
    if (first_run) {
        first_run = false;
        query_controller_state(&current_controller_state);  // make sure this has a default state so we don't think the controller is being touched at startup.
    }

	int16_t new_mouse_x = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
	int16_t new_mouse_y = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

	const bool new_mouse_left = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) ? true : false;
	const int16_t new_mouse_wheelup = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP);
	const int16_t new_mouse_wheeldown = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);

    /* RetroArch appears to send a mouse state at the start to let you move the cursor to its actual start position.
       mouse_* starts set to 0xFFFFFFFF, an impossible position, so we can identify this as the first start-position
       input and not decide the user is now using a mouse as their primary input. */
    if (new_mouse_x && (mouse_x == 0xFFFFFFFF)) {
        mouse_x = new_mouse_x;
        new_mouse_x = 0;
    }
    if (new_mouse_y && (mouse_y == 0xFFFFFFFF)) {
        mouse_y = new_mouse_y;
        new_mouse_y = 0;
    }

    if (new_mouse_x || new_mouse_y) {
        // only redraw unless the user has previously clicked a button to make this the current input,
        //  and don't take mouse motion as a signal to make it the current input device, so bumping the
        // mouse a little doesn't cause confusion.
        if (current_input_device == CURRENTINPUTDEV_MOUSE) {
            must_update_frame_buffer = true;
        }
        mouse_x += new_mouse_x;
        if (mouse_x < 0) { mouse_x = 0; } else if (mouse_x >= FRAMEBUFFER_WIDTH) { mouse_x = FRAMEBUFFER_WIDTH-1; }
        mouse_y += new_mouse_y;
        if (mouse_y < 0) { mouse_y = 0; } else if (mouse_y >= FRAMEBUFFER_HEIGHT) { mouse_y = FRAMEBUFFER_HEIGHT-1; }
        update_virtual_key_under_mouse_cursor();
    }

    #if TEST_TOUCH_WITH_MOUSE
    const bool new_pointer_pressed = new_mouse_left;
    const int16_t new_pointer_x = scale_mouse_to_touch_coords(mouse_x, FRAMEBUFFER_WIDTH);
    const int16_t new_pointer_y = scale_mouse_to_touch_coords(mouse_y, FRAMEBUFFER_HEIGHT);
    must_update_frame_buffer = true;
    #elif IGNORE_TOUCH_INPUT  // !!! FIXME: right now RetroArch doesn't distinguish between touch and mouse on RETRO_DEVICE_POINTER, so just disable touch if this isn't a touchy platform, like mobile.
    const bool new_pointer_pressed = false;
    const int16_t new_pointer_x = 0;
    const int16_t new_pointer_y = 0;
    #else
    const bool new_pointer_pressed = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) ? true : false;
    const int16_t new_pointer_x = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
    const int16_t new_pointer_y = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
    #endif

    // some devices, might send touch _and_ mouse input at the same time. Ignore the mouse in that case.
    if (!new_pointer_pressed || !new_mouse_left) {  // both mouse and touch in the same frame? Assume it's mirroring input. Only process mouse if not.
        if (new_mouse_left != mouse_button_down) {
            handle_mouse_press(mouse_x, mouse_y, new_mouse_left);
        }

        if (new_mouse_wheelup || new_mouse_wheeldown) {
            handle_mouse_wheel(mouse_x, mouse_y, new_mouse_wheelup, new_mouse_wheeldown);
        }
    }

    handle_touch(new_pointer_x, new_pointer_y, new_pointer_pressed);

    handle_controller_input();

    if (input_ready) {
        char *terminal_buffer = scrollback[SCROLLBACK_LINES - TERMINAL_HEIGHT];
        terminal_buffer[cursor_position] = (char) ' ';
        writestr_mojozork_libretro("\n", 1);

        // !!! FIXME: this is a hack. Blank out invalid characters.
        for (char *ptr = (char *) next_inputbuf; *ptr; ptr++) {
            const char ch = *ptr;
            if ((ch >= 'A') && (ch <= 'Z')) {
                *ptr = 'a' + (ch - 'A');  // lowercase it.
            } else if (((ch >= 'a') && (ch <= 'z')) || ((ch >= '0') && (ch <= '9')) || (strchr(" .,!?_#'\"/\\-:()", ch) != NULL)) {
                /* cool */ ;
            } else {
                *ptr = ' ';  /* oh well, blank it out. */
            }
        }

        // trim whitespace.
        for (char *ptr = (char *) next_inputbuf; *ptr; ptr++) {
            if (*ptr != ' ') {
                memmove(next_inputbuf, ptr, strlen(ptr) + 1);
                break;
            }
        }

        char *lastnonspace = NULL;
        for (char *ptr = (char *) next_inputbuf; *ptr; ptr++) {
            if (*ptr != ' ') {
                lastnonspace = ptr;
            }
        }
        if (lastnonspace != NULL) {
            lastnonspace[1] = '\0';
        }

        // special case input for interpreter, not the game.
        if (strncmp((const char *) next_inputbuf, "#random ", 8) == 0) {
            const uint16 val = doRandom((sint16) atoi((const char *) (next_inputbuf+8)));
            char msg[128];
            snprintf(msg, sizeof (msg), "*** random replied: %u\n>", (unsigned int) val);
            writestr(msg);
            next_inputbuf_pos = 0;  // go again.
            input_ready = false;
            return 1;
        }

        GState->operands[0] = next_operands[0];  // tokenizing needs this.
        GState->operands[1] = next_operands[1];
        GState->operand_count = 2;
        tokenizeUserInput();  // now the Z-Machine will get what it expects from the previous READ instruction.
        next_inputbuf = NULL;  // don't do this again until we hit another READ opcode.
        next_inputbuflen = 0;

        step_zmachine();  // run until we get to the next input prompt.

        next_inputbuf_pos = 0;
        input_ready = false;  // reset for next input.
        return 1;  // notify caller we need to redraw the framebuffer.
    }

    return 0;  // nothing happening at the moment.
}

void retro_run(void)
{
    bool frame_is_dupe = true;
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        check_variables();
    }

    if (update_input()) {
        must_update_frame_buffer = true;
    }

    if (virtual_keyboard_enabled && (virtual_keyboard_height != VIRTUAL_KEYBOARD_HEIGHT)) {
        virtual_keyboard_height += 5;  // !!! FIXME: do this based on time.
        if (virtual_keyboard_height > VIRTUAL_KEYBOARD_HEIGHT) {
            virtual_keyboard_height = VIRTUAL_KEYBOARD_HEIGHT;
        }
        must_update_frame_buffer = true;
    } else if (!virtual_keyboard_enabled && (virtual_keyboard_height != 0)) {
        virtual_keyboard_height -= 5;  // !!! FIXME: do this based on time.
        if (virtual_keyboard_height < 0) {
            virtual_keyboard_height = 0;
        }
        must_update_frame_buffer = true;
    }

    // only blink the cursor if not wading through the scrollback.
    if (scrollback_read_pos== (SCROLLBACK_LINES - TERMINAL_HEIGHT)) {
        char *terminal_buffer = scrollback[scrollback_read_pos];
        const char cursor_char = ((runtime_usecs / 1000000) % 2) ? ' ' : 0xFF;
        if (cursor_char != terminal_buffer[cursor_position]) {
            terminal_buffer[cursor_position] = cursor_char;
            must_update_frame_buffer = true;
        }
    }

    if (must_update_frame_buffer) {   /* don't bother redrawing if nothing changed. */
        update_frame_buffer();
        must_update_frame_buffer = false;
        frame_is_dupe = false;
    }

    video_cb((frame_is_dupe && frontend_supports_frame_dupe) ? NULL : frame_buffer, FRAMEBUFFER_WIDTH, FRAMEBUFFER_HEIGHT, FRAMEBUFFER_WIDTH * 2);
}

static void split_window_mojozork_libretro(const uint16 oldval, const uint16 newval)
{
    if (GState->header.version == 3) { // clear the new top window only in ver3.
        memset(upper_window, ' ', sizeof (upper_window));
        must_update_frame_buffer = true;
    } else {
        // !!! FIXME: existing screen contents are meant to be preserved? Copy them from scrollback to upper_window?
        // 8.6.1: "Selecting, or re-sizing, the upper window does not change the screen's appearance."
    }
}

static void set_window_mojozork_libretro(const uint16 oldval, const uint16 newval)
{
    if (newval == 1) {  // upper window
        // 8.6.1: "Whenever the upper window is selected, its cursor position is reset to the top left."
        upper_window_cursor_position = 0;
    }
}

static void writestr_mojozork_libretro(const char *str, const uintptr slen)
{
    char *terminal_buffer = scrollback[SCROLLBACK_LINES - TERMINAL_HEIGHT];

    if (GState->current_window == 0) {
        scrollback_read_pos = SCROLLBACK_LINES - TERMINAL_HEIGHT;  // if we are in scrollback and write to window 0, snap back to the present time.

        for (uintptr i = 0; i < slen; i++) {
            const char ch = str[i];
            if (ch == '\n') {
                /* !!! FIXME: this is laziness; we should use a ring buffer, but it'd make all of this more complicated. */
                memmove(scrollback, &scrollback[1], (SCROLLBACK_LINES - 1) * TERMINAL_WIDTH);
                memset(scrollback[SCROLLBACK_LINES-1], ' ', TERMINAL_WIDTH);
                cursor_position = TERMINAL_WIDTH * (TERMINAL_HEIGHT-1);
                terminal_word_start = -1;
                if (scrollback_count < SCROLLBACK_LINES) {
                    scrollback_count++;
                }
            } else {
                if (ch == ' ') {
                    terminal_word_start = -1;
                } else if (terminal_word_start == -1) {
                    terminal_word_start = cursor_position;
                }

                if ((cursor_position % TERMINAL_WIDTH) == (TERMINAL_WIDTH-1)) {
                    const int wordlen = cursor_position - terminal_word_start;
                    if ((terminal_word_start != -1) && (wordlen < 15)) {  // do a simple wordwrap if possible.
                        char tmpbuf[17];
                        tmpbuf[0] = '\n';
                        memcpy(tmpbuf + 1, terminal_buffer + terminal_word_start, wordlen);
                        memset(terminal_buffer + terminal_word_start, ' ', wordlen);
                        cursor_position = terminal_word_start;
                        writestr_mojozork_libretro(tmpbuf, wordlen + 1);
                    } else {
                        writestr_mojozork_libretro("\n", 1);
                    }
                    if (ch != ' ') {
                        writestr_mojozork_libretro(&ch, 1);
                    }
                } else {
                    terminal_buffer[cursor_position++] = ch;
                }
            }
        }
        must_update_frame_buffer = true;
    } else if (GState->current_window == 1) {  // upper window
        const int32_t max_cursor_pos = GState->upper_window_line_count * TERMINAL_WIDTH;
        // upper window does no word wrap or scrolling.
        for (uintptr i = 0; i < slen; i++) {
            if (upper_window_cursor_position >= max_cursor_pos) {
                break;
            }

            const char ch = str[i];
            if (ch == '\n') {
                const int32_t adjust = TERMINAL_WIDTH - (upper_window_cursor_position % TERMINAL_WIDTH);
                upper_window_cursor_position += adjust;
            } else {
                upper_window[upper_window_cursor_position++] = ch;
            }
        }
        must_update_frame_buffer = true;
    } else {
        // !!! FIXME: can there be more windows in later Z-Machine revisions?
    }
}

static void writestr(const char *str)
{
    const uint16_t selected_window = GState->current_window;
    GState->current_window = 0;  // always write to the "normal" window.
    writestr_mojozork_libretro(str, strlen(str));
    GState->current_window = selected_window;
}


#if defined(__GNUC__) || defined(__clang__)
static void die_mojozork_libretro(const char *fmt, ...) __attribute__((noreturn));
#elif defined(_MSC_VER)
__declspec(noreturn) static void die_mojozork_libretro(const char *fmt, ...);
#endif
static void die_mojozork_libretro(const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    writestr("\nERROR: ");
    va_start(ap, fmt);
    vsnprintf(buf, sizeof (buf), fmt, ap);
    va_end(ap);
    writestr(buf);
    snprintf(buf, sizeof (buf), " (pc=%X)\n", (unsigned int) GState->logical_pc);
    writestr(buf);
    snprintf(buf, sizeof (buf), " %u instructions run\n", (unsigned int) GState->instructions_run);
    writestr(buf);
    writestr("\n\n*** THE Z-MACHINE HAS CRASHED ***\n");

    GState->step_completed = GState->quit = 1;
    longjmp(jmpbuf, 1);
}

static void opcode_save_mojozork_libretro(void)
{
    writestr("Just use RetroArch save states, please.\n");
    doBranch(0);
}

static void opcode_restore_mojozork_libretro(void)
{
    writestr("Just use RetroArch save states, please.\n");
    doBranch(0);
}

static void restart_game(void);

static void opcode_restart_mojozork_libretro(void)
{
    restart_game();
}

static void opcode_read_mojozork_libretro(void)
{
    dbg("read from input stream: text-buffer=%X parse-buffer=%X\n", (unsigned int) GState->operands[0], (unsigned int) GState->operands[1]);

    uint8 *input = GState->story + GState->operands[0];
    const uint8 inputlen = *(input++);
    dbg("max input: %u\n", (unsigned int) inputlen);
    if (inputlen < 3)
        GState->die("text buffer is too small for reading");  // happens on buffer overflow.

    const uint8 *parse = GState->story + GState->operands[1];
    const uint8 parselen = *(parse++);

    dbg("max parse: %u\n", (unsigned int) parselen);
    if (parselen < 4)
        GState->die("parse buffer is too small for reading");  // happens on buffer overflow.

    updateStatusBar();

    next_inputbuf = input;
    next_inputbuflen = inputlen;
    next_operands[0] = GState->operands[0];
    next_operands[1] = GState->operands[1];
    GState->logical_pc = (uint32) (GState->pc - GState->story);  // next time, run the instructions _right after_ the READ opcode.
    GState->step_completed = 1;  // time to break out of the Z-Machine simulation loop.
}


static uint8 *original_story = NULL;
static size_t original_story_len = 0;

static ZMachineState zmachine_state;

static void restart_game(void)
{
    check_variables();

    random_seed = (int) time(NULL);

    memset(&zmachine_state, '\0', sizeof (zmachine_state));
    GState = &zmachine_state;
    GState->writestr = writestr_mojozork_libretro;
    GState->die = die_mojozork_libretro;
    GState->split_window = split_window_mojozork_libretro;
    GState->set_window = set_window_mojozork_libretro;

    memset(scrollback, ' ', sizeof (scrollback));
    memset(upper_window, ' ', sizeof (upper_window));
    scrollback_read_pos = SCROLLBACK_LINES - TERMINAL_HEIGHT;
    terminal_word_start = -1;
    cursor_position = TERMINAL_WIDTH * (TERMINAL_HEIGHT-1);
    must_update_frame_buffer = true;
    next_inputbuf = NULL;
    next_inputbuflen = 0;
    next_operands[0] = next_operands[1] = 0;
    next_inputbuf_pos = 0;
    input_ready = false;
    runtime_usecs = 0;

    uint8 *story = (uint8 *) malloc(original_story_len);
    if (!story) {
        writestr("\n\n*** OUT OF MEMORY, ABORTING ***\n");
        GState->quit = 1;
    } else {
        memcpy(story, original_story, original_story_len);
        initStory(NULL, story, (uint32) original_story_len);
        GState->opcodes[181].fn = opcode_save_mojozork_libretro;
        GState->opcodes[182].fn = opcode_restore_mojozork_libretro;
        GState->opcodes[183].fn = opcode_restart_mojozork_libretro;
        GState->opcodes[228].fn = opcode_read_mojozork_libretro;

        memset(status_bar, ' ', sizeof (status_bar));
        GState->status_bar_enabled = 1;
        GState->status_bar = status_bar;
        GState->status_bar_len = sizeof (status_bar);
        GState->story[1] &= ~(1<<4);  // so the game knows that a status bar is available
        GState->story[1] |= (1<<5);  // so the game knows that window-splitting is available

        // make sure the header matches our tweaks.
        const uint8 *ptr = GState->story;
        GState->header.version = READUI8(ptr);
        GState->header.flags1 = READUI8(ptr);
        step_zmachine();  // run until we get to the first input prompt.
    }
}

static void post_notification(const char *msgstr, const unsigned int duration)
{
    unsigned int msgver = 0;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &msgver)) {
        msgver = 0;
    }

    if (msgver == 0) {
        struct retro_message msg = { msgstr, duration };
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    } else {
        struct retro_message_ext msg;
        msg.msg = msgstr;
        msg.duration = duration;
        msg.priority = 99;
        msg.level = RETRO_LOG_INFO;
        msg.target = RETRO_MESSAGE_TARGET_OSD;
        msg.type = RETRO_MESSAGE_TYPE_NOTIFICATION_ALT;
        msg.progress = -1;
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
    }
}

static void explain_controls(void)
{
    static int already_told = 0;
    if (!already_told) {
        already_told = 1;
        post_notification("You can switch between a keyboard, controller, mouse, or touch to play.", 5000);
        post_notification("If you're using a physical keyboard, don't forget to set Focus Mode (press ScrollLock, usually)!", 5000);
    }
}

bool retro_load_game(const struct retro_game_info *info)
{
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
    {
        log_cb(RETRO_LOG_INFO, "RGB565 is not supported.\n");
        return false;
    }

    uint64_t serquirks = RETRO_SERIALIZATION_QUIRK_ENDIAN_DEPENDENT; // !!! FIXME
    environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &serquirks);

    free(original_story);
    original_story_len = info->size;
    original_story = (uint8 *) malloc(original_story_len);
    if (!original_story) {
        log_cb(RETRO_LOG_INFO, "out of memory!\n");
        return false;
    }

    explain_controls();

    memcpy(original_story, info->data, original_story_len);
    restart_game();
    return true;
}

void retro_unload_game(void)
{
    if (GState) {
        free(GState->story);
        free(GState->story_filename);
        GState = NULL;
    }

    free(original_story);
    original_story = NULL;
    original_story_len = 0;
}

void retro_reset(void)
{
    restart_game();
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   return false;
}


#define MOJOZORK_SERIALIZATION_MAGIC 0x6B5A6A4D  // littleendian number is "MjZk" in ASCII.
#define MOJOZORK_SERIALIZATION_CURRENT_VERSION 3

/* MAKE SURE THESE STAY IN ORDER: 64-bit first, 32 second, then 16, then BUFFER.
   This will make sure memory accesses stay aligned. */
#define MOJOZORK_SERIALIZE_STATE(version) { \
    MOJOZORK_SERIALIZE_UINT64(runtime_usecs) \
    MOJOZORK_SERIALIZE_UINT32(GState->instructions_run) \
    if (version < 2) { MOJOZORK_SERIALIZE_UINT32(GState->header.staticmem_addr) } /* whoops, serialized this twice. */ \
    MOJOZORK_SERIALIZE_UINT32(GState->logical_pc) \
    MOJOZORK_SERIALIZE_UINT32(scrollback_read_pos) \
    MOJOZORK_SERIALIZE_UINT32(scrollback_count) \
    MOJOZORK_SERIALIZE_SINT32(cursor_position) \
    MOJOZORK_SERIALIZE_SINT32(terminal_word_start) \
    if (version >= 1) { MOJOZORK_SERIALIZE_SINT32(GState->status_bar_enabled) } \
    MOJOZORK_SERIALIZE_UINT16(GState->header.staticmem_addr) \
    MOJOZORK_SERIALIZE_UINT16(logical_sp) \
    MOJOZORK_SERIALIZE_UINT16(GState->bp) \
    MOJOZORK_SERIALIZE_UINT16(GState->quit) \
    MOJOZORK_SERIALIZE_UINT16(GState->step_completed) \
    MOJOZORK_SERIALIZE_UINT16(GState->operand_count) \
    MOJOZORK_SERIALIZE_UINT16(next_operands[0]) \
    MOJOZORK_SERIALIZE_UINT16(next_operands[1]) \
    MOJOZORK_SERIALIZE_UINT16(logical_next_inputbuf) \
    MOJOZORK_SERIALIZE_UINT16(next_inputbuflen) \
    MOJOZORK_SERIALIZE_UINT16(next_inputbuf_pos) \
    MOJOZORK_SERIALIZE_UINT16(input_ready) \
    if (version >= 2) { MOJOZORK_SERIALIZE_UINT16(GState->current_window) } \
    if (version >= 2) { MOJOZORK_SERIALIZE_UINT16(GState->upper_window_line_count) } \
    if (version >= 2) { MOJOZORK_SERIALIZE_UINT16(upper_window_cursor_position) } \
    MOJOZORK_SERIALIZE_BUFFER(GState->story, GState->header.staticmem_addr) \
    MOJOZORK_SERIALIZE_BUFFER(GState->operands, sizeof (GState->operands)) \
    MOJOZORK_SERIALIZE_BUFFER(GState->stack, 256) /* hopefully 256 is enough */ \
    MOJOZORK_SERIALIZE_BUFFER(scrollback, sizeof (scrollback)) \
    if (version >= 2) { MOJOZORK_SERIALIZE_BUFFER(upper_window, sizeof (upper_window)) } \
    if (version >= 3) { MOJOZORK_SERIALIZE_SINT32(random_seed) } \
}

size_t retro_serialize_size(void)
{
    size_t retval = 0;

    #define MOJOZORK_SERIALIZE_UINT16(var) retval += sizeof (uint16);
    #define MOJOZORK_SERIALIZE_UINT32(var) retval += sizeof (uint32);
    #define MOJOZORK_SERIALIZE_SINT32(var) retval += sizeof (sint32);
    #define MOJOZORK_SERIALIZE_UINT64(var) retval += sizeof (uint64);
    #define MOJOZORK_SERIALIZE_BUFFER(var, siz) retval += siz;

    MOJOZORK_SERIALIZE_UINT32(MOJOZORK_SERIALIZATION_MAGIC);
    MOJOZORK_SERIALIZE_UINT32(MOJOZORK_SERIALIZATION_CURRENT_VERSION);
    MOJOZORK_SERIALIZE_STATE(MOJOZORK_SERIALIZATION_CURRENT_VERSION);

    #undef MOJOZORK_SERIALIZE_UINT16
    #undef MOJOZORK_SERIALIZE_UINT32
    #undef MOJOZORK_SERIALIZE_SINT32
    #undef MOJOZORK_SERIALIZE_UINT64
    #undef MOJOZORK_SERIALIZE_BUFFER

    //log_cb(RETRO_LOG_INFO, "Serialize size == %u bytes\n", (unsigned int) retval);

    return retval;
}

bool retro_serialize(void *data_, size_t size)
{
    uint8 *data = (uint8 *) data_;

    const uint16 logical_sp = (uint16) (GState->sp - GState->stack);
    const uint16 logical_next_inputbuf = next_inputbuf ? ((uint16) (GState->story - next_inputbuf)) : 0;

    assert(size >= retro_serialize_size());

    // !!! FIXME: byteswap
    #define MOJOZORK_SERIALIZE_UINT16(var) *((uint16 *) data) = (uint16) (var); data += sizeof (uint16);
    #define MOJOZORK_SERIALIZE_UINT32(var) *((uint32 *) data) = (uint32) (var); data += sizeof (uint32);
    #define MOJOZORK_SERIALIZE_SINT32(var) *((sint32 *) data) = (sint32) (var); data += sizeof (sint32);
    #define MOJOZORK_SERIALIZE_UINT64(var) *((uint64 *) data) = (uint64) (var); data += sizeof (uint64);
    #define MOJOZORK_SERIALIZE_BUFFER(var, siz) memcpy(data, var, (siz)); data += (siz);

    MOJOZORK_SERIALIZE_UINT32(MOJOZORK_SERIALIZATION_MAGIC);
    MOJOZORK_SERIALIZE_UINT32(MOJOZORK_SERIALIZATION_CURRENT_VERSION);
    MOJOZORK_SERIALIZE_STATE(MOJOZORK_SERIALIZATION_CURRENT_VERSION);

    #undef MOJOZORK_SERIALIZE_UINT16
    #undef MOJOZORK_SERIALIZE_UINT32
    #undef MOJOZORK_SERIALIZE_SINT32
    #undef MOJOZORK_SERIALIZE_UINT64
    #undef MOJOZORK_SERIALIZE_BUFFER

    return true;
}

bool retro_unserialize(const void *data_, size_t size)
{
    const uint8 *data = (const uint8 *) data_;
    uint16 logical_sp, logical_next_inputbuf;

    assert(size <= retro_serialize_size());

    // !!! FIXME: byteswap
    #define MOJOZORK_SERIALIZE_UINT16(var) var = *((uint16 *) data); data += sizeof (uint16);
    #define MOJOZORK_SERIALIZE_UINT32(var) var = *((uint32 *) data); data += sizeof (uint32);
    #define MOJOZORK_SERIALIZE_SINT32(var) var = *((sint32 *) data); data += sizeof (sint32);
    #define MOJOZORK_SERIALIZE_UINT64(var) var = *((uint64 *) data); data += sizeof (uint64);
    #define MOJOZORK_SERIALIZE_BUFFER(var, siz) memcpy(var, data, (siz)); data += (siz);

    uint32 magic = 0;
    uint32 version = 0;
    MOJOZORK_SERIALIZE_UINT32(magic);
    if (magic == MOJOZORK_SERIALIZATION_MAGIC) {
        MOJOZORK_SERIALIZE_UINT32(version);
    } else {
        data -= sizeof (uint32);  // first builds had no magic or version (rookie mistake!). We'll call that version 0, and move the serialization pointer back to the start.
    }

    // in the slim chance you end up with version 0 that had a timestamp that matched magic, everything will break, that's unfortunate.
    // in the better chance you feed this data that isn't a serialization stream at all, you are also screwed. But let's hope the frontend mitigates that.

    MOJOZORK_SERIALIZE_STATE(version);

    #undef MOJOZORK_SERIALIZE_UINT16
    #undef MOJOZORK_SERIALIZE_UINT32
    #undef MOJOZORK_SERIALIZE_SINT32
    #undef MOJOZORK_SERIALIZE_UINT64
    #undef MOJOZORK_SERIALIZE_BUFFER

    GState->status_bar = status_bar;
    GState->status_bar_len = sizeof (status_bar);
    GState->sp = GState->stack + logical_sp;
    next_inputbuf = logical_next_inputbuf ? (GState->story + logical_next_inputbuf) : NULL;

    updateStatusBar();

    // reset some input and view stuff.
    mouse_button_down = false;  // just in case
    touch_pressed = false;  // just in case
    touch_scrolling = -1;  // just in case
    previous_touch_x = previous_touch_y = 0;  // just in case
    scrollback_read_pos = SCROLLBACK_LINES - TERMINAL_HEIGHT;
    virtual_keyboard_key_highlighted = NULL;
    virtual_keyboard_key_pressed = false;

    must_update_frame_buffer = true;

    return true;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}


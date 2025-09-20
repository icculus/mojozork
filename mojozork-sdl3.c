/**
 * MojoZork; a simple, just-for-fun implementation of Infocom's Z-Machine.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define DRAW_OWN_MOUSE_CURSOR 0
#include "mojozork-libretro.c"   // yeah, this is nuts. This app just implements enough of a libretro host to run the libretro core, compiled inline.

static const char *visual_styles[] = { "Standard", "AppleII", "MS-DOS", "Commodore-64" };  // !!! FIXME: move to the libretro core, and build out the cvar from this list, to unify things?

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static SDL_Gamepad *gamepad = NULL;
static const char *style = NULL;
static bool retro_init_called = false;
static retro_frame_time_callback_t frame_time_callback_impl = NULL;
static retro_keyboard_event_t keyboard_event_impl = NULL;
static bool game_loaded = false;
static bool visual_style_updated = false;
static SDL_MouseButtonFlags current_mouse_buttons;
static int current_mouse_x, current_mouse_y;
static int prev_mouse_x, prev_mouse_y;
static int mouse_wheel_accumulator;
//static int prev_pointer_x, prev_pointer_y;

static SDL_AppResult panic(const char *title, const char *msg)
{
    char msgcpy[256];
    SDL_strlcpy(msgcpy, msg, sizeof (msgcpy));  // in case this is SDL_GetError() and the buffer changes.
    msg = msgcpy;

    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", title, msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, window);
    return SDL_APP_FAILURE;
}

static SDL_AppResult usage(void)
{
    return panic("USAGE", "<gamedata_path> [--style=STYLENAME]");
}

static void RETRO_CALLCONV log_entry_point(enum retro_log_level level, const char *fmt, ...)
{
    SDL_LogPriority prio = SDL_LOG_PRIORITY_INFO;
    switch (level) {
        #define PRIO(x) case RETRO_LOG_##x: prio = SDL_LOG_PRIORITY_##x; break
        PRIO(DEBUG);
        PRIO(INFO);
        PRIO(WARN);
        PRIO(ERROR);
        #undef PRIO
        default: break;
    }

    va_list ap;
    va_start(ap, fmt);
    SDL_LogMessageV(SDL_LOG_CATEGORY_APPLICATION, prio, fmt, ap);
    va_end(ap);
}

static bool RETRO_CALLCONV environment_entry_point(unsigned cmd, void *data)
{
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *((bool *) data) = visual_style_updated;
            visual_style_updated = false;
            return true;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *) data)->log = log_entry_point;
            return true;

        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = (struct retro_variable *) data;
            if (SDL_strcmp(var->key, "style") == 0) {
                var->value = style;
                return true;
            }
            var->value = NULL;
            return false;
        }

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            if (*((enum retro_pixel_format *) data) == RETRO_PIXEL_FORMAT_RGB565) {
                return true;
            }
            return false;

        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *((bool *) data) = true;
            return true;

        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
            const struct retro_frame_time_callback *ftcb = (const struct retro_frame_time_callback *) data;
            frame_time_callback_impl = ftcb->callback;
            char hintstr[64];
            SDL_snprintf(hintstr, sizeof (hintstr), "%f", (1.0 / ((double) ftcb->reference)) * 1000000.0);
            SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, hintstr);
            return true;
        }

        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
            keyboard_event_impl = ((struct retro_keyboard_callback *) data)->callback;
            return true;
    }
    return false;
}

static int16_t RETRO_CALLCONV input_state_entry_point(unsigned port, unsigned device, unsigned index, unsigned id)
{
    SDL_assert(port == 0);
    SDL_assert(index == 0);

    int16_t retval = 0;

    switch (device) {
        case RETRO_DEVICE_JOYPAD:
            if (gamepad) {
                switch (id) {
                    case RETRO_DEVICE_ID_JOYPAD_UP: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_DOWN: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_LEFT: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_RIGHT: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_A: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_B: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_X: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_Y: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_SELECT: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_START: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_L: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_L2: return (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 20000) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_L3: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_R: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_R2: return (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 20000) ? 1 : 0;
                    case RETRO_DEVICE_ID_JOYPAD_R3: return SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK) ? 1 : 0;
                        break;
                }
            }
            break;

        case RETRO_DEVICE_MOUSE: {
            switch (id) {
                case RETRO_DEVICE_ID_MOUSE_X: retval = (int16_t) (current_mouse_x - prev_mouse_x); prev_mouse_x = current_mouse_x; break;
                case RETRO_DEVICE_ID_MOUSE_Y: retval = (int16_t) (current_mouse_y - prev_mouse_y); prev_mouse_y = current_mouse_y; break;
                case RETRO_DEVICE_ID_MOUSE_LEFT: retval = (current_mouse_buttons & SDL_BUTTON_LMASK) ? 1 : 0; break;
                case RETRO_DEVICE_ID_MOUSE_WHEELUP: if (mouse_wheel_accumulator > 0) { retval = (int16_t) mouse_wheel_accumulator; mouse_wheel_accumulator = 0; } break;
                case RETRO_DEVICE_ID_MOUSE_WHEELDOWN: if (mouse_wheel_accumulator < 0) { retval = (int16_t) -mouse_wheel_accumulator; mouse_wheel_accumulator = 0; } break;
                    break;
            }
            break;
        }

        case RETRO_DEVICE_POINTER:
            switch (id) {
                case RETRO_DEVICE_ID_POINTER_X:
                case RETRO_DEVICE_ID_POINTER_Y:
                    // !!! FIXME: implement me.
                    break;
            }
            break;
    }

    return retval;
}

static void RETRO_CALLCONV input_poll_entry_point(void)
{
    // !!! FIXME: poll devices, decide if there was a state change.
}

static void RETRO_CALLCONV video_refresh_entry_point(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (data != NULL) {  // NULL==duplicate frame, don't update the existing texture. We'll draw the next frame with what we already have.
        const SDL_Rect rect = { 0, 0, width, height };
        SDL_UpdateTexture(texture, &rect, data, (int) pitch);
    }
}

static void load_game(const char *gamefname)
{
    size_t gamedatalen = 0;
    void *gamedata = SDL_LoadFile(gamefname, &gamedatalen);
    if (!gamedata) {
        panic("Couldn't load game file", SDL_GetError());
        return;
    }

    game_loaded = false;

    if (!retro_init_called) {
        retro_init();
        retro_init_called = true;
    }

    struct retro_game_info gameinfo;
    SDL_zero(gameinfo);
    gameinfo.path = gamefname;
    gameinfo.data = gamedata;
    gameinfo.size = gamedatalen;
    if (retro_load_game(&gameinfo)) {
        game_loaded = true;
    } else {
        panic("Couldn't load game file", "Game data is apparently corrupt/invalid");
    }
    SDL_free(gamedata);
}


SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    retro_api_version();

    struct retro_system_info sysinfo;
    SDL_zero(sysinfo);
    retro_get_system_info(&sysinfo);  // strictly speaking, this is called in the wrong sequence verses how libretro normally does it.

    SDL_SetAppMetadata("MojoZork SDL3", sysinfo.library_version ? sysinfo.library_version : "0.1", "org.icculus.mojozork");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        return panic("SDL_Init failed!", SDL_GetError());
    }

    const char *gamefname = NULL;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if ((arg[0] == '-') && (arg[1] == '-')) {  // command line option?
            arg += 2;
            if (SDL_strncmp(arg, "style=", 6) == 0) {
                style = arg + 6;
            } else {
                return usage();
            }
        } else if (gamefname == NULL) {
            gamefname = arg;
        } else {
            return usage();
        }
    }

    bool valid_style = false;
    if (style) {
        for (int i = 0; i < SDL_arraysize(visual_styles); i++) {
            if (SDL_strcasecmp(style, visual_styles[i]) == 0) {
                style = visual_styles[i];
                valid_style = true;
                break;
            }
        }
    }

    if (!valid_style) {
        style = visual_styles[0];
    }

    // no audio at the moment.
    //retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
    //retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
    retro_set_input_poll(input_poll_entry_point);
    retro_set_input_state(input_state_entry_point);
    retro_set_video_refresh(video_refresh_entry_point);
    retro_set_environment(environment_entry_point);

    if (gamefname) {
        load_game(gamefname);
    }

    struct retro_system_av_info avinfo;
    SDL_zero(avinfo);
    retro_get_system_av_info(&avinfo);

    if (!SDL_CreateWindowAndRenderer("MojoZork", avinfo.geometry.base_width, avinfo.geometry.base_height, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        return panic("Couldn't create window/renderer", SDL_GetError());
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, avinfo.geometry.base_width, avinfo.geometry.base_height);
    if (!texture) {
        return panic("Couldn't create texture", SDL_GetError());
    }

    SDL_StartTextInput(window);  // assume a keyboard until otherwise proven.
    SDL_SetRenderLogicalPresentation(renderer, avinfo.geometry.base_width, avinfo.geometry.base_height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    return SDL_APP_CONTINUE;
}

static void SDLCALL open_on_main_thread(void *userdata)
{
    if (userdata) {
        load_game((const char *) userdata);
        SDL_free(userdata);
    }
}

static void SDLCALL file_open_callback(void *userdata, const char * const *filelist, int filter)
{
    if (!filelist || !*filelist) {
        return;  // cancelled or error, ignore it.
    }
    SDL_RunOnMainThread(open_on_main_thread, SDL_strdup(*filelist), false);
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;  /* end the program, reporting success to the OS. */
    } else if ( ((event->type == SDL_EVENT_KEY_UP) || (event->type == SDL_EVENT_KEY_DOWN)) && keyboard_event_impl ) {
        if ((event->key.key == SDLK_F3) && event->key.down) {
            SDL_ShowOpenFileDialog(file_open_callback, NULL, window, NULL, 0, NULL, false);
            return SDL_APP_CONTINUE;
        } else if ((event->key.key == SDLK_F6) && event->key.down) {
            for (int i = 0; i < SDL_arraysize(visual_styles); i++) {
                if (SDL_strcmp(style, visual_styles[i]) == 0) {
                    style = visual_styles[(i + 1) % SDL_arraysize(visual_styles)];
                    visual_style_updated = true;
                    return SDL_APP_CONTINUE;
                }
            }
            // uh...shouldn't have gotten here...?
            style = visual_styles[0];
            visual_style_updated = true;
            return SDL_APP_CONTINUE;
        }

        // we only care about an extremely small set of keys for MojoZork, and we're not going to use text input events atm. It's 1980s input tech!
        unsigned int keycode = 0;
        switch (event->key.key) {
            case SDLK_UP: keycode = RETROK_UP; break;
            case SDLK_DOWN: keycode = RETROK_DOWN; break;
            case SDLK_PAGEUP: keycode = RETROK_PAGEUP; break;
            case SDLK_PAGEDOWN: keycode = RETROK_PAGEDOWN; break;
            case SDLK_BACKSPACE: keycode = RETROK_BACKSPACE; break;
            case SDLK_KP_ENTER: keycode = RETROK_KP_ENTER; break;
            case SDLK_RETURN: keycode = RETROK_RETURN; break;
            default: break;
        }
        if (keycode) {
            keyboard_event_impl(event->key.down, keycode, 0, 0);
        }
    } else if ((event->type == SDL_EVENT_TEXT_INPUT) && keyboard_event_impl) {
        const char *text = event->text.text;
        Uint32 ch;

        while ((ch = SDL_StepUTF8(&text, NULL)) != 0) {
            if ((ch >= 32) && (ch <= 127)) {
                keyboard_event_impl(true, 0, (unsigned int) ch, 0);
                keyboard_event_impl(false, 0, (unsigned int) ch, 0);
            }
        }
    } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        if (event->wheel.integer_y) {
            mouse_wheel_accumulator += event->wheel.integer_y;
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        if (!gamepad) {
            gamepad = SDL_OpenGamepad(event->gdevice.which);
            if (!gamepad) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open newly-connected gamepad: %s", SDL_GetError());
            }
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        if (gamepad && (SDL_GetGamepadID(gamepad) == event->gdevice.which)) {
            SDL_CloseGamepad(gamepad);
            gamepad = NULL;
        }
    } else if (event->type == SDL_EVENT_DROP_FILE) {
        load_game((const char *) event->drop.data);
    }

    return SDL_APP_CONTINUE;  /* carry on with the program! */
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    static Uint64 last_iteration_ns = 0;
    const Uint64 now_ns = SDL_GetTicksNS();

    if (frame_time_callback_impl) {
        const Uint64 diff_ns = last_iteration_ns ? (now_ns - last_iteration_ns) : 0;
        frame_time_callback_impl((retro_usec_t) (diff_ns / 1000));
    }
    last_iteration_ns = now_ns;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (game_loaded) {
        float fx, fy;
        current_mouse_buttons = SDL_GetMouseState(&fx, &fy);
        current_mouse_x = (int) fx;
        current_mouse_y = (int) fy;
        retro_run();
        SDL_RenderTexture(renderer, texture, NULL, NULL);
    } else {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        const char *msg = "Press F3 to load a Z-Machine program. F6 to toggle visual styles.";
        const float y = (480 - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) / 2.0f;
        const float x  = (640 - (SDL_strlen(msg) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE)) / 2.0f;
        SDL_RenderDebugText(renderer, x, y, msg);
    }

    SDL_RenderPresent(renderer);

    return SDL_APP_CONTINUE;  /* carry on with the program! */
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    if (retro_init_called) {
        retro_deinit();
    }
    SDL_CloseGamepad(gamepad);
    SDL_DestroyTexture(texture);
}

// end of mojozork-sdl.c ...

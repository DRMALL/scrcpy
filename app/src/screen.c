#include "screen.h"

#include <SDL2/SDL.h>
#include <string.h>

#include "icon.xpm"
#include "lockutil.h"
#include "tinyxpm.h"

#define DISPLAY_MARGINS 96

SDL_bool sdl_init_and_configure(void) {
    if (SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Could not initialize SDL: %s", SDL_GetError());
        return SDL_FALSE;
    }

    // FIXME it may crash in SDL_Quit in i965_dri.so
    // As a workaround, do not call SDL_Quit() (we are exiting anyway).
    // atexit(SDL_Quit);

    // Bilinear resizing
    if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not enable bilinear filtering");
    }

#if SDL_VERSION_ATLEAST(2, 0, 5)
    // Handle a click to gain focus as any other click
    if (!SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1")) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not enable mouse focus clickthrough");
    }
#endif

    return SDL_TRUE;
}

// get the window size in a struct size
static struct size get_native_window_size(SDL_Window *window) {
    int width;
    int height;
    SDL_GetWindowSize(window, &width, &height);

    struct size size;
    size.width = width;
    size.height = height;
    return size;
}

// get the windowed window size
static struct size get_window_size(const struct screen *screen) {
    if (screen->fullscreen) {
        return screen->windowed_window_size;
    }
    return get_native_window_size(screen->window);
}

// set the window size to be applied when fullscreen is disabled
static void set_window_size(struct screen *screen, struct size new_size) {
    // setting the window size during fullscreen is implementation defined,
    // so apply the resize only after fullscreen is disabled
    if (screen->fullscreen) {
        // SDL_SetWindowSize will be called when fullscreen will be disabled
        screen->windowed_window_size = new_size;
    } else {
        SDL_SetWindowSize(screen->window, new_size.width, new_size.height);
    }
}

// get the preferred display bounds (i.e. the screen bounds with some margins)
static SDL_bool get_preferred_display_bounds(struct size *bounds) {
    SDL_Rect rect;
#if SDL_VERSION_ATLEAST(2, 0, 5)
# define GET_DISPLAY_BOUNDS(i, r) SDL_GetDisplayUsableBounds((i), (r))
#else
# define GET_DISPLAY_BOUNDS(i, r) SDL_GetDisplayBounds((i), (r))
#endif
    if (GET_DISPLAY_BOUNDS(0, &rect)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_SYSTEM, "Could not get display usable bounds: %s", SDL_GetError());
        return SDL_FALSE;
    }

    bounds->width = MAX(0, rect.w - DISPLAY_MARGINS);
    bounds->height = MAX(0, rect.h - DISPLAY_MARGINS);
    return SDL_TRUE;
}

// return the optimal size of the window, with the following constraints:
//  - it attempts to keep at least one dimension of the current_size (i.e. it crops the black borders)
//  - it keeps the aspect ratio
//  - it scales down to make it fit in the display_size
static struct size get_optimal_size(struct size current_size, struct size frame_size) {
    struct size display_size;
    // 32 bits because we need to multiply two 16 bits values
    Uint32 w;
    Uint32 h;

    if (!get_preferred_display_bounds(&display_size)) {
        // cannot get display bounds, do not constraint the size
        w = current_size.width;
        h = current_size.height;
    } else {
        w = MIN(current_size.width, display_size.width);
        h = MIN(current_size.height, display_size.height);
    }

    SDL_bool keep_width = frame_size.width * h > frame_size.height * w;
    if (keep_width) {
        // remove black borders on top and bottom
        h = frame_size.height * w / frame_size.width;
    } else {
        // remove black borders on left and right (or none at all if it already fits)
        w = frame_size.width * h / frame_size.height;
    }

    // w and h must fit into 16 bits
    SDL_assert_release(w < 0x10000 && h < 0x10000);
    return (struct size) {w, h};
}

// same as get_optimal_size(), but read the current size from the window
static inline struct size get_optimal_window_size(const struct screen *screen, struct size frame_size) {
    struct size current_size = get_window_size(screen);
    return get_optimal_size(current_size, frame_size);
}

// initially, there is no current size, so use the frame size as current size
static inline struct size get_initial_optimal_size(struct size frame_size) {
    return get_optimal_size(frame_size, frame_size);
}

void screen_init(struct screen *screen) {
    *screen = (struct screen) SCREEN_INITIALIZER;
}

SDL_bool screen_init_rendering(struct screen *screen, const char *device_name, struct size frame_size) {
    screen->frame_size = frame_size;

    struct size window_size = get_initial_optimal_size(frame_size);
    screen->window = SDL_CreateWindow(device_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                      window_size.width, window_size.height, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    if (!screen->window) {
        SDL_LogCritical(SDL_LOG_CATEGORY_SYSTEM, "Could not create window: %s", SDL_GetError());
        return SDL_FALSE;
    }

    screen->renderer = SDL_CreateRenderer(screen->window, -1, SDL_RENDERER_ACCELERATED);
    if (!screen->renderer) {
        SDL_LogCritical(SDL_LOG_CATEGORY_RENDER, "Could not create renderer: %s", SDL_GetError());
        screen_destroy(screen);
        return SDL_FALSE;
    }

    if (SDL_RenderSetLogicalSize(screen->renderer, frame_size.width, frame_size.height)) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not set renderer logical size: %s", SDL_GetError());
        screen_destroy(screen);
        return SDL_FALSE;
    }

    SDL_Surface *icon = read_xpm(icon_xpm);
    if (!icon) {
        SDL_LogError(SDL_LOG_CATEGORY_SYSTEM, "Could not load icon: %s", SDL_GetError());
        screen_destroy(screen);
        return SDL_FALSE;
    }
    SDL_SetWindowIcon(screen->window, icon);
    SDL_FreeSurface(icon);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Initial texture: %" PRIu16 "x%" PRIu16, frame_size.width, frame_size.height);
    screen->texture = SDL_CreateTexture(screen->renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
                                        frame_size.width, frame_size.height);
    if (!screen->texture) {
        SDL_LogCritical(SDL_LOG_CATEGORY_RENDER, "Could not create texture: %s", SDL_GetError());
        screen_destroy(screen);
        return SDL_FALSE;
    }

    screen_render(screen);
    return SDL_TRUE;
}

void screen_show_window(struct screen *screen) {
    SDL_ShowWindow(screen->window);
}

void screen_destroy(struct screen *screen) {
    if (screen->texture) {
        SDL_DestroyTexture(screen->texture);
    }
    if (screen->renderer) {
        // FIXME it may crash at exit if we destroy the renderer or the window,
        // with the exact same stack trace as <https://bugs.launchpad.net/mir/+bug/1466535>.
        // As a workaround, leak the renderer and the window (we are exiting anyway).
        //SDL_DestroyRenderer(screen->renderer);
    }
    if (screen->window) {
        //SDL_DestroyWindow(screen->window);
        // at least we hide it
        SDL_HideWindow(screen->window);
    }
}

// recreate the texture and resize the window if the frame size has changed
static SDL_bool prepare_for_frame(struct screen *screen, struct size new_frame_size) {
    if (screen->frame_size.width != new_frame_size.width || screen->frame_size.height != new_frame_size.height) {
        if (SDL_RenderSetLogicalSize(screen->renderer, new_frame_size.width, new_frame_size.height)) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Could not set renderer logical size: %s", SDL_GetError());
            return SDL_FALSE;
        }

        // frame dimension changed, destroy texture
        SDL_DestroyTexture(screen->texture);

        struct size current_size = get_window_size(screen);
        struct size target_size = {
            (Uint32) current_size.width * new_frame_size.width / screen->frame_size.width,
            (Uint32) current_size.height * new_frame_size.height / screen->frame_size.height,
        };
        target_size = get_optimal_size(target_size, new_frame_size);
        set_window_size(screen, target_size);

        screen->frame_size = new_frame_size;

        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "New texture: %" PRIu16 "x%" PRIu16,
                     screen->frame_size.width, screen->frame_size.height);
        screen->texture = SDL_CreateTexture(screen->renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
                                            new_frame_size.width, new_frame_size.height);
        if (!screen->texture) {
            SDL_LogCritical(SDL_LOG_CATEGORY_RENDER, "Could not create texture: %s", SDL_GetError());
            return SDL_FALSE;
        }
    }

    return SDL_TRUE;
}

// write the frame into the texture
static void update_texture(struct screen *screen, const AVFrame *frame) {
    SDL_UpdateYUVTexture(screen->texture, NULL,
            frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1],
            frame->data[2], frame->linesize[2]);
}

SDL_bool screen_update_frame(struct screen *screen, struct frames *frames) {
    mutex_lock(frames->mutex);
    const AVFrame *frame = frames_consume_rendered_frame(frames);
    struct size new_frame_size = {frame->width, frame->height};
    if (!prepare_for_frame(screen, new_frame_size)) {
        mutex_unlock(frames->mutex);
        return SDL_FALSE;
    }
    update_texture(screen, frame);
    mutex_unlock(frames->mutex);

    screen_render(screen);
    return SDL_TRUE;
}

void screen_render(struct screen *screen) {
    SDL_RenderClear(screen->renderer);
    SDL_RenderCopy(screen->renderer, screen->texture, NULL, NULL);
    SDL_RenderPresent(screen->renderer);
}

void screen_switch_fullscreen(struct screen *screen) {
    if (!screen->fullscreen) {
        // going to fullscreen, store the current windowed window size
        screen->windowed_window_size = get_native_window_size(screen->window);
    }
    Uint32 new_mode = screen->fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (SDL_SetWindowFullscreen(screen->window, new_mode)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Could not switch fullscreen mode: %s", SDL_GetError());
        return;
    }

    screen->fullscreen = !screen->fullscreen;
    if (!screen->fullscreen) {
        // fullscreen disabled, restore expected windowed window size
        SDL_SetWindowSize(screen->window, screen->windowed_window_size.width, screen->windowed_window_size.height);
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Switched to %s mode", screen->fullscreen ? "fullscreen" : "windowed");
    screen_render(screen);
}

void screen_resize_to_fit(struct screen *screen) {
    if (!screen->fullscreen) {
        struct size optimal_size = get_optimal_window_size(screen, screen->frame_size);
        SDL_SetWindowSize(screen->window, optimal_size.width, optimal_size.height);
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Resized to optimal size");
    }
}

void screen_resize_to_pixel_perfect(struct screen *screen) {
    if (!screen->fullscreen) {
        SDL_SetWindowSize(screen->window, screen->frame_size.width, screen->frame_size.height);
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "Resized to pixel-perfect");
    }
}
/*
 * bs_keys_native.c - Keystroke abstraction for native Linux (SDL2).
 *
 * SDL2 is already initialised by bs_gfx_init().
 * Key events come from the SDL display window; stdin is left entirely
 * for the konsole (UART simulation).
 *
 * QUIT events (window close) are handled here via exit(); they are also
 * caught in bs_gfx_present() during boot when keys are not polled.
 */
#ifdef BS_KEYS_NATIVE

#include "bs/bs_keys.h"
#include <SDL2/SDL.h>
#include <stdlib.h>

void bs_keys_init(const bs_arch_t* arch) {
    (void)arch;  /* SDL already running from bs_gfx_init */
}

bool bs_keys_poll(bs_key_t* out) {
    if (!out) return false;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        if (e.type != SDL_KEYDOWN) continue;

        out->ch = 0;
        switch (e.key.keysym.sym) {
            case SDLK_UP:        out->id = BS_KEY_UP;    return true;
            case SDLK_DOWN:      out->id = BS_KEY_DOWN;  return true;
            case SDLK_LEFT:      out->id = BS_KEY_LEFT;  return true;
            case SDLK_RIGHT:     out->id = BS_KEY_RIGHT; return true;
            case SDLK_RETURN:    out->id = BS_KEY_ENTER; return true;
            case SDLK_KP_ENTER:  out->id = BS_KEY_ENTER; return true;
            case SDLK_BACKSPACE: out->id = BS_KEY_BACK;  return true;
            case SDLK_ESCAPE:    out->id = BS_KEY_ESC;   return true;
            default: {
                SDL_Keycode sym = e.key.keysym.sym;
                if (sym >= SDLK_SPACE && sym < 0x7F) {
                    out->id = BS_KEY_CHAR;
                    out->ch = (char)sym;
                    return true;
                }
            }
        }
    }
    return false;
}

#endif /* BS_KEYS_NATIVE */

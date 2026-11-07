// keymap.h - SDL key symbols -> Doom key codes (doomkeys.h), following doomgeneric_sdl.c.
#pragma once
#include <SDL_keycode.h>
#include <cstdint>

namespace vboard {

// Returns the Doom key code for an SDL keysym, or 0 if the key has no Doom meaning.
uint8_t sdl_to_doom_key(SDL_Keycode sym);

}  // namespace vboard

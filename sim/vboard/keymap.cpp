#include "keymap.h"

namespace vboard {

// Doom key codes (third_party/doomgeneric/doomgeneric/doomkeys.h).
namespace {
constexpr uint8_t K_RIGHT = 0xae, K_LEFT = 0xac, K_UP = 0xad, K_DOWN = 0xaf, K_USE = 0xa2, K_FIRE = 0xa3,
                  K_ESC = 27, K_ENTER = 13, K_TAB = 9, K_BACKSPACE = 0x7f, K_PAUSE = 0xff,
                  K_RSHIFT = 0x80 + 0x36, K_RALT = 0x80 + 0x38,
                  K_HOME = 0x80 + 0x47, K_END = 0x80 + 0x4f, K_PGUP = 0x80 + 0x49, K_PGDN = 0x80 + 0x51,
                  K_INS = 0x80 + 0x52, K_DEL = 0x80 + 0x53;
}

uint8_t sdl_to_doom_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return K_ENTER;
    case SDLK_ESCAPE: return K_ESC;
    case SDLK_LEFT: case SDLK_KP_4: return K_LEFT;
    case SDLK_RIGHT: case SDLK_KP_6: return K_RIGHT;
    case SDLK_UP: case SDLK_KP_8: return K_UP;
    case SDLK_DOWN: case SDLK_KP_2: return K_DOWN;
    case SDLK_LCTRL: case SDLK_RCTRL: return K_FIRE;
    case SDLK_SPACE: return K_USE;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return K_RSHIFT;
    case SDLK_LALT: case SDLK_RALT: return K_RALT;
    case SDLK_TAB: return K_TAB;
    case SDLK_BACKSPACE: return K_BACKSPACE;
    case SDLK_PAUSE: return K_PAUSE;
    case SDLK_HOME: case SDLK_KP_7: return K_HOME;
    case SDLK_END: case SDLK_KP_1: return K_END;
    case SDLK_PAGEUP: case SDLK_KP_9: return K_PGUP;
    case SDLK_PAGEDOWN: case SDLK_KP_3: return K_PGDN;
    case SDLK_INSERT: return K_INS;
    case SDLK_DELETE: return K_DEL;
    case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_EQUALS: return '=';
    case SDLK_MINUS: case SDLK_KP_MINUS: return '-';
    case SDLK_KP_0: return '0';
    case SDLK_KP_5: return '5';
    case SDLK_KP_DIVIDE: return '/';
    case SDLK_KP_PLUS: return '+';
    case SDLK_KP_MULTIPLY: return '*';
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F10) return uint8_t(0x80 + 0x3b + (k - SDLK_F1));
    if (k == SDLK_F11) return 0x80 + 0x57;
    if (k == SDLK_F12) return 0x80 + 0x58;
    if (k >= 0x21 && k <= 0x7e) {                 // printable ASCII: digits, letters (SDL gives lower case), punctuation
        if (k >= 'A' && k <= 'Z') return uint8_t(k - 'A' + 'a');
        return uint8_t(k);
    }
    return 0;
}

}  // namespace vboard

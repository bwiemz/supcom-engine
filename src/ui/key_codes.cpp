#include "ui/key_codes.hpp"

#include <GLFW/glfw3.h>

namespace osc::ui {

namespace {

// wxWidgets 2.4's codes, as Moho's EMauiKeyCode lists them (faf-re).
constexpr i32 kWxBack = 8;
constexpr i32 kWxTab = 9;
constexpr i32 kWxReturn = 13;
constexpr i32 kWxEscape = 27;
constexpr i32 kWxDelete = 127;
constexpr i32 kWxShift = 306;
constexpr i32 kWxAlt = 307;
constexpr i32 kWxControl = 308;
constexpr i32 kWxMenu = 309;
constexpr i32 kWxPause = 310;
constexpr i32 kWxCapital = 311;
constexpr i32 kWxPrior = 312;
constexpr i32 kWxNext = 313;
constexpr i32 kWxEnd = 314;
constexpr i32 kWxHome = 315;
constexpr i32 kWxLeft = 316;
constexpr i32 kWxUp = 317;
constexpr i32 kWxRight = 318;
constexpr i32 kWxDown = 319;
constexpr i32 kWxSnapshot = 323;
constexpr i32 kWxInsert = 324;
constexpr i32 kWxNumpad0 = 326;
constexpr i32 kWxMultiply = 336;
constexpr i32 kWxAdd = 337;
constexpr i32 kWxSubtract = 339;
constexpr i32 kWxDecimal = 340;
constexpr i32 kWxDivide = 341;
constexpr i32 kWxF1 = 342;
constexpr i32 kWxNumlock = 366;
constexpr i32 kWxScroll = 367;
constexpr i32 kWxNumpadEqual = 390;

} // namespace

i32 moho_key_code(i32 glfw_key) {
    // GLFW's printable keys are their (unshifted, upper case) ASCII codes,
    // as wx's are.
    if (glfw_key >= GLFW_KEY_SPACE && glfw_key <= GLFW_KEY_GRAVE_ACCENT) return glfw_key;
    if (glfw_key >= GLFW_KEY_F1 && glfw_key <= GLFW_KEY_F24)
        return kWxF1 + (glfw_key - GLFW_KEY_F1);
    if (glfw_key >= GLFW_KEY_KP_0 && glfw_key <= GLFW_KEY_KP_9)
        return kWxNumpad0 + (glfw_key - GLFW_KEY_KP_0);
    switch (glfw_key) {
    case GLFW_KEY_ESCAPE: return kWxEscape;
    case GLFW_KEY_ENTER: return kWxReturn;
    case GLFW_KEY_KP_ENTER: return kWxReturn; // Windows reports it as Return
    case GLFW_KEY_TAB: return kWxTab;
    case GLFW_KEY_BACKSPACE: return kWxBack;
    case GLFW_KEY_INSERT: return kWxInsert;
    case GLFW_KEY_DELETE: return kWxDelete;
    case GLFW_KEY_RIGHT: return kWxRight;
    case GLFW_KEY_LEFT: return kWxLeft;
    case GLFW_KEY_DOWN: return kWxDown;
    case GLFW_KEY_UP: return kWxUp;
    case GLFW_KEY_PAGE_UP: return kWxPrior;
    case GLFW_KEY_PAGE_DOWN: return kWxNext;
    case GLFW_KEY_HOME: return kWxHome;
    case GLFW_KEY_END: return kWxEnd;
    case GLFW_KEY_CAPS_LOCK: return kWxCapital;
    case GLFW_KEY_SCROLL_LOCK: return kWxScroll;
    case GLFW_KEY_NUM_LOCK: return kWxNumlock;
    case GLFW_KEY_PRINT_SCREEN: return kWxSnapshot;
    case GLFW_KEY_PAUSE: return kWxPause;
    case GLFW_KEY_KP_DECIMAL: return kWxDecimal;
    case GLFW_KEY_KP_DIVIDE: return kWxDivide;
    case GLFW_KEY_KP_MULTIPLY: return kWxMultiply;
    case GLFW_KEY_KP_SUBTRACT: return kWxSubtract;
    case GLFW_KEY_KP_ADD: return kWxAdd;
    case GLFW_KEY_KP_EQUAL: return kWxNumpadEqual;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT: return kWxShift;
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL: return kWxControl;
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT: return kWxAlt;
    case GLFW_KEY_MENU: return kWxMenu;
    default: return 0; // the Windows keys and the rest: wx 2.4 has no code
    }
}

i32 windows_key_code(i32 glfw_key) {
    // Letters, digits and space share their codes with Windows.
    if ((glfw_key >= GLFW_KEY_A && glfw_key <= GLFW_KEY_Z) ||
        (glfw_key >= GLFW_KEY_0 && glfw_key <= GLFW_KEY_9) || glfw_key == GLFW_KEY_SPACE)
        return glfw_key;
    if (glfw_key >= GLFW_KEY_F1 && glfw_key <= GLFW_KEY_F24) return 0x70 + (glfw_key - GLFW_KEY_F1);
    if (glfw_key >= GLFW_KEY_KP_0 && glfw_key <= GLFW_KEY_KP_9)
        return 0x60 + (glfw_key - GLFW_KEY_KP_0);
    switch (glfw_key) {
    case GLFW_KEY_APOSTROPHE: return 0xDE;
    case GLFW_KEY_COMMA: return 0xBC;
    case GLFW_KEY_MINUS: return 0xBD;
    case GLFW_KEY_PERIOD: return 0xBE;
    case GLFW_KEY_SLASH: return 0xBF;
    case GLFW_KEY_SEMICOLON: return 0xBA;
    case GLFW_KEY_EQUAL: return 0xBB;
    case GLFW_KEY_LEFT_BRACKET: return 0xDB;
    case GLFW_KEY_BACKSLASH: return 0xDC;
    case GLFW_KEY_RIGHT_BRACKET: return 0xDD;
    case GLFW_KEY_GRAVE_ACCENT: return 0xC0;
    case GLFW_KEY_WORLD_1: return 0xE2;
    case GLFW_KEY_ESCAPE: return 0x1B;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER: return 0x0D;
    case GLFW_KEY_TAB: return 0x09;
    case GLFW_KEY_BACKSPACE: return 0x08;
    case GLFW_KEY_INSERT: return 0x2D;
    case GLFW_KEY_DELETE: return 0x2E;
    case GLFW_KEY_RIGHT: return 0x27;
    case GLFW_KEY_LEFT: return 0x25;
    case GLFW_KEY_DOWN: return 0x28;
    case GLFW_KEY_UP: return 0x26;
    case GLFW_KEY_PAGE_UP: return 0x21;
    case GLFW_KEY_PAGE_DOWN: return 0x22;
    case GLFW_KEY_HOME: return 0x24;
    case GLFW_KEY_END: return 0x23;
    case GLFW_KEY_CAPS_LOCK: return 0x14;
    case GLFW_KEY_SCROLL_LOCK: return 0x91;
    case GLFW_KEY_NUM_LOCK: return 0x90;
    case GLFW_KEY_PRINT_SCREEN: return 0x2C;
    case GLFW_KEY_PAUSE: return 0x13;
    case GLFW_KEY_KP_DECIMAL: return 0x6E;
    case GLFW_KEY_KP_DIVIDE: return 0x6F;
    case GLFW_KEY_KP_MULTIPLY: return 0x6A;
    case GLFW_KEY_KP_SUBTRACT: return 0x6D;
    case GLFW_KEY_KP_ADD: return 0x6B;
    // A key message names the generic modifier, not its side.
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT: return 0x10;
    case GLFW_KEY_LEFT_CONTROL:
    case GLFW_KEY_RIGHT_CONTROL: return 0x11;
    case GLFW_KEY_LEFT_ALT:
    case GLFW_KEY_RIGHT_ALT: return 0x12;
    case GLFW_KEY_LEFT_SUPER: return 0x5B;
    case GLFW_KEY_RIGHT_SUPER: return 0x5C;
    case GLFW_KEY_MENU: return 0x5D;
    default: return 0;
    }
}

i32 moho_mouse_button(i32 glfw_button) {
    switch (glfw_button) {
    case GLFW_MOUSE_BUTTON_LEFT: return 1;
    case GLFW_MOUSE_BUTTON_MIDDLE: return 2;
    case GLFW_MOUSE_BUTTON_RIGHT: return 3;
    default: return 0;
    }
}

} // namespace osc::ui

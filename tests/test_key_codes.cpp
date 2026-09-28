#include <catch2/catch_test_macros.hpp>

#include "ui/key_codes.hpp"

#include <GLFW/glfw3.h>

using osc::ui::moho_key_code;
using osc::ui::moho_mouse_button;
using osc::ui::windows_key_code;

TEST_CASE("Keys reach Lua with wx's key codes, as Moho gives them", "[ui][input]") {
    // The codes retail's scripts compare against (UIUtil.VK_*, and the
    // F-keys and Delete some dialogs test by number).
    CHECK(moho_key_code(GLFW_KEY_ESCAPE) == 27);
    CHECK(moho_key_code(GLFW_KEY_ENTER) == 13);
    CHECK(moho_key_code(GLFW_KEY_KP_ENTER) == 13);
    CHECK(moho_key_code(GLFW_KEY_SPACE) == 32);
    CHECK(moho_key_code(GLFW_KEY_TAB) == 9);
    CHECK(moho_key_code(GLFW_KEY_BACKSPACE) == 8);
    CHECK(moho_key_code(GLFW_KEY_DELETE) == 127);
    CHECK(moho_key_code(GLFW_KEY_F1) == 342);
    CHECK(moho_key_code(GLFW_KEY_F3) == 344);
    CHECK(moho_key_code(GLFW_KEY_F11) == 352);
    CHECK(moho_key_code(GLFW_KEY_F24) == 365);
    CHECK(moho_key_code(GLFW_KEY_PAUSE) == 310);
    // Letters, digits and punctuation keep their ASCII code.
    CHECK(moho_key_code(GLFW_KEY_A) == 'A');
    CHECK(moho_key_code(GLFW_KEY_Z) == 'Z');
    CHECK(moho_key_code(GLFW_KEY_0) == '0');
    CHECK(moho_key_code(GLFW_KEY_MINUS) == '-');
    CHECK(moho_key_code(GLFW_KEY_GRAVE_ACCENT) == '`');
    // wx's own codes for the rest.
    CHECK(moho_key_code(GLFW_KEY_LEFT) == 316);
    CHECK(moho_key_code(GLFW_KEY_UP) == 317);
    CHECK(moho_key_code(GLFW_KEY_RIGHT) == 318);
    CHECK(moho_key_code(GLFW_KEY_DOWN) == 319);
    CHECK(moho_key_code(GLFW_KEY_PAGE_UP) == 312);
    CHECK(moho_key_code(GLFW_KEY_PAGE_DOWN) == 313);
    CHECK(moho_key_code(GLFW_KEY_END) == 314);
    CHECK(moho_key_code(GLFW_KEY_HOME) == 315);
    CHECK(moho_key_code(GLFW_KEY_INSERT) == 324);
    CHECK(moho_key_code(GLFW_KEY_KP_0) == 326);
    CHECK(moho_key_code(GLFW_KEY_KP_9) == 335);
    CHECK(moho_key_code(GLFW_KEY_KP_MULTIPLY) == 336);
    CHECK(moho_key_code(GLFW_KEY_KP_ADD) == 337);
    CHECK(moho_key_code(GLFW_KEY_KP_SUBTRACT) == 339);
    CHECK(moho_key_code(GLFW_KEY_KP_DECIMAL) == 340);
    CHECK(moho_key_code(GLFW_KEY_KP_DIVIDE) == 341);
    CHECK(moho_key_code(GLFW_KEY_LEFT_SHIFT) == 306);
    CHECK(moho_key_code(GLFW_KEY_RIGHT_SHIFT) == 306);
    CHECK(moho_key_code(GLFW_KEY_LEFT_ALT) == 307);
    CHECK(moho_key_code(GLFW_KEY_RIGHT_CONTROL) == 308);
    CHECK(moho_key_code(GLFW_KEY_CAPS_LOCK) == 311);
    CHECK(moho_key_code(GLFW_KEY_NUM_LOCK) == 366);
    CHECK(moho_key_code(GLFW_KEY_SCROLL_LOCK) == 367);
    // Keys wx 2.4 has no code for.
    CHECK(moho_key_code(GLFW_KEY_LEFT_SUPER) == 0);
    CHECK(moho_key_code(GLFW_KEY_F25) == 0);
    CHECK(moho_key_code(GLFW_KEY_UNKNOWN) == 0);
}

TEST_CASE("RawKeyCode is the Windows virtual-key code keyNames.lua names", "[ui][input]") {
    CHECK(windows_key_code(GLFW_KEY_ESCAPE) == 0x1B);
    CHECK(windows_key_code(GLFW_KEY_ENTER) == 0x0D);
    CHECK(windows_key_code(GLFW_KEY_A) == 0x41);
    CHECK(windows_key_code(GLFW_KEY_9) == 0x39);
    CHECK(windows_key_code(GLFW_KEY_SPACE) == 0x20);
    CHECK(windows_key_code(GLFW_KEY_F1) == 0x70);
    CHECK(windows_key_code(GLFW_KEY_F24) == 0x87);
    CHECK(windows_key_code(GLFW_KEY_KP_0) == 0x60);
    CHECK(windows_key_code(GLFW_KEY_KP_DIVIDE) == 0x6F);
    CHECK(windows_key_code(GLFW_KEY_LEFT) == 0x25);
    CHECK(windows_key_code(GLFW_KEY_DOWN) == 0x28);
    CHECK(windows_key_code(GLFW_KEY_DELETE) == 0x2E);
    CHECK(windows_key_code(GLFW_KEY_SEMICOLON) == 0xBA);
    CHECK(windows_key_code(GLFW_KEY_EQUAL) == 0xBB);
    CHECK(windows_key_code(GLFW_KEY_COMMA) == 0xBC);
    CHECK(windows_key_code(GLFW_KEY_MINUS) == 0xBD);
    CHECK(windows_key_code(GLFW_KEY_SLASH) == 0xBF);
    CHECK(windows_key_code(GLFW_KEY_GRAVE_ACCENT) == 0xC0);
    CHECK(windows_key_code(GLFW_KEY_LEFT_BRACKET) == 0xDB);
    CHECK(windows_key_code(GLFW_KEY_APOSTROPHE) == 0xDE);
    CHECK(windows_key_code(GLFW_KEY_RIGHT_SHIFT) == 0x10);
    CHECK(windows_key_code(GLFW_KEY_LEFT_CONTROL) == 0x11);
    CHECK(windows_key_code(GLFW_KEY_RIGHT_ALT) == 0x12);
    CHECK(windows_key_code(GLFW_KEY_LEFT_SUPER) == 0x5B);
    CHECK(windows_key_code(GLFW_KEY_UNKNOWN) == 0);
}

TEST_CASE("A mouse button's KeyCode is wx's button number", "[ui][input]") {
    CHECK(moho_mouse_button(GLFW_MOUSE_BUTTON_LEFT) == 1);
    CHECK(moho_mouse_button(GLFW_MOUSE_BUTTON_MIDDLE) == 2);
    CHECK(moho_mouse_button(GLFW_MOUSE_BUTTON_RIGHT) == 3);
    CHECK(moho_mouse_button(GLFW_MOUSE_BUTTON_4) == 0);
}

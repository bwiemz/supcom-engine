#include "sim/shield.hpp"

// Shield is a minimal Entity subclass — all complex logic (state machine,
// regeneration, damage absorption, energy management) lives in FA Lua
// (shield.lua). The C++ side just provides entity identity, health tracking,
// and a few moho method fields (is_on, size, shield_type).

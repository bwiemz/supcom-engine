# Lua 5.0.3 — patched with LuaPlus extensions for Supreme Commander compatibility
# Patches: != operator, continue statement, direct table iteration (for k,v in tbl do)

add_library(lua50 STATIC
    lapi.c
    lauxlib.c
    lbaselib.c
    lcode.c
    ldblib.c
    ldebug.c
    ldo.c
    ldump.c
    lfunc.c
    lgc.c
    liolib.c
    llex.c
    lmathlib.c
    lmem.c
    loadlib.c
    lobject.c
    lopcodes.c
    lparser.c
    lstate.c
    lstring.c
    lstrlib.c
    ltable.c
    ltablib.c
    ltm.c
    lundump.c
    lvm.c
    lzio.c
)

# Build as C
set_target_properties(lua50 PROPERTIES LINKER_LANGUAGE C)

# Public include directory so consumers can #include <lua.h>
target_include_directories(lua50 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Suppress common warnings in vendored code
if(MSVC)
    target_compile_options(lua50 PRIVATE /W0)
else()
    target_compile_options(lua50 PRIVATE -w)
endif()

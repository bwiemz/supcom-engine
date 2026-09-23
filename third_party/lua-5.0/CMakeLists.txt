# Lua 5.0.3 — patched with LuaPlus extensions for Supreme Commander compatibility
# Patches: != operator, continue statement, direct table iteration (for k,v in tbl do),
# '#' line comments

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

# Compile Lua as C++ so lua_error unwinds C++ binding frames with exceptions
# (destructors run) instead of longjmp, which skips them. See ldo.c. On MSVC
# this also needs /EHs (set in cmake/OscCompileOptions.cmake): the default
# /EHsc assumes extern "C" functions -- the whole Lua API -- never throw.
get_target_property(_lua50_sources lua50 SOURCES)
set_source_files_properties(${_lua50_sources} PROPERTIES LANGUAGE CXX)
set_target_properties(lua50 PROPERTIES LINKER_LANGUAGE CXX)

# Public include directory so consumers can #include <lua.h>
target_include_directories(lua50 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# lmathlib's transcendental functions (see the note in lmathlib.c).
target_link_libraries(lua50 PUBLIC osc_fdlibm)

# Suppress common warnings in vendored code
if(MSVC)
    target_compile_options(lua50 PRIVATE /W0)
else()
    target_compile_options(lua50 PRIVATE -w)
endif()

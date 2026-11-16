/*
** OpenSupCom: what the host keeps with a Lua state, shared by all its
** threads (lstate.h's global_State):
** - one pointer of its own (lua_sethostdata), as the engine keeps the sim a
**   state belongs to;
** - the keys its bindings read on nearly every call ("_c_object"...), each
**   interned once and kept (lua_pushhostkey), so a push hashes nothing.
**
** Apart from lua.h, so that a change here rebuilds only what uses it.
*/

#ifndef lhost_h
#define lhost_h

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many keys a state keeps */
#define LUA_HOSTKEYS	16

LUA_API void  lua_sethostdata (lua_State *L, void *p);
LUA_API void *lua_gethostdata (lua_State *L);

/* Push the string `s' as key `k' (0 <= k < LUA_HOSTKEYS): the state interns
   it on its first push and keeps it for its life (the collector marks it as
   a root), and later pushes reuse it. A key is always the same string. The
   value is the one lua_pushstring(L, s) pushes, and the push may collect as
   that one may. */
LUA_API void  lua_pushhostkey (lua_State *L, int k, const char *s);

#ifdef __cplusplus
}
#endif

#endif

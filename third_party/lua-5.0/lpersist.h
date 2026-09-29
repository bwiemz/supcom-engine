/*
** OpenSupCom: a Lua state's heap as bytes, and back (M208c).
**
** Everything the collector's mark starts from -- the registry, the main
** thread's globals, the per-type metatables, the frozen roots -- reaches,
** weakly held objects included, is written: tables in their exact layout
** (so they iterate as they did), closures, prototypes, upvalues (open ones
** with their thread), suspended coroutines with their call frames, frozen
** tables (lua_freeze) and the collector's modes. Garbage is not: the byte
** count a load leaves is its own, so a state whose collections matter
** collects only when told (lua_setmanualgc). C functions are written as
** their offset in this binary: a snapshot loads into the same build. Light
** userdata go through the caller's hooks, which name each address and find
** it again. A full userdata's bytes are written as they are.
**
** C++ only (the library is compiled as C++).
*/

#ifndef lpersist_h
#define lpersist_h

#include <cstdint>
#include <string>
#include <string_view>

struct lua_State;

struct lua_PersistHooks {
  /* Name a light userdata's address: false when it has none (the save
     fails). NULL is written without asking. */
  bool (*name)(void *ud, void *p, std::uint32_t *kind, std::uint64_t *id) = nullptr;
  /* The address a name gives now: false when it names nothing (the load
     fails). */
  bool (*find)(void *ud, std::uint32_t kind, std::uint64_t id, void **p) = nullptr;
  void *ud = nullptr;
};

/* Append L's heap to `out`. Empty on success, else what failed. L is the
   main thread, at rest: nothing on its stack, no call under way. It is not
   written itself (a load keeps its own). */
std::string lua_persist(lua_State *L, std::string &out, const lua_PersistHooks &hooks);

/* Replace L's heap (L the main thread, at rest) with a persisted one,
   written by this build. The old heap is collected. Empty on success, else
   what failed. A snapshot that is not one, or is corrupted, fails with the
   state as it was (what the load made is garbage); a failure after the new
   heap took over (out of memory) leaves a state to discard. */
std::string lua_unpersist(lua_State *L, std::string_view in, const lua_PersistHooks &hooks);

/* Rewrite a snapshot's hash after its bytes were changed on purpose (tests
   that damage one, to see a load refuse it). */
void lua_persist_seal(std::string &snapshot);

#endif

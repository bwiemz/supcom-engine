/*
** $Id: lgc.h,v 1.19a 2003/02/28 19:45:15 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#ifndef lgc_h
#define lgc_h


#include "lobject.h"


#define luaC_checkGC(L) { lua_assert(!(L->ci->state & CI_CALLING)); \
	if (G(L)->nblocks >= G(L)->GCthreshold) luaC_collectgarbage(L); }


/*
** OpenSupCom (M224g): frozen tables (lua_freeze). A frozen table is fixed
** (never collected, and not traversed by the mark), and lives off `rootgc'
** (not swept). One that holds anything but frozen tables and strings -- or
** is written after it froze -- is a frozen root: the mark traverses it every
** collection, so what it holds is marked.
*/
#define FROZENBIT	5  /* a frozen table */
#define FROZENROOTBIT	6  /* in `frozenroots' */
#define isfrozen(t)	((t)->marked & (1<<FROZENBIT))
#define luaC_frozenbarrier(L,t)	{ if (isfrozen(t)) luaC_frozenwrite(L,t); }


size_t luaC_separateudata (lua_State *L);
void luaC_callGCTM (lua_State *L);
void luaC_sweep (lua_State *L, int all);
void luaC_collectgarbage (lua_State *L);
int luaC_sweepstep (lua_State *L, int work);
void luaC_endsweep (lua_State *L);
void luaC_link (lua_State *L, GCObject *o, lu_byte tt);
void luaC_freeze (lua_State *L, Table *t);
void luaC_frozenwrite (lua_State *L, Table *t);


#endif

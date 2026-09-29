/*
** OpenSupCom: a Lua state's heap as bytes, and back (M208c). See lpersist.h
** and docs/plans/2026-09-29-m208c-state-snapshots-design.md.
**
** The stream: a header (the collector's modes), then every object's kind
** and sizes, so a load can make them all before filling any, then every
** object's contents, then the roots, then a hash of it all: a corrupted
** snapshot fails before anything is made. (A crafted one could still name
** any address as a C function; see the design's Risks.) Objects are numbered in the order a
** breadth-first walk from the roots meets them, so nothing recurses however
** deep the heap is.
**
** What is written is what the program can reach, through weak references
** too: a weakly held object stays until the next collection in both games.
** The rest is garbage, and a load doesn't make it. The collector's byte
** count therefore differs after a load, as it differs between builds and
** runs anyway; a state whose collections are game state collects only when
** told (lua_setmanualgc), as the sim does.
**
** A save may come in the middle of a lazy sweep (M224g). Everything it
** reads is reachable, so intact: only garbage is freed, and a closure keeps
** the slot its open upvalue names marked (lgc.c's traverseclosure).
*/

#define lpersist_c

#include "lpersist.h"

#include "lua.h"

#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr char kMagic[8] = {'O', 'S', 'C', 'L', 'U', 'A', '5', '0'};
constexpr std::uint32_t kVersion = 1;

// What an object is, in the stream.
enum Kind : std::uint8_t {
  K_STRING = 1,
  K_TABLE,
  K_LCLOSURE,
  K_CCLOSURE,
  K_PROTO,
  K_UPVAL,
  K_THREAD,
  K_USERDATA,
};

// What a value is, in the stream.
enum Tag : std::uint8_t {
  T_NIL = 0,
  T_FALSE,
  T_TRUE,
  T_NUMBER,
  T_OBJECT,     // an object, by its number
  T_LIGHT,      // a light userdata, by the hooks' (kind, id)
  T_NULLLIGHT,  // a NULL light userdata
  T_MAIN,       // the main thread
  T_DEADKEY,    // a removed table key whose object is gone (never matches)
};

// An object's flags, in the stream.
enum Flag : std::uint8_t {
  F_FIXED = 1,   // a string the collector never frees (luaS_fix)
  F_FROZEN = 1,  // a frozen table (lua_freeze)
};

// A full userdata's payload, in the stream.
enum Payload : std::uint8_t {
  U_RAW = 0,   // its bytes as they are
  U_IOFILE,    // an io library file: which standard stream it is
};

// liolib.c's FileHandle (its metatable is registry["FILE*"]). Its FILE*
// is the process's own, so a snapshot names the standard stream it is:
// 0 closed, 1 stdin, 2 stdout, 3 stderr; any other open file can't be
// saved.
struct IoFile {
  FILE *f;
  int ispipe;
};

// The io library's file metatable, if the state has one: found without
// making a string (a save changes nothing).
Table *io_file_meta(lua_State *L) {
  Table *reg = hvalue(registry(L));
  for (int i = 0; i < sizenode(reg); ++i) {
    Node *n = gnode(reg, i);
    if (ttisstring(gkey(n)) && ttistable(gval(n)) &&
        std::strcmp(getstr(tsvalue(gkey(n))), "FILE*") == 0)
      return hvalue(gval(n));
  }
  return nullptr;
}

bool is_io_file(lua_State *L, Table *io_meta, const Udata *u) {
  (void)L;
  return io_meta != nullptr && u->uv.metatable == io_meta && u->uv.len == sizeof(IoFile);
}

// A table whose keys all hash by value is written slot by slot and comes
// back in the same slots, iterating as it did; one with a key that hashes
// by address is written in its iteration order and inserted again. (Its
// other keys can't keep their slots either: an address key's new main
// position may hold a key out of its own, and ltable.c's newkey assumes no
// chain starts at such a slot.)
enum Layout : std::uint8_t { L_EXACT = 0, L_INSERT = 1 };

// A thread's stack, at most (Lua 5.0 sets no limit; this is far past any
// the sim reaches, and keeps a malformed snapshot from asking for gigabytes).
constexpr std::int32_t kMaxStack = 1 << 24;

// The snapshot's hash, written after it.
std::uint64_t snapshot_hash(std::string_view s) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  std::size_t i = 0;
  for (; i + 8 <= s.size(); i += 8) {
    std::uint64_t w;
    std::memcpy(&w, s.data() + i, sizeof w);
    h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 32;
  }
  for (; i < s.size(); ++i) h = (h ^ static_cast<unsigned char>(s[i])) * 0x100000001b3ull;
  return h;
}

// The anchor C functions are measured from: any function in this binary.
int anchor(lua_State *) { return 0; }

std::int64_t cfunc_offset(lua_CFunction f) {
  return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(f) -
                                   reinterpret_cast<std::intptr_t>(&anchor));
}

lua_CFunction cfunc_at(std::int64_t offset) {
  return reinterpret_cast<lua_CFunction>(reinterpret_cast<std::intptr_t>(&anchor) +
                                         static_cast<std::intptr_t>(offset));
}

// A key that hashes by the object's address rather than its value.
bool hashes_by_address(const TObject *k) {
  switch (ttype(k)) {
    case LUA_TNIL: case LUA_TBOOLEAN: case LUA_TNUMBER: case LUA_TSTRING:
      return false;
    default:
      return true;
  }
}

class Writer {
 public:
  explicit Writer(std::string &out) : out_(out) {}
  void u8(std::uint8_t v) { out_.push_back(static_cast<char>(v)); }
  void u16(std::uint16_t v) { raw(&v, sizeof v); }
  void u32(std::uint32_t v) { raw(&v, sizeof v); }
  void i32(std::int32_t v) { raw(&v, sizeof v); }
  void u64(std::uint64_t v) { raw(&v, sizeof v); }
  void i64(std::int64_t v) { raw(&v, sizeof v); }
  void num(lua_Number v) { raw(&v, sizeof v); }
  void raw(const void *p, std::size_t n) { out_.append(static_cast<const char *>(p), n); }

 private:
  std::string &out_;
};

class Reader {
 public:
  explicit Reader(std::string_view in) : in_(in) {}
  bool ok() const { return ok_; }
  std::uint8_t u8() { std::uint8_t v = 0; raw(&v, sizeof v); return v; }
  std::uint16_t u16() { std::uint16_t v = 0; raw(&v, sizeof v); return v; }
  std::uint32_t u32() { std::uint32_t v = 0; raw(&v, sizeof v); return v; }
  std::int32_t i32() { std::int32_t v = 0; raw(&v, sizeof v); return v; }
  std::uint64_t u64() { std::uint64_t v = 0; raw(&v, sizeof v); return v; }
  std::int64_t i64() { std::int64_t v = 0; raw(&v, sizeof v); return v; }
  lua_Number num() { lua_Number v = 0; raw(&v, sizeof v); return v; }
  void raw(void *p, std::size_t n) {
    if (!ok_ || in_.size() - pos_ < n) { ok_ = false; std::memset(p, 0, n); return; }
    std::memcpy(p, in_.data() + pos_, n);
    pos_ += n;
  }
  std::string_view take(std::size_t n) {
    if (!ok_ || in_.size() - pos_ < n) { ok_ = false; return {}; }
    std::string_view s = in_.substr(pos_, n);
    pos_ += n;
    return s;
  }
  bool at_end() const { return pos_ == in_.size(); }
  // What is left to read: a bound on any size the stream asks for.
  std::size_t left() const { return in_.size() - pos_; }

 private:
  std::string_view in_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

// ------------------------------------------------------------------ saving

class Persister {
 public:
  Persister(lua_State *L, std::string &out, const lua_PersistHooks &hooks)
      : L_(L), g_(G(L)), w_(out), hooks_(hooks) {}

  std::string run() {
    if (L_ != g_->mainthread) return "not the state's main thread";
    if (L_->ci != L_->base_ci || L_->top != L_->base || L_->openupval != nullptr)
      return "the main thread is not at rest (its stack holds values)";
    collect_strings();
    io_meta_ = io_file_meta(L_);
    // The roots, as the collector's mark has them (lgc.c's markroot).
    visit(registry(L_));
    visit(gt(L_));
    for (int i = 0; i <= NUM_TAGS; ++i)
      if (g_->mt[i] != nullptr) add(valtogco(g_->mt[i]));
    for (int i = 0; i < g_->nfrozenroots; ++i) add(valtogco(g_->frozenroots[i]));
    // Walk until nothing new appears: values reached through upvalues left
    // open on threads the walk doesn't reach join late.
    std::size_t walked = 0;
    for (;;) {
      for (; walked < objs_.size() && err_.empty(); ++walked) children(objs_[walked]);
      if (!err_.empty()) return err_;
      const std::size_t before = objs_.size();
      open_upvals();
      if (!err_.empty()) return err_;
      if (objs_.size() == before) break;
    }

    w_.raw(kMagic, sizeof kMagic);
    w_.u32(kVersion);
    w_.u32(static_cast<std::uint32_t>(sizeof(Instruction)));
    w_.u8(g_->lazysweep);
    w_.u8(g_->manualgc);
    w_.u32(static_cast<std::uint32_t>(objs_.size()));
    for (GCObject *o : objs_) header(o);
    for (GCObject *o : objs_) contents(o);
    if (!err_.empty()) return err_;
    value(registry(L_));
    value(gt(L_));
    for (int i = 0; i <= NUM_TAGS; ++i) object(g_->mt[i] ? valtogco(g_->mt[i]) : nullptr);
    w_.u32(static_cast<std::uint32_t>(g_->nfrozenroots));
    for (int i = 0; i < g_->nfrozenroots; ++i) object(valtogco(g_->frozenroots[i]));
    return err_;
  }

 private:
  // Every interned string: a removed key's string is written only if it
  // is still one of them (otherwise its memory is gone).
  void collect_strings() {
    for (int i = 0; i < g_->strt.size; ++i)
      for (GCObject *o = g_->strt.hash[i]; o != nullptr; o = o->gch.next) strings_.insert(o);
  }

  void add(GCObject *o) {
    if (o == nullptr || o == valtogco(g_->mainthread)) return;
    if (index_.emplace(o, static_cast<std::uint32_t>(objs_.size())).second) objs_.push_back(o);
  }
  void visit(const TObject *o) {
    if (iscollectable(o)) add(gcvalue(o));
  }

  // A removed key (its value nil): its object is written only when it is
  // certainly alive -- met otherwise, or an interned string.
  bool dead_key_alive(const TObject *k) const {
    if (!iscollectable(k)) return true;
    GCObject *o = gcvalue(k);
    if (ttisstring(k)) return strings_.count(o) != 0;
    return index_.count(o) != 0;
  }

  void children(GCObject *o) {
    switch (o->gch.tt) {
      case LUA_TSTRING:
        break;
      case LUA_TTABLE: {
        Table *h = gcotoh(o);
        if (h->metatable != hvalue(defaultmeta(L_))) add(valtogco(h->metatable));
        for (int i = 0; i < h->sizearray; ++i) visit(&h->array[i]);
        if (h->node != g_->dummynode) {
          for (int i = 0; i < sizenode(h); ++i) {
            Node *n = gnode(h, i);
            if (ttisnil(gval(n))) {
              // Removed: an interned string key is kept (a script making
              // that string again finds this node, as in the saved game).
              if (ttisstring(gkey(n)) && strings_.count(gcvalue(gkey(n)))) visit(gkey(n));
              continue;
            }
            visit(gkey(n));
            visit(gval(n));
          }
        }
        break;
      }
      case LUA_TFUNCTION: {
        Closure *cl = gcotocl(o);
        if (cl->c.isC) {
          for (int i = 0; i < cl->c.nupvalues; ++i) visit(&cl->c.upvalue[i]);
        } else {
          add(valtogco(cl->l.p));
          visit(&cl->l.g);
          for (int i = 0; i < cl->l.nupvalues; ++i) add(valtogco(cl->l.upvals[i]));
        }
        break;
      }
      case LUA_TPROTO: {
        Proto *p = gcotop(o);
        for (int i = 0; i < p->sizek; ++i) visit(&p->k[i]);
        for (int i = 0; i < p->sizep; ++i) add(valtogco(p->p[i]));
        for (int i = 0; i < p->sizelocvars; ++i) add(valtogco(p->locvars[i].varname));
        for (int i = 0; i < p->sizeupvalues; ++i) add(valtogco(p->upvalues[i]));
        add(valtogco(p->source));
        break;
      }
      case LUA_TUPVAL: {
        UpVal *uv = gcotouv(o);
        if (uv->v == &uv->value) visit(&uv->value);
        break;  // an open one's value is its thread's slot
      }
      case LUA_TTHREAD: {
        lua_State *th = gcototh(o);
        if (th->nCcalls != 0 || th->errorJmp != nullptr) {
          err_ = "a coroutine is running (not at rest between ticks)";
          return;
        }
        for (StkId s = th->stack; s < th->top; ++s) visit(s);
        visit(gt(th));
        for (GCObject *u = th->openupval; u != nullptr; u = u->gch.next) add(u);
        break;
      }
      case LUA_TUSERDATA: {
        Udata *u = gcotou(o);
        if (u->uv.metatable != hvalue(defaultmeta(L_))) add(valtogco(u->uv.metatable));
        break;
      }
      default:
        err_ = "an object of an unknown kind";
    }
  }

  // Open upvalues: each one's thread and slot. One open on a thread the
  // save doesn't reach (garbage, never to run again) is written closed,
  // with its value: nothing can tell the difference.
  void open_upvals() {
    open_.clear();
    for (GCObject *o : objs_) {
      if (o->gch.tt != LUA_TTHREAD) continue;
      lua_State *th = gcototh(o);
      for (GCObject *u = th->openupval; u != nullptr; u = u->gch.next)
        open_[gcotouv(u)] = {index_.at(o), static_cast<std::int32_t>(gcotouv(u)->v - th->stack)};
    }
    for (GCObject *o : objs_) {
      if (o->gch.tt != LUA_TUPVAL) continue;
      UpVal *uv = gcotouv(o);
      if (uv->v != &uv->value && open_.count(uv) == 0) {
        visit(uv->v);  // closed in the save: its value must be written
        if (uv->v >= g_->mainthread->stack && uv->v < g_->mainthread->stack + g_->mainthread->stacksize) {
          err_ = "an upvalue is open on the main thread";
          return;
        }
      }
    }
  }

  void header(GCObject *o) {
    switch (o->gch.tt) {
      case LUA_TSTRING: {
        TString *s = gcotots(o);
        w_.u8(K_STRING);
        w_.u8((s->tsv.marked & (1 << 4)) ? F_FIXED : 0);
        w_.u64(s->tsv.len);
        w_.raw(getstr(s), s->tsv.len);
        break;
      }
      case LUA_TTABLE: {
        Table *h = gcotoh(o);
        w_.u8(K_TABLE);
        w_.i32(h->sizearray);
        w_.u8(h->node == g_->dummynode ? 0 : static_cast<std::uint8_t>(h->lsizenode + 1));
        w_.u8((h->marked & (1 << FROZENBIT)) ? F_FROZEN : 0);
        break;
      }
      case LUA_TFUNCTION: {
        Closure *cl = gcotocl(o);
        w_.u8(cl->c.isC ? K_CCLOSURE : K_LCLOSURE);
        w_.u8(cl->c.nupvalues);
        break;
      }
      case LUA_TPROTO: {
        Proto *p = gcotop(o);
        w_.u8(K_PROTO);
        w_.i32(p->sizek);
        w_.i32(p->sizecode);
        w_.i32(p->sizep);
        w_.i32(p->sizelineinfo);
        w_.i32(p->sizelocvars);
        w_.i32(p->sizeupvalues);
        break;
      }
      case LUA_TUPVAL:
        w_.u8(K_UPVAL);
        break;
      case LUA_TTHREAD: {
        lua_State *th = gcototh(o);
        w_.u8(K_THREAD);
        w_.i32(th->stacksize);
        w_.i32(th->size_ci);
        break;
      }
      case LUA_TUSERDATA:
        w_.u8(K_USERDATA);
        w_.u64(gcotou(o)->uv.len);
        break;
    }
  }

  void object(GCObject *o) {
    if (o == nullptr) { w_.u32(0); return; }
    w_.u32(index_.at(o) + 1);
  }

  void value(const TObject *o) {
    switch (ttype(o)) {
      case LUA_TNIL: w_.u8(T_NIL); return;
      case LUA_TBOOLEAN: w_.u8(bvalue(o) ? T_TRUE : T_FALSE); return;
      case LUA_TNUMBER: w_.u8(T_NUMBER); w_.num(nvalue(o)); return;
      case LUA_TLIGHTUSERDATA: {
        void *p = pvalue(o);
        if (p == nullptr) { w_.u8(T_NULLLIGHT); return; }
        std::uint32_t kind = 0;
        std::uint64_t id = 0;
        if (hooks_.name == nullptr || !hooks_.name(hooks_.ud, p, &kind, &id)) {
          if (err_.empty()) err_ = "a light userdata the hooks cannot name";
          w_.u8(T_NULLLIGHT);
          return;
        }
        w_.u8(T_LIGHT);
        w_.u32(kind);
        w_.u64(id);
        return;
      }
      default: {
        GCObject *g = gcvalue(o);
        if (g == valtogco(g_->mainthread)) { w_.u8(T_MAIN); return; }
        w_.u8(T_OBJECT);
        w_.u32(index_.at(g));
      }
    }
  }

  // A key: a removed one whose object is gone becomes a dead key.
  void key(const TObject *k, bool removed) {
    if (removed && !dead_key_alive(k)) { w_.u8(T_DEADKEY); return; }
    value(k);
  }

  void contents(GCObject *o) {
    switch (o->gch.tt) {
      case LUA_TSTRING:
        break;
      case LUA_TTABLE: table(gcotoh(o)); break;
      case LUA_TFUNCTION: {
        Closure *cl = gcotocl(o);
        if (cl->c.isC) {
          w_.i64(cfunc_offset(cl->c.f));
          for (int i = 0; i < cl->c.nupvalues; ++i) value(&cl->c.upvalue[i]);
        } else {
          object(valtogco(cl->l.p));
          value(&cl->l.g);
          for (int i = 0; i < cl->l.nupvalues; ++i) object(valtogco(cl->l.upvals[i]));
        }
        break;
      }
      case LUA_TPROTO: {
        Proto *p = gcotop(o);
        for (int i = 0; i < p->sizek; ++i) value(&p->k[i]);
        w_.raw(p->code, sizeof(Instruction) * static_cast<std::size_t>(p->sizecode));
        for (int i = 0; i < p->sizep; ++i) object(valtogco(p->p[i]));
        w_.raw(p->lineinfo, sizeof(int) * static_cast<std::size_t>(p->sizelineinfo));
        for (int i = 0; i < p->sizelocvars; ++i) {
          object(valtogco(p->locvars[i].varname));
          w_.i32(p->locvars[i].startpc);
          w_.i32(p->locvars[i].endpc);
        }
        for (int i = 0; i < p->sizeupvalues; ++i) object(valtogco(p->upvalues[i]));
        object(valtogco(p->source));
        w_.i32(p->lineDefined);
        w_.u8(p->nups);
        w_.u8(p->numparams);
        w_.u8(p->is_vararg);
        w_.u8(p->maxstacksize);
        break;
      }
      case LUA_TUPVAL: {
        UpVal *uv = gcotouv(o);
        auto open = open_.find(uv);
        if (open != open_.end()) {
          w_.u8(1);
          w_.u32(open->second.first);
          w_.i32(open->second.second);
        } else {
          w_.u8(0);
          value(uv->v);  // closed (or open on a thread the save leaves out)
        }
        break;
      }
      case LUA_TTHREAD: thread(gcototh(o)); break;
      case LUA_TUSERDATA: {
        Udata *u = gcotou(o);
        object(u->uv.metatable != hvalue(defaultmeta(L_)) ? valtogco(u->uv.metatable) : nullptr);
        if (is_io_file(L_, io_meta_, u)) {
          const IoFile *file = reinterpret_cast<const IoFile *>(u + 1);
          std::uint8_t stream = 0;
          if (file->f == stdin) stream = 1;
          else if (file->f == stdout) stream = 2;
          else if (file->f == stderr) stream = 3;
          else if (file->f != nullptr && err_.empty()) err_ = "a file open in the io library";
          w_.u8(U_IOFILE);
          w_.u8(stream);
          w_.i32(file->ispipe);
        } else {
          w_.u8(U_RAW);
          w_.raw(u + 1, u->uv.len);
        }
        break;
      }
    }
  }

  void table(Table *h) {
    object(h->metatable != hvalue(defaultmeta(L_)) ? valtogco(h->metatable) : nullptr);
    for (int i = 0; i < h->sizearray; ++i) value(&h->array[i]);
    if (h->node == g_->dummynode) return;
    const int size = sizenode(h);
    bool exact = true;
    for (int i = 0; i < size && exact; ++i) {
      Node *n = gnode(h, i);
      const bool removed = ttisnil(gval(n));
      if (ttisnil(gkey(n)) || ttype(gkey(n)) == LUA_TNONE) continue;
      if (removed && !dead_key_alive(gkey(n))) continue;  // written as a dead key
      if (hashes_by_address(gkey(n))) exact = false;
    }
    w_.u8(exact ? L_EXACT : L_INSERT);
    if (exact) {
      for (int i = 0; i < size; ++i) {
        Node *n = gnode(h, i);
        if (ttype(gkey(n)) == LUA_TNONE) w_.u8(T_DEADKEY);
        else key(gkey(n), ttisnil(gval(n)));
        value(gval(n));
        w_.i32(n->next ? static_cast<std::int32_t>(n->next - h->node) : -1);
      }
      w_.i32(static_cast<std::int32_t>(h->firstfree - h->node));
    } else {
      // In the order the table iterates; a load inserts them again.
      std::uint32_t count = 0;
      for (int i = 0; i < size; ++i)
        if (!ttisnil(gval(gnode(h, i)))) ++count;
      w_.u32(count);
      for (int i = 0; i < size; ++i) {
        Node *n = gnode(h, i);
        if (ttisnil(gval(n))) continue;
        value(gkey(n));
        value(gval(n));
      }
    }
  }

  void thread(lua_State *th) {
    value(gt(th));
    const auto slot = [&](StkId s) { return static_cast<std::int32_t>(s - th->stack); };
    w_.i32(slot(th->top));
    w_.i32(slot(th->base));
    for (StkId s = th->stack; s < th->top; ++s) value(s);
    const std::int32_t frames = static_cast<std::int32_t>(th->ci - th->base_ci);
    w_.i32(frames);
    for (CallInfo *ci = th->base_ci; ci <= th->ci; ++ci) {
      w_.i32(slot(ci->base));
      w_.i32(slot(ci->top));
      w_.i32(ci->state);
      const bool lua_frame = !(ci->state & CI_C) && ci != th->base_ci;
      if (lua_frame) {
        const Proto *p = ci_func(ci)->l.p;
        w_.i32((ci->state & CI_SAVEDPC) ? static_cast<std::int32_t>(ci->u.l.savedpc - p->code) : -1);
        w_.i32(ci->u.l.tailcalls);
      }
    }
    // Its open upvalues, highest slot first, as it keeps them.
    std::uint32_t n = 0;
    for (GCObject *u = th->openupval; u != nullptr; u = u->gch.next) ++n;
    w_.u32(n);
    for (GCObject *u = th->openupval; u != nullptr; u = u->gch.next) object(u);
  }

  lua_State *L_;
  global_State *g_;
  Writer w_;
  const lua_PersistHooks &hooks_;
  std::string err_;
  std::vector<GCObject *> objs_;
  std::unordered_map<const GCObject *, std::uint32_t> index_;
  std::unordered_set<const GCObject *> strings_;
  std::unordered_map<const UpVal *, std::pair<std::uint32_t, std::int32_t>> open_;
  Table *io_meta_ = nullptr;
};

// ----------------------------------------------------------------- loading

struct Created {
  std::uint8_t kind = 0;
  std::uint8_t flags = 0;  // Flag
  bool linked = false;     // an upvalue on the heap's list (a closed one, filled)
  GCObject *o = nullptr;
  // Sizes a table or thread was made with, checked while filling.
  std::int32_t a = 0, b = 0;
};

class Unpersister {
 public:
  Unpersister(lua_State *L, std::string_view in, const lua_PersistHooks &hooks)
      : L_(L), g_(G(L)), r_(in), hooks_(hooks) {}

  std::string err;
  bool committed = false;  // the new heap took over

  // After a failed load, what it made is garbage for the state's next
  // collection: all but the upvalues it had yet to link to the heap's list.
  void discard() {
    for (const Created &c : objs_)
      if (c.kind == K_THREAD && c.o != nullptr) gcototh(c.o)->openupval = nullptr;
    for (const Created &c : objs_)
      if (c.kind == K_UPVAL && c.o != nullptr && !c.linked) luaM_freelem(L_, gcotouv(c.o));
  }

  void run() {
    if (L_ != g_->mainthread) return fail("not the state's main thread");
    if (L_->ci != L_->base_ci || L_->top != L_->base || L_->openupval != nullptr)
      return fail("the main thread is not at rest (its stack holds values)");
    char magic[sizeof kMagic];
    r_.raw(magic, sizeof magic);
    if (!r_.ok() || std::memcmp(magic, kMagic, sizeof kMagic) != 0) return fail("not a Lua snapshot");
    if (r_.u32() != kVersion) return fail("a Lua snapshot of another version");
    if (r_.u32() != sizeof(Instruction)) return fail("a Lua snapshot from another platform");
    const std::uint8_t lazy = r_.u8();
    const std::uint8_t manual = r_.u8();
    const std::uint32_t count = r_.u32();
    if (!r_.ok()) return fail("a truncated Lua snapshot");

    // Nothing is collected while the heap is half made.
    g_->GCthreshold = MAX_LUMEM;
    objs_.resize(count);
    for (std::uint32_t i = 0; i < count && err.empty(); ++i) make(objs_[i]);
    for (std::uint32_t i = 0; i < count && err.empty(); ++i) fill(objs_[i]);
    if (!err.empty()) return;
    // Frames' saved pcs, into their functions' code
    for (const auto &[ci, pc] : pcs_) {
      const Proto *p = clvalue(ci->base - 1)->l.p;
      if (p == nullptr || pc > p->sizecode) return fail("a frame's pc past its code");
      ci->u.l.savedpc = p->code + pc;
    }

    TObject reg, globals;
    value(&reg);
    value(&globals);
    Table *mt[NUM_TAGS + 1];
    for (int i = 0; i <= NUM_TAGS; ++i) {
      GCObject *o = object(K_TABLE);
      mt[i] = o ? gcotoh(o) : nullptr;
    }
    const std::uint32_t nroots = r_.u32();
    if (nroots > count) return fail("a malformed Lua snapshot");
    std::vector<Table *> frozen_roots(nroots);
    for (Table *&t : frozen_roots) {
      const std::uint32_t n = r_.u32();
      if (n == 0 || n > count || !(objs_[n - 1].flags & F_FROZEN) || objs_[n - 1].kind != K_TABLE)
        return fail("a frozen root that is no frozen table");
      t = gcotoh(objs_[n - 1].o);
    }
    if (!r_.ok() || !r_.at_end()) return fail("a malformed Lua snapshot");
    if (!ttistable(&reg) || !ttistable(&globals)) return fail("a snapshot without its registry");

    // The new heap takes over; the old one is garbage. (The main thread's
    // stack is emptied: anything on it would keep the old heap alive.)
    setobj(registry(L_), &reg);
    setobj(gt(L_), &globals);
    for (int i = 0; i <= NUM_TAGS; ++i) g_->mt[i] = mt[i];
    committed = true;
    L_->top = L_->base;
    thaw();
    // Collect the old heap, all of it now. The new one is marked
    // beforehand, so the collector neither walks it nor clears its weak
    // tables: they keep what the saved game's held until its next
    // collection. The collection leaves the threshold as the saved state's
    // mode has it.
    for (const Created &c : objs_) c.o->gch.marked |= 1;
    // The old heap's userdata go without their finalizers: those would run
    // the old heap's code against the new registry.
    for (GCObject *o = g_->rootudata; o != nullptr; o = o->gch.next)
      if (!(o->gch.marked & 1)) gcotou(o)->uv.metatable = hvalue(defaultmeta(L_));
    g_->lazysweep = 0;
    g_->manualgc = manual ? 1 : 0;
    luaC_collectgarbage(L_);
    g_->lazysweep = lazy ? 1 : 0;
    freeze(frozen_roots);
  }

 private:
  void fail(const char *what) {
    if (err.empty()) err = what;
  }

  // The old heap's frozen tables are garbage like the rest of it: back on
  // `rootgc', no longer marked, for the collection to free. (The strings
  // they fixed stay: a state's own -- reserved words, metamethod names --
  // are fixed too, and must stay so.)
  void thaw() {
    for (GCObject *o = g_->frozengc; o != nullptr;) {
      GCObject *next = o->gch.next;
      o->gch.marked &= static_cast<lu_byte>(~((1 << FROZENBIT) | (1 << FROZENROOTBIT) | (1 << 4)));
      o->gch.next = g_->rootgc;
      g_->rootgc = o;
      o = next;
    }
    g_->frozengc = nullptr;
    luaM_freearray(L_, g_->frozenroots, g_->sizefrozenroots, Table *);
    g_->frozenroots = nullptr;
    g_->nfrozenroots = g_->sizefrozenroots = 0;
  }

  // What the saved game froze, frozen again, as lua_freeze leaves it (lgc.c):
  // fixed, on `frozengc', its strings fixed, and its frozen roots in order.
  void freeze(const std::vector<Table *> &roots) {
    for (const Created &c : objs_) {
      if (c.kind == K_TABLE && (c.flags & F_FROZEN))
        c.o->gch.marked |= static_cast<lu_byte>((1 << FROZENBIT) | (1 << 4));
      else if (c.kind == K_STRING && (c.flags & F_FIXED))
        luaS_fix(gcotots(c.o));
    }
    for (GCObject **p = &g_->rootgc; *p != nullptr;) {
      GCObject *o = *p;
      if (o->gch.tt == LUA_TTABLE && (o->gch.marked & (1 << FROZENBIT))) {
        *p = o->gch.next;
        o->gch.next = g_->frozengc;
        g_->frozengc = o;
      } else {
        p = &o->gch.next;
      }
    }
    for (Table *t : roots) luaC_frozenwrite(L_, t);
  }

  void make(Created &c) {
    c.kind = r_.u8();
    switch (c.kind) {
      case K_STRING: {
        c.flags = r_.u8();
        const std::uint64_t len = r_.u64();
        std::string_view s = r_.take(static_cast<std::size_t>(len));
        if (!r_.ok()) return fail("a truncated string");
        c.o = valtogco(luaS_newlstr(L_, s.data(), s.size()));
        break;
      }
      case K_TABLE: {
        c.a = r_.i32();          // array size
        c.b = r_.u8();           // log2 of the node size, plus one (0: none)
        c.flags = r_.u8();
        // (Its array's values follow, a byte each at least. One node of its
        // own Lua never makes: a table with lsizenode 0 has the dummy node.)
        if (c.a < 0 || static_cast<std::size_t>(c.a) > r_.left() || c.b == 1 || c.b > 31)
          return fail("a malformed table");
        c.o = valtogco(luaH_new(L_, c.a, c.b == 0 ? 0 : c.b - 1));
        break;
      }
      case K_LCLOSURE: {
        const int n = r_.u8();
        TObject nil;
        setnilvalue(&nil);
        Closure *cl = luaF_newLclosure(L_, n, &nil);
        cl->l.p = nullptr;
        for (int i = 0; i < n; ++i) cl->l.upvals[i] = nullptr;
        c.o = valtogco(cl);
        c.a = n;
        break;
      }
      case K_CCLOSURE: {
        const int n = r_.u8();
        Closure *cl = luaF_newCclosure(L_, n);
        cl->c.f = &anchor;
        for (int i = 0; i < n; ++i) setnilvalue(&cl->c.upvalue[i]);
        c.o = valtogco(cl);
        c.a = n;
        break;
      }
      case K_PROTO: {
        Proto *p = luaF_newproto(L_);
        const std::int32_t sizes[6] = {r_.i32(), r_.i32(), r_.i32(), r_.i32(), r_.i32(), r_.i32()};
        // Each array's contents follow: at least this many bytes an entry
        const std::size_t bytes[6] = {1, sizeof(Instruction), 4, sizeof(int), 12, 4};
        for (int i = 0; i < 6; ++i)
          if (sizes[i] < 0 || static_cast<std::size_t>(sizes[i]) > r_.left() / bytes[i])
            return fail("a malformed prototype");
        p->k = luaM_newvector(L_, sizes[0], TObject);
        p->sizek = sizes[0];
        for (int i = 0; i < p->sizek; ++i) setnilvalue(&p->k[i]);
        p->code = luaM_newvector(L_, sizes[1], Instruction);
        p->sizecode = sizes[1];
        p->p = luaM_newvector(L_, sizes[2], Proto *);
        p->sizep = sizes[2];
        for (int i = 0; i < p->sizep; ++i) p->p[i] = nullptr;
        p->lineinfo = luaM_newvector(L_, sizes[3], int);
        p->sizelineinfo = sizes[3];
        p->locvars = luaM_newvector(L_, sizes[4], LocVar);
        p->sizelocvars = sizes[4];
        for (int i = 0; i < p->sizelocvars; ++i) p->locvars[i].varname = nullptr;
        p->upvalues = luaM_newvector(L_, sizes[5], TString *);
        p->sizeupvalues = sizes[5];
        for (int i = 0; i < p->sizeupvalues; ++i) p->upvalues[i] = nullptr;
        c.o = valtogco(p);
        break;
      }
      case K_UPVAL: {
        UpVal *uv = luaM_new(L_, UpVal);
        uv->tt = LUA_TUPVAL;
        uv->marked = 1;
        uv->next = nullptr;
        uv->v = &uv->value;
        setnilvalue(&uv->value);
        c.o = valtogco(uv);  // linked when filled: closed to the heap, open to its thread
        break;
      }
      case K_THREAD: {
        c.a = r_.i32();  // stack size
        c.b = r_.i32();  // call-info size
        if (c.a <= EXTRA_STACK || c.a > kMaxStack || c.b < 1 || c.b > 0xFFFF)
          return fail("a malformed thread");
        lua_State *th = luaE_newthread(L_);
        luaD_reallocstack(th, c.a);
        luaD_reallocCI(th, c.b);
        for (int i = 0; i < th->stacksize; ++i) setnilvalue(th->stack + i);
        c.o = valtogco(th);
        break;
      }
      case K_USERDATA: {
        const std::uint64_t len = r_.u64();
        if (len > r_.left()) return fail("a malformed userdata");  // its bytes follow
        c.o = valtogco(luaS_newudata(L_, static_cast<std::size_t>(len)));
        c.a = static_cast<std::int32_t>(len);
        break;
      }
      default:
        return fail("an object of an unknown kind");
    }
    if (!r_.ok()) fail("a truncated object");
  }

  // An object by its number, of the kind the caller needs (0: any).
  GCObject *object(std::uint8_t kind) {
    const std::uint32_t n = r_.u32();
    if (n == 0) return nullptr;
    if (n > objs_.size()) { fail("a reference past the objects"); return nullptr; }
    const Created &c = objs_[n - 1];
    if (kind != 0 && c.kind != kind) { fail("a reference to the wrong kind of object"); return nullptr; }
    return c.o;
  }

  void value(TObject *o) {
    switch (r_.u8()) {
      case T_NIL: setnilvalue(o); return;
      case T_FALSE: setbvalue(o, 0); return;
      case T_TRUE: setbvalue(o, 1); return;
      case T_NUMBER: setnvalue(o, r_.num()); return;
      case T_NULLLIGHT: setpvalue(o, nullptr); return;
      case T_LIGHT: {
        const std::uint32_t kind = r_.u32();
        const std::uint64_t id = r_.u64();
        void *p = nullptr;
        if (hooks_.find == nullptr || !hooks_.find(hooks_.ud, kind, id, &p))
          return fail("a light userdata the hooks cannot find");
        setpvalue(o, p);
        return;
      }
      case T_MAIN: setthvalue(o, g_->mainthread); return;
      case T_DEADKEY: setnilvalue(o); setttype(o, LUA_TNONE); return;
      case T_OBJECT: {
        const std::uint32_t n = r_.u32();
        if (n >= objs_.size()) return fail("a reference past the objects");
        const Created &c = objs_[n];
        switch (c.kind) {
          case K_STRING: setsvalue(o, gcotots(c.o)); return;
          case K_TABLE: sethvalue(o, gcotoh(c.o)); return;
          case K_LCLOSURE: case K_CCLOSURE: setclvalue(o, gcotocl(c.o)); return;
          case K_THREAD: setthvalue(o, gcototh(c.o)); return;
          case K_USERDATA: setuvalue(o, gcotou(c.o)); return;
          default: return fail("a value naming an internal object");
        }
      }
      default:
        fail("a value of an unknown kind");
    }
  }

  Table *metatable() {
    GCObject *mt = object(K_TABLE);
    return mt ? gcotoh(mt) : hvalue(defaultmeta(L_));
  }

  void fill(Created &c) {
    switch (c.kind) {
      case K_STRING: break;
      case K_TABLE: table(gcotoh(c.o), c); break;
      case K_LCLOSURE: {
        Closure *cl = gcotocl(c.o);
        GCObject *p = object(K_PROTO);
        if (p == nullptr) return fail("a closure without its prototype");
        cl->l.p = gcotop(p);
        value(&cl->l.g);
        for (int i = 0; i < c.a; ++i) {
          GCObject *uv = object(K_UPVAL);
          if (uv == nullptr) return fail("a closure without an upvalue");
          cl->l.upvals[i] = gcotouv(uv);
        }
        break;
      }
      case K_CCLOSURE: {
        Closure *cl = gcotocl(c.o);
        cl->c.f = cfunc_at(r_.i64());
        for (int i = 0; i < c.a; ++i) value(&cl->c.upvalue[i]);
        break;
      }
      case K_PROTO: {
        Proto *p = gcotop(c.o);
        for (int i = 0; i < p->sizek; ++i) value(&p->k[i]);
        r_.raw(p->code, sizeof(Instruction) * static_cast<std::size_t>(p->sizecode));
        for (int i = 0; i < p->sizep; ++i) {
          GCObject *q = object(K_PROTO);
          if (q == nullptr) return fail("a prototype without a nested one");
          p->p[i] = gcotop(q);
        }
        r_.raw(p->lineinfo, sizeof(int) * static_cast<std::size_t>(p->sizelineinfo));
        for (int i = 0; i < p->sizelocvars; ++i) {
          GCObject *name = object(K_STRING);
          p->locvars[i].varname = name ? gcotots(name) : nullptr;
          p->locvars[i].startpc = r_.i32();
          p->locvars[i].endpc = r_.i32();
        }
        for (int i = 0; i < p->sizeupvalues; ++i) {
          GCObject *name = object(K_STRING);
          p->upvalues[i] = name ? gcotots(name) : nullptr;
        }
        GCObject *source = object(K_STRING);
        p->source = source ? gcotots(source) : nullptr;
        p->lineDefined = r_.i32();
        p->nups = r_.u8();
        p->numparams = r_.u8();
        p->is_vararg = r_.u8();
        p->maxstacksize = r_.u8();
        break;
      }
      case K_UPVAL: {
        UpVal *uv = gcotouv(c.o);
        if (r_.u8() == 1) {
          // Open: its slot on its thread, linked when the thread is filled.
          const std::uint32_t th = r_.u32();
          const std::int32_t slot = r_.i32();
          if (th >= objs_.size() || objs_[th].kind != K_THREAD) return fail("an upvalue open on no thread");
          const Created &t = objs_[th];
          if (slot < 0 || slot >= t.a - EXTRA_STACK) return fail("an upvalue open past its thread's stack");
          uv->v = gcototh(t.o)->stack + slot;
        } else {
          value(&uv->value);
          uv->v = &uv->value;
          luaC_link(L_, valtogco(uv), LUA_TUPVAL);
          c.linked = true;
        }
        break;
      }
      case K_THREAD: thread(gcototh(c.o), c); break;
      case K_USERDATA: {
        Udata *u = gcotou(c.o);
        u->uv.metatable = metatable();
        const std::uint8_t payload = r_.u8();
        if (payload == U_RAW) {
          r_.raw(u + 1, u->uv.len);
        } else if (payload == U_IOFILE) {
          if (u->uv.len != sizeof(IoFile)) return fail("an io file of another size");
          IoFile *file = reinterpret_cast<IoFile *>(u + 1);
          const std::uint8_t stream = r_.u8();
          file->ispipe = r_.i32();
          FILE *const streams[4] = {nullptr, stdin, stdout, stderr};
          if (stream > 3) return fail("an io file of no stream");
          file->f = streams[stream];
        } else {
          return fail("a userdata of an unknown kind");
        }
        break;
      }
    }
  }

  void table(Table *h, const Created &c) {
    h->metatable = metatable();
    h->flags = 0;  // the metamethod cache refills as it is used
    for (int i = 0; i < h->sizearray; ++i) value(&h->array[i]);
    if (c.b == 0) return;
    const int size = sizenode(h);
    const std::uint8_t layout = r_.u8();
    if (layout == L_EXACT) {
      for (int i = 0; i < size; ++i) {
        Node *n = gnode(h, i);
        value(gkey(n));
        value(gval(n));
        const std::int32_t next = r_.i32();
        if (next < -1 || next >= size) return fail("a table chain past its nodes");
        n->next = next < 0 ? nullptr : gnode(h, next);
      }
      const std::int32_t free = r_.i32();
      if (free < 0 || free >= size) return fail("a table's free node past its nodes");
      h->firstfree = gnode(h, free);
    } else if (layout == L_INSERT) {
      const std::uint32_t count = r_.u32();
      for (std::uint32_t i = 0; i < count && err.empty() && r_.ok(); ++i) {
        TObject k, v;
        value(&k);
        value(&v);
        if (ttisnil(&k)) return fail("a nil table key");
        setobj(luaH_set(L_, h, &k), &v);
      }
      if (sizenode(h) != size) return fail("a table that grew as it was filled");
    } else {
      fail("a table of an unknown layout");
    }
  }

  void thread(lua_State *th, const Created &c) {
    value(gt(th));
    // Nothing past stack_last + 1: the collector clears up to the highest
    // top, and the slots after stack_last are Lua's slack (EXTRA_STACK)
    const std::int32_t last = c.a - EXTRA_STACK;
    const std::int32_t top = r_.i32();
    const std::int32_t base = r_.i32();
    if (top < 0 || top > last || base < 0 || base > top) return fail("a thread's stack past its size");
    for (std::int32_t i = 0; i < top; ++i) value(th->stack + i);
    th->top = th->stack + top;
    th->base = th->stack + base;
    const std::int32_t frames = r_.i32();
    if (frames < 0 || frames >= c.b) return fail("a thread's frames past its size");
    for (std::int32_t f = 0; f <= frames && err.empty(); ++f) {
      CallInfo *ci = th->base_ci + f;
      const std::int32_t cb = r_.i32();
      const std::int32_t ct = r_.i32();
      if (cb < 0 || cb > last || ct < 0 || ct > last) return fail("a frame past its thread's stack");
      ci->base = th->stack + cb;
      ci->top = th->stack + ct;
      ci->state = r_.i32();
      ci->u.l.pc = nullptr;
      ci->u.l.savedpc = nullptr;
      ci->u.l.tailcalls = 0;
      if (!(ci->state & CI_C) && f != 0) {
        const std::int32_t pc = r_.i32();
        ci->u.l.tailcalls = r_.i32();
        if (cb < 1 || !ttisfunction(ci->base - 1) || clvalue(ci->base - 1)->c.isC)
          return fail("a Lua frame without its function");
        // Its function's prototype may not be filled yet: the pc waits.
        if (pc >= 0) pcs_.push_back({ci, pc});
      }
    }
    th->ci = th->base_ci + frames;
    const std::uint32_t open = r_.u32();
    GCObject **tail = &th->openupval;
    for (std::uint32_t i = 0; i < open && err.empty(); ++i) {
      GCObject *u = object(K_UPVAL);
      if (u == nullptr) return fail("a thread's open upvalue missing");
      *tail = u;
      tail = &u->gch.next;
    }
    *tail = nullptr;
  }

  lua_State *L_;
  global_State *g_;
  Reader r_;
  const lua_PersistHooks &hooks_;
  std::vector<Created> objs_;
  std::vector<std::pair<CallInfo *, std::int32_t>> pcs_;
};

struct LoadCall {
  Unpersister *u;
};

void load_protected(lua_State *, void *ud) { static_cast<LoadCall *>(ud)->u->run(); }

}  // namespace

std::string lua_persist(lua_State *L, std::string &out, const lua_PersistHooks &hooks) {
  const std::size_t start = out.size();
  Persister p(L, out, hooks);
  std::string err = p.run();
  if (!err.empty()) {
    out.resize(start);
    return err;
  }
  const std::uint64_t h = snapshot_hash(std::string_view(out).substr(start));
  out.append(reinterpret_cast<const char *>(&h), sizeof h);
  return {};
}

void lua_persist_seal(std::string &snapshot) {
  if (snapshot.size() < sizeof(std::uint64_t)) return;
  const std::size_t body = snapshot.size() - sizeof(std::uint64_t);
  const std::uint64_t h = snapshot_hash(std::string_view(snapshot).substr(0, body));
  std::memcpy(snapshot.data() + body, &h, sizeof h);
}

std::string lua_unpersist(lua_State *L, std::string_view in, const lua_PersistHooks &hooks) {
  std::uint64_t h = 0;
  if (in.size() < sizeof h) return "not a Lua snapshot";
  in.remove_suffix(sizeof h);
  std::memcpy(&h, in.data() + in.size(), sizeof h);
  if (snapshot_hash(in) != h) return "a corrupted Lua snapshot (its hash differs)";
  Unpersister u(L, in, hooks);
  LoadCall call{&u};
  const lu_mem threshold = G(L)->GCthreshold;
  const int status = luaD_rawrunprotected(L, load_protected, &call);
  if (status != 0 && u.err.empty()) {
    u.err = status == LUA_ERRMEM ? "out of memory while loading" : "a Lua error while loading";
    if (lua_type(L, -1) == LUA_TSTRING) u.err += std::string(": ") + lua_tostring(L, -1);
  }
  if (!u.err.empty() && !u.committed) {
    u.discard();
    G(L)->GCthreshold = threshold;
  }
  return u.err;
}

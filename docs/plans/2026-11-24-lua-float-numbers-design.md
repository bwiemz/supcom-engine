# Lua numbers as floats

Design, 2026-11-24. Step 3 of the bit-exact plan (`2026-10-23-moho-bit-exact-sim-design.md`, section
3). Moho's Lua keeps a number in a float. The engine's keeps it in a double, so a script's result is
rounded once, at the end, instead of after each operation, and a value the engine moves a little
ends up one unit in the last place (ulp) away from retail's. This note says where Moho's Lua makes a
number and what it makes, what the engine makes there, and the change.

Path prefixes:

- **FR:** faf-re (Draiget/faf-re, the decompiled FAF build of Moho), `src/`
- **EN:** this repository; `LUA:` is `third_party/lua-5.0/`

Every rule below comes from FR and, where marked **probe**, from retail 3599 itself: a sim hook that
logs what Lua makes of the same expressions, on one recorded game. The probe ran the engine with the
same hook before and after the change.

## 1. Where Moho's Lua makes a number

Moho's LuaPlus is built with `LUA_NUMBER=float`: a value is a float in a `TObject` of 8 bytes, and
the exported `lua_pushnumber` takes a `float` (its mangled name, `?lua_pushnumber@@YAXPAUlua_State@@M@Z`)
and `lua_tonumber` returns one (FR `lua/LuaObjectApi.inl`, `lua/luaplus/include/lua.h`). The simulation runs the x87 at 24-bit
precision (`_controlfp(_PC_24, _MCW_PC)` at each of its six call sites, listed in FR
`moho/net/CGpgNetInterface.cpp`), so each operation rounds its significand to 24 bits, and its
result is stored to the float.

| Where | Moho (FR) | Probe on retail |
|---|---|---|
| `+`, `-`, `*`, `/` | `op_Add`…`op_Div` store the float result. A string operand is not converted: the operator calls a tag method or raises (`CallBinTmOrRaiseArithmeticTypeError`). | `0.1 + 0.2` = 0.30000001192092896; `1 / 3` = 0.3333333432674408; `16777216 + 1` = 16777216; `"10" + 1` raises *attempt to perform arithmetic on a string value* |
| Unary minus | `op_Unm`: `-0.0f - x`, exact. The one arithmetic operator that converts a numeric string. | `-"2.5"` = -2.5 |
| Numeric `for` | `fornum` emits `init - step` (an `OP_SUB`) before the loop; `op_ForLoop` adds the step to the float index and compares floats; a string limit or step goes through `luaO_str2d`, but the `OP_SUB` raises first for a string step or initial value. | `for i = 0.1, 1, 0.1` runs 9 times, the last `i` 0.90000009536743164; `for i = 16777215, 16777220` never passes 16777216 |
| Decimal literals | `read_numeral` gathers digits, `.` and `e`/`E`, and `luaO_str2d` gives the float of the C runtime's `strtod` (MSVC8's). | `0.1` = 0.10000000149011612; `16777217` = 16777216; `4e-45` = 4.2038953929744512e-45; `1e39` is infinite |
| Hex literals | `read_numeral`: a lower-case `0x` and at most eight hex digits, gathered into an `int32`, then made a float. | `0xffffffff` = -1; `0x7fffffff` = 2147483648; `0x80000000` = -2147483648; `0X10` and `0x123456789` are syntax errors |
| Numeric strings | `luaO_str2d` (MSVC8's `strtod`, then a float) for `tonumber` in base 10, `lua_tonumber` and `lua_isnumber`, unary minus and a `for` limit. MSVC8's `strtod` reads no hex, `inf` or `nan`, and takes `d` or `D` for the exponent's `e`. | `tonumber("0.1")` = 0.10000000149011612; `tonumber("16777217")` = 16777216; `tonumber("0x10")`, `("inf")`, `("nan")` = nil; `tonumber("1d2")` = 100; `tonumber("3.4028236e38")` is infinite; `tonumber("7e-46")` = 0 |
| `tonumber` in another base | `luaB_tonumber`: `strtoul`, then `lua_pushnumber`. | `tonumber("ffffffff", 16)` = 4294967296 |
| `lua_pushnumber` | Takes a float: every value a binding pushes is the float nearest it. | `math.pi` = 3.1415927410125732 |
| `lua_tonumber` | Returns the float. | — |
| Bit operators (`&`, `\|`, `<<`, `>>`) | Each operand truncated to 64 bits and its low 32 kept; the shift count is the x86's, modulo 32; the result, read as unsigned, made a float. A string operand raises, as in arithmetic. | `0xffffffff & 0xffffffff` = 4294967296; `1 << 32` = 1; `256 >> 33` = 128; `"5" & 3` raises |
| `^` | `codebinop` lowers `^` (`OPR_POW`) to the opcode `op_BXor` runs: exclusive or, not a power. | `2 ^ 3` = 1; `2.5 ^ 2` = 0 |
| `math` library | Each function pushes a float. `math.deg` and `math.rad` multiply by the floats 57.29578 and 0.017453292. `sin`, `cos`, `tan`, `atan`, `atan2` are the x87 instructions; `exp`, `log`, `log10` inline x87 sequences; `asin`, `acos`, `pow`, `mod` the MSVC8 C runtime's x87 bodies (FR `lua/LuaObjectTableMathLib.inl`). | `math.deg(1)` = 57.295780181884766; `math.rad(1)` = 0.01745329238474369 |
| Text | `luaV_tostring` formats with `"%.14g"` after the float is promoted to double. | `tostring(0.1)` = "0.10000000149012" |

Retail's scripts use hex literals in `footprints.lua` (small flags) and `RenderSelectParams.lua`
(`ren_SelectColor = 0xffffffff`, which Moho reads as -1). `^` appears once, in `utilities.lua`'s
`GetDistanceBetweenTwoPoints`, which no retail script calls.

## 2. What the engine does now

`lua_Number` is `double` (LUA `lua.h`). Arithmetic, the `for` index, literals and `tonumber` keep
doubles, and `lua_pushnumber` keeps what it is given. A hex literal is read by `strtoul` (upper-case
`0X` too, any number of digits, unsigned), and `luaO_str2d` takes hex strings and glibc's `strtod`
spellings (`inf`, `nan`, hex floats) but not a `d` exponent. Bit operators give 0 for a shift of 32 or
more. On the probe, the engine matched retail on 6 of its 51 lines.

## 3. The change

The value stays a double, rounded to a float wherever Moho makes one. One macro does it,
`lua_numfloat(n)` = `(lua_Number)(float)(n)` (LUA `lua.h`):

- the results of `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV` and of their slow path (`Arith`);
- the `for` index after each step;
- the results of the bit operators, with the shift count taken modulo 32;
- `luaO_str2d`, which reads as MSVC8's `strtod` reads (no hex, `inf` or `nan`; `d` and `D` as `e`):
  decimal literals, numeric strings and `tonumber` in base 10;
- a hex literal, read as Moho's lexer reads it;
- `lua_pushnumber`: every value C++ hands Lua, `tonumber` in another base and the `math` library
  among them. `math.deg` and `math.rad` multiply by Moho's float constants.

Unary minus is exact and needs nothing.

**Why the result is Moho's bit for bit.** Every operand is already a float. For floats `a` and `b`,
the double result of `a + b`, `a - b`, `a * b` or `a / b` rounded to a float equals the float result
of the same operation: a double carries 53 bits, at least 2 × 24 + 2, so rounding twice gives what
rounding once gives. The x87 at 24 bits keeps its wider exponent until the store, so a result below
the float's normal range can round twice there and once here; nothing else differs. A literal or a
numeric string is the float of the double `strtod` gives, as in Moho.

**The engine's own handles stay exact.** The engine carries some of its own values in Lua numbers
where Moho uses userdata or strings: an entity's key (53 bits, in `_c_entity_id`, `_entity_id` and
`_c_prop_id`), a thread's serial and registry reference (`_c_serial`, `_c_ref`), an effect's id
(`_c_effect_id`) and a user decal's (`_c_user_decal`). Moho never gives Lua these as numbers, so
they are pushed unrounded, by a new `lua_pushdouble` (LUA `lapi.c`). So is a number copied from one
Lua state to another or restored from bytes (EN `core/lua_copy.cpp`, `sim/lua_bytes.cpp`): that is a
copy, not a new number. The other hidden fields hold small integers (a sim generation, an army),
which a float holds exactly.

**An entity id as a string stays exact.** Moho hands scripts an id as a string of its digits and
reads one back with `atoi` (`cfunc_GetEntityByIdL`, FR `moho/sim/SimLuaGetters.cpp`). The engine read
it through `lua_tonumber`, which now rounds an id past 2²⁴; it reads a numeric string as an integer
instead (EN `sim/script_class.cpp`, `to_entity_id`).

**`GetSystemTimeSeconds`** returned the seconds since the Unix epoch, about 1.7 × 10⁹, where floats
are 128 s apart. Moho's returns the seconds of a process timer that starts on its first use, early
in the program (`cfunc_GetSystemTimeSecondsL`, FR `moho/sim/UserLuaSession.cpp`;
`gpg::time::GetSystemTimer`). The engine's counts from the program's start.

The snapshot format is unchanged: a number persists as a double, already a float.

## 4. What it shifts

Every script computation, in the sim and the UI, moves in its last bits, and some by more:

- **Large integers** lose their low bits above 2²⁴, as in Moho; FA's and FAF's scripts are written
  for it.
- **Text**: `tostring(0.1)` is "0.10000000149012", as retail's.
- **Blueprints**: their numbers are literals, so they become the floats Moho reads.
- **Tests** that read a number back from Lua as a double: unit tests, and three data tests (a
  blueprint's `SizeX` as text, an effect's `Lifetime`, entity ids passed through `tonumber`). The
  goldens do not change.

## 5. Measured

On the change (the engine with every rule of section 3):

- **The probe**: the engine gives retail's value on 39 of its 51 lines, against 6 before. The 12
  left are section 6's: string operands (5 lines), `^` (1), number text (2, the floats equal), the
  order of `pairs` (1), and the text of three errors (two syntax errors both raise; one is the
  probe's own wrong call).
- **The Cybran build beam ends** (`CreateCybranBuildBeams`: `ox + x`, with `x` from
  `Unit.GetRandomOffset`'s three `Random()` and a heading's sine and cosine) on 20 recorded games,
  against retail's checksum beats: the ends within 2 ulp of retail's go from 67 on 51 beats to 1,
  on a beat where that game has already parted (20 larger differences there). The other entities
  that differ up to beat 550 go down as well: Cybran build drones and UEF build-effect projectiles,
  122 entity-beats fewer, none more.
- **Twenty other recorded games of that class**, played to their end: the first beat where the game
  parts from retail's moves later in 16 and stays in 4 (a unit parts on the same beat), earlier in
  none; the first script entity apart moves later in all 20, from beat 50 or 100 to 200–550.
- **Blueprints**: a dump of every field of the 3,778 retail blueprints, from retail and from the
  engine: 10,189 numbers the engine read rounded otherwise than retail; none with the change. 520
  more are the same float written otherwise (MSVC's three exponent digits and `1.#INF`).

## 6. Out of this step

Found on the way, each its own change:

- **String operands of arithmetic and bit operators**, and a string initial value or step of a
  numeric `for`: Moho raises, the engine converts them.
- **`^`** is exclusive or in Moho, a power in the engine.
- **Number text**: MSVC8 writes three exponent digits (`tostring(1e15)` = "9.999999869911e+014",
  `"%.17g"` of 0.540302276611328125 ends in 3, not 2) and rounds a `%f` half away from zero
  (`string.format("%5.1f", 2.25)` = "  2.3").
- **The `math` functions that are not correctly rounded** (`exp`, `log`, `log10`, `asin`, `acos`,
  `pow`, `mod`): step 2's `osc::dmath`.
- **`math.random`**: Moho's draws from the C runtime's `rand()`; the engine's from the session's
  stream.
- **Numeric table keys**: a key outside the array part hashes the bits of the float `n + 1.0f` in
  Moho (`luaH_mainposition`, FR `lua/LuaObjectStringsTables.inl`) and the double's words in the
  engine, so `pairs` visits such keys in another order (probe: retail
  `1,2,3,-1,10000000000,1.5,2.5,-0.25,100000,0.5`, the engine
  `1,2,3,-1,1.5,-0.25,2.5,10000000000,100000,0.5`).
- **Bindings that compute in double** what Moho computes in float: each binding, with its system.

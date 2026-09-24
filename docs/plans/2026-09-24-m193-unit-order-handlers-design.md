# M193 — Unit order handlers

Status: done, 2026-09-24. Phase C (architecture seams), brought forward after an external review of the codebase. It changes no behaviour.

## Why

- **`Unit::update` was 1,251 lines** (at M206f). One function held every step of a unit's tick:
  - dying and cargo;
  - the army's economy efficiency;
  - settling orders that were taken away unfinished;
  - 22 order kinds in one `switch`, driven by `continue`, `goto done_commands` and `return`;
  - then coasting, layer changes, air separation, fuel, the silo, regeneration, weapons and manipulators.
- **Every order-fidelity fix edited it,** and state from one order leaked into others. M206f's ferry kept its cycle on the unit, and a route replaced in one tick resumed the old one's leg.
- **Its size made review and history harder.** Splitting M206e into atomic commits meant hand-staging hunks inside the one function.
- **`unit.cpp` was 3,993 lines.**

## The design

Plain member functions, no class hierarchy.

- **One handler per order kind,** in `src/sim/unit_orders.cpp`: `order_move`, `order_attack`, `order_build_mobile`, `order_reclaim`, `order_guard`, `order_ferry`, and so on. Orders whose code was the same share one: `order_build_in_place` (a factory's build, an upgrade) and `order_launch` (nuke, tactical, OverCharge).
- **A handler takes what it uses:** the order, `dt`, the `SimContext`, the army's economy efficiency; `order_dive` only the Lua state.
- **What a handler returns:**

  | `OrderStep` | Meaning | Replaces |
  |---|---|---|
  | `Next` | The order is finished or dropped (the handler popped it); run the next one this tick. | `continue` |
  | `Hold` | The order goes on next tick. | `goto done_commands` |
  | `Gone` | A script destroyed the unit; stop updating it. | `return` |

- **`run_order`** dispatches: one `switch` line per order kind. **`tick_orders`** is the loop over it.
- **`Unit::update` is named phases,** in the same order as before:
  1. `tick_lifecycle`: dying, and following a transport. It says whether the tick stops there.
  2. The army's economy efficiency; renewing silo assist; settling interrupted orders; resetting ferry state (inline).
  3. `tick_orders` (skipped while paused).
  4. `tick_after_orders`: coasting, amphibious layer changes, air separation, fuel (skipped while paused).
  5. `tick_upkeep`: silo-assist payment, regeneration, the silo, motion events, weapons, manipulators.
- **What moved with the orders:** the helpers only they use (`reclaim_costs`, the lobby build rules) and the order-only members (`approach_update`, `ferry_beacon`, `ferry_fly`).

The same statements run in the same order. Only their homes changed.

## How

- **The conversion is a script** (it tracks block nesting through a small C++ tokenizer). Inside a case, at the handler's own level, `continue` became `Next`, `goto done_commands` `Hold`, and `return` `Gone`. A `continue` or `return` inside an inner loop or a lambda was left alone; a `break` at the handler's level, a `goto` in a lambda or a braceless loop holding control flow would have stopped it.
- **It reported every site:**
  - 64 `continue`s became `Next`; the 65th was the default case, now in `run_order`.
  - 31 `goto`s became `Hold`.
  - 6 `return`s became `Gone`; 6 more, in the ferry's `for_each_unit` lambdas, stayed.
  - These match a plain count over the old `switch`.
- **Before merging a second case into a handler,** it checked the bodies match token for token, comments aside.

## Proof

- **The oracle:** the per-tick domain checksum (11 domains, PR #65). Four-AI games were played before and after the split: SCMP_009, seeds 4242, 1, 7 and 99, 6,000 ticks, and 18,000 for seed 4242. The traces must be byte-identical, tick by tick and domain by domain. Every order's state lies in the orders, units, navigation, weapons and economy-event domains, so an order that behaved differently would show at the tick it happened.
- **Tests:** every gate mode (each order has its own `--*-test`) and the unit tests.
- **Cross-platform:** the cross-OS replay on the PR's Windows build.

**Results:**
- **The traces are byte-identical** for all five games: 6,001 lines each, and 18,001 for the long game. The oracle was run twice, before and after the patrol change below.
- **The gate** passes, 127 of 127, and the MP tests 5 of 5. The 443 unit tests pass.
- **The conversion is checked at the token level** (comments and whitespace aside):
  - the handlers match the script's output of the old cases;
  - the moved helpers match the old ones.

  The only token difference is deliberate. Patrol now moves the finished order to the back of the queue instead of copying it; clang-tidy flagged the copy, and the element was about to be destroyed anyway.
- The shields and economy-events domains stay empty in these games; their gate modes cover them.

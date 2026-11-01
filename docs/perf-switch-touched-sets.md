# coroutine-legacy switch cost: from O(app size) per yield to O(what the coroutine touched)

**Status:** implemented on the `perf/switch-o1` branch (ext + a 6-line framework change) · **Date:** 2026-10-24 · PHP 8.3.6 / OpenSwoole 26.2 · measured on a 24-core lab container, one worker, `ab -c 50`.

The earlier base-swap research (`perf-o1-baseswap-research.md`) ruled out
making the switch O(1) by repointing `CG(map_ptr_base)`. This work takes the
other route: keep every stage's semantics, but stop paying for state the
coroutine never touched.

## Where the time went (before)

`ZEALPHP_SWITCH_PROF=1` + `zealphp_switch_profile()` (new, per-stage
wall-clock accumulators) on the framework alone (no app classes), per switch
(park + unpark):

| stage | per switch |
|---|---|
| class statics save + restore | 31 + 31 µs |
| settings (cwd/locale/umask/tz/mbenc/libxml getters) | 1.8 µs |
| superglobals save + restore (7 deep copies each way) | 1.4 + 1.5 µs |
| $GLOBALS save + restore (5 table walks) | 1.4 + 2.3 µs |
| fn statics save + restore | 0.9 + 1.3 µs |
| **total** | **~70 µs** |

and per request: `zealphp_reset_request_rtcaches` 47 µs (full function +
class table sweep), `zealphp_coroutine_globals_request_end` 13 µs.

Class statics dominated with **zero** user classes because every boot class
(the framework's own `App` with ~150 statics) had each scalar/array static
deep-copied on every yield and copied back on every resume.

## What changed

1. **Profiler.** `ZEALPHP_SWITCH_PROF=1` enables per-stage counters;
   `zealphp_switch_profile(bool $reset = false)` returns them. Off: one
   static branch per stage.

2. **Class statics (S5b).**
   - A worker-level *index* of user classes that have static members (rebuilt
     only when the class table changes size, a deletion bumps
     `zealphp_symtab_gen`, or the boot snapshot is retaken) replaces the
     per-yield walk of `EG(class_table)`.
   - Boot classes get a *baseline* captured at the **end of boot** (the first
     request claim — `zealphp_request_input_set` / `zealphp_superglobals_owner`
     — before any request code runs; `zealphp_globals_baseline_refresh()`
     re-captures it). The baseline holds a shared reference, so any later
     in-place write COW-separates and the slot's payload pointer changes: an
     unchanged slot is a pointer compare. Changed slots are carried in the
     coroutine's lane **and re-parked at the baseline** (the post-boot
     template regime applied to boot classes; previously boot statics were
     copied every yield and never re-parked, so a peer could read another
     request's value first-touch). Request end resets boot classes to the
     baseline (PHP-FPM parity; was: never reset).
   - Static-property **write opcodes** are hooked (`ASSIGN_STATIC_PROP*`,
     `*INC/DEC_STATIC_PROP`, `FETCH_STATIC_PROP_W/RW/FUNC_ARG/UNSET`,
     `UNSET_STATIC_PROP`) to record the class (and its ancestors, which own
     inherited slots) per coroutine. The save visits only those classes; the
     request-end reset only the classes written since the last reset. A write
     whose class can't be resolved without autoload records "all" (full walk).
     Writes that bypass the VM (`ReflectionProperty::setValue` on a static,
     `zend_update_static_property` from C) are not seen until the next VM
     write.
   - `zealphp_shared_static_classes(array $prefixes)` (new): class-name
     prefixes whose statics are process-wide — never isolated, re-parked or
     reset. The framework registers `['ZealPHP\\']`: its statics are per-worker
     tables that concurrent requests share by design (route table, the
     pipelining turnstile `App::$connection_turns`, the fatal guard, memo
     caches). Before this, a coroutine's resume rewrote those arrays from its
     own stale snapshot, undoing concurrent requests' entries.

3. **Function statics (S5a).** The existing `BIND_STATIC` hook now also
   records the function in the running coroutine's *touched* set and in a
   worker-level *dirty* set. Save/restore walk the touched set (what this
   coroutine ran); the request-begin refresh and request-end reset walk the
   dirty set and clear it; a restore re-dirties what it re-applies.

4. **$GLOBALS (S2).** The global-variable write paths are hooked —
   `ZEND_BIND_GLOBAL`, `FETCH_W/RW/FUNC_ARG/UNSET` + `UNSET_VAR` with the
   GLOBAL fetch type (`$GLOBALS['x']`, `$$name` in global scope), `extract()`
   into a global-scope frame, and every CV of a Stage-8 `zealphp_require_global`
   frame — recording keys per coroutine and in a dirty set. The save visits
   the touched keys (capture + park + reset in one step; tombstone a baseline
   key the coroutine `unset()`), the resets visit the dirty keys. An
   unresolvable key records "all" (full one-pass walk, itself down from five
   walks). The restore-side belt-and-suspenders reset is skipped while the
   live table is known to be at the parent (`zealphp_globals_live_is_parent`,
   set by every reset, cleared when a coroutine starts or resumes). The
   stale-`global $K` repair (Phase R) scans the touched keys instead of the
   table.

5. **Lanes share instead of deep-copying.** Superglobals are *moved* into the
   lane on yield (the live slot is parked at the immutable empty array,
   `PG(http_globals)` in step) and moved back on resume — the owner's arrays
   keep their refcount, so the first write after a resume separates nothing.
   Class/function-static and $GLOBALS lanes hold a shared reference
   (`ZVAL_COPY`): a refcount ≥ 2 payload COW-separates on any write, so a lane
   can never observe a later mutation, and a slot that still shares the
   lane's payload is skipped on restore.

6. **Settings (S9).** `chdir`, `setlocale`, `umask`, `date_default_timezone_set`,
   `mb_internal_encoding`, `libxml_use_internal_errors` have their internal
   handlers wrapped to flag the calling coroutine; the per-yield getters, the
   close-time re-park and the request-end re-park run only for flagged
   coroutines (or lanes). Stage-internal calls are excluded.

7. **Per-request.** `zealphp_reset_request_rtcaches` keeps the vector of
   non-snapshot op_arrays (rebuilt on table change) instead of sweeping both
   tables with a hash probe per symbol: 47 µs → 0.2 µs.

## Results

Framework only (no app symbols), per switch: **70 µs → 1.8 µs** (1.1 park +
0.75 unpark); per request resets: 60 µs → ~2 µs.

| route (c=50, 1 worker) | coroutine | coroutine-legacy before | after |
|---|---|---|---|
| /ping (no yield) | 11.3k | 5.4k | 9.2k |
| /yield/5 | 13.1k | 1.4k | 9.3k |
| /yield/20 | 12.6k | 0.52k | 7.2k |

Synthetic app size (2000 classes with statics + 2000 static-using functions +
2000 globals, all instantiated at boot), per switch:

| stage | before | after |
|---|---|---|
| fn statics save + restore | 142 + 125 µs | 0.23 + 0.23 µs |
| $GLOBALS save + restore | 66 + 34 µs | 0.25 + 0.25 µs |
| class statics save | 27 µs | 0.14 µs |
| request-end: fn reset / class reset / globals end | 30 / 29 / 33 µs | 0.3 / 0.4 / 0.3 µs |
| **/yield/20 throughput** | **115 req/s** | **6,821 req/s** |

Per-switch cost no longer grows with the number of declared functions,
classes or globals; it grows with what the coroutine wrote since its last
yield. At N=2000 the switch is 1.24 + 0.90 µs, within noise of N=0. The
remaining linear terms are the Phase R frame walk (O(frames × CVs)) and the
cid→ptr bridge lookup on resume (O(live coroutines), ~50 at c=50).

## Correctness gates run

- ext phpt suite run the CI way (`php run-tests.php tests/`, OpenSwoole
  loaded so the coroutine-runtime tests execute): 91 pass / 0 fail / 1 skip.
  (`make test` runs `php -n` and silently skips the 42 coroutine tests — it
  hid the BIND_GLOBAL bug below until CI ran.)
- framework `TrustBarIsolationTest`, `CoroutineIsolationContractTest`,
  `CoroutineLegacyBehaviorTest`, `PhpInputIsolationTest`: 8/8, trust-bar at
  40-way concurrency with every primitive `iso`.

The first attempt captured boot baselines lazily at the first yield and the
trust-bar caught it (`class_static=LEAK2`): the first request had already
mutated the slot, so its view was never restored. Hence the end-of-boot
capture.

## Pitfall found by CI: repeatable opcodes

`ZEND_BIND_GLOBAL` is a *repeatable* opcode: the engine's handler loops over
the consecutive `BIND_GLOBAL`s of one `global $a, $b;` statement internally
(`ZEND_VM_REPEAT_OPCODE`), so a user opcode handler runs once per statement,
not once per name. The first version recorded only `$a`; `$b` was never
saved or reset and a peer's value leaked into it (test 063 `A:A/B`, and the
new test 055). The hook now records the whole consecutive run. The same
applies to `ZEND_BIND_STATIC`, which is harmless there because the
function-static hook records the op_array, not the variable.

## Round 2 and 3: skip what an untouched coroutine cannot need

Measured in a local Docker container (php:8.3-cli-bookworm, OpenSwoole
26.2, Apple-silicon VM, one worker, `ab -c 50`; absolute numbers are not
comparable with the lab tables above, ratios are).

Per switch with the profiler on (about 0.3 µs of each figure is the fourteen
timers themselves):

| stage | round 1 | round 3 |
|---|---|---|
| superglobals save + restore | 303 + 137 ns | 178 + 104 ns |
| process settings save + restore | 95 + 28 ns | 23 + 19 ns |
| class statics save + restore | 89 + 48 ns | 68 + 46 ns |
| fn statics save + restore | 147 + 121 ns | 19 + 18 ns |
| $GLOBALS save + restore | 241 + 184 ns | 97 + 66 ns |
| **park + unpark** | **1038 + 682 ns** | **578 + 419 ns** |

Throughput, profiler off, best of three:

| route | coroutine | coroutine-legacy round 1 | round 3 |
|---|---|---|---|
| /ping | ~20–26k | 14.8k | 16.5–17.5k |
| /yield/20 | ~20–21k | 10.3k | 15.2–15.9k |

What changed:

- **Owner gate once per park.** `zealphp_process_state_owner_ok()` is pure per
  coroutine; the park computes it once and the setting stages read the cached
  answer (was two hash probes in each of ~10 callers).
- **One gate for the six setter-driven setting stages.** A per-coroutine
  "holds any lane" flag plus the existing setter-touched flag decide in one
  probe whether cwd/locale/umask/tz/mbenc/libxml run at all, on save and on
  restore. last-error keeps its own `PG(last_error_message)` check.
- **Function statics under a shared prefix are process-shared**, like the
  static properties of those classes: `zealphp_shared_static_classes()`
  prefixes now also match function and method names (cached per op_array),
  so the framework's own helpers never enter the touched set. Semantics match
  plain coroutine mode, where they were never isolated.
- **$GLOBALS: nothing recorded, nothing done.** Superglobal keys are never
  recorded (S1 owns them). A coroutine with no recorded write returns before
  the frame walk and the lane allocation; a restore that finds no lane still
  re-parks a table a non-gated peer left dirty. Phase R skips when the
  coroutine bound nothing and finds the touched set through an O(1)
  ptr→cid bridge instead of scanning the cid→ptr table.
- **Superglobal lane:** interned keys (no rehash per move), precomputed
  `PG(http_globals)` indexes, pre-sized lane, and an immutable-empty
  superglobal is left in place: it already is the parked value.
- **Class statics:** classes under a shared prefix never enter the touched or
  dirty sets; an empty touched set skips the index refresh.

Attribution of the remaining gap (profiler off, feature subsets on top of
plain coroutine mode): superglobals + $GLOBALS + statics account for most of
the non-yield per-request cost (~3 µs on /ping); the per-switch remainder is
~0.7 µs, spread over the superglobal moves (7 hash finds + lane) and the
fixed probes of the park/unpark prologue.

Side finding: coroutine mode with `App::superglobals(true)` but without the
rest of the legacy flags (not an official mode) dies under concurrency with
`assert($app !== null)` at the request closure; `coroutine-legacy` itself,
which sets every flag, is unaffected.

## Rebased onto 0.4.1 (child lanes, deferred releases)

ext 0.4.1 landed while this branch was open and changed the same functions
(B1/B6 deferred releases, B3 COW snapshots, F12 child coroutines sharing their
request's lane). The branch was rebased commit by commit; what the rebase
had to reconcile:

- **Deferred releases (B1/B6).** Every value the stages displace inside a
  switch callback goes through `zealphp_release()` (the dead lists use
  `zealphp_deferred_zval_dtor`), including the touched-key save path and the
  class/function-static restores that share instead of deep-copying.
- **Child lanes (F12).** A `go()` child shares its request's lane, but the
  write hooks run in the child's coroutine. The touched sets are therefore
  keyed by the LANE's cid: a recording hook resolves child → lane task →
  lane cid (`zealphp_touch_cid()`), and the save side resolves its lane
  argument the same way (`zealphp_lane_touch_cid()`), because a lane is
  parked from other coroutines too (a child parking it, a capture at child
  start, an inline resumer parking the task it resumed over). A child that
  writes before its lane ever parked marks the lane "full walk"; the full
  0.4.1 walks also remain the path for lanes without a cid, for the "unknown
  key" marker and for capture-only saves. A first attempt gated the fast
  paths on "any child live in the worker", which disabled them for most of a
  busy worker's life (the framework keeps short-lived children around) and
  doubled the switch cost back up; the per-lane form has no such cliff.
- **One hidden merge bug.** The automatic merge had left a second
  `zend_hash_index_update(&zealphp_coro_globals_deltas, cid, &delta)` in the
  full-walk save; updating the same key with the same array releases that
  array, so every request's lane was empty on resume whenever the full walk
  ran (tests 057, 092–096 caught it). Removed.
- **Test 094** now expects the service object's destructor in request 1,
  where `$_SESSION = []` drops the last reference: the superglobal lane is
  moved, not shared, so it holds no extra reference and the owner's drop runs
  the destructor in the owner's coroutine (php-fpm semantics). 0.4.1's
  shared lane deferred it to the next request.

Gates on the rebased build: ext phpt 109/109 (CI-style, OpenSwoole loaded);
framework isolation integration 10/10 with every trust-bar primitive `iso`;
valgrind clean on 055/057/063/095/096 (093 reports an uninitialised read
inside OpenSwoole's own `coro_interrupt_function`, identical on pristine
0.4.1).

## Round 4: two blind spots closed, struct lane

Two write paths the touched sets could not see (both leaked across requests
in coroutine-legacy on rounds 1–3; both are covered by the full walks of
0.4.0/0.4.1 and by tests 110 and 111 now):

- **Global-scope includes outside `zealphp_require_global()`.** A file the
  entry file `include`s runs its top-level `$x = ...` as plain CV writes on
  the request's global frame — no write opcode fires. The compile-file hook
  now registers the compiled file's CVs for the lane when the including frame
  is the request's global frame (wp-settings.php and everything it pulls in);
  `eval()` on that frame records the every-key marker.
- **Reflection static writes.** `ReflectionProperty::setValue()`,
  `setRawValue()` and `ReflectionClass::setStaticPropertyValue()` write
  static slots from C. Their handlers are wrapped: a static write records the
  class (and its ancestors) for the lane; an instance write records nothing,
  so ORM-style hydrators do not pay.

Still invisible by construction: `zend_update_static_property()` called from
another C extension, and `$GLOBALS` writes performed by C code other than
`extract()`.

Perf: the superglobal lane is a per-coroutine struct of seven zvals instead
of a PHP array allocated and filled on every yield: superglobals save 160 →
86 ns; park 600 → 526 ns, unpark 437 → 428 ns (profiler on). The /yield/20
route runs at 16.2k req/s against 21.3k in plain coroutine mode, /ping at
17.2k against 19.0k.

`zealphp_switch_profile()` now also returns `touched_globals`,
`touched_classes` and `touched_functions` for the calling coroutine's lane,
so an app can see which of its writes the switch carries. For a framework
request they are all empty: the remaining per-stage cost is probes and the
profiler's own timers, not work.

## Complexity (measured 2026-11-01)

Per switch the path is **O(1) in app size and O(K) in the state the request
has written so far** (K = globals written + classes whose statics were written
+ functions whose `static` ran). Measured in the Docker bench
(`lab/bench/touched.php`, profiler on, 2000 yields per sample, averages in ns):

| written | K=0 | K=10 | K=100 | K=1000 | K=5000 |
|---|---|---|---|---|---|
| globals: park / unpark | 282 / 273 | 610 / 532 | 3374 / 2855 | 29970 / 22370 | 150286 / 114747 |
| class statics: park / unpark | 285 / 281 | 562 / 358 | 3118 / 1087 | 32867 / 11322 | 183144 / 64238 |
| function statics: park / unpark | 281 / 269 | 557 / 304 | 2927 / 616 | 36911 / 14038 | 316455 / 114506 |

So roughly 30 ns per written global and 33–37 ns per written class or
function on park, half to a third of that on unpark, and the per-request
cleanup is O(K) once (18 µs for 1000 globals; 400 µs for 5000 functions).
The K=0 constant is almost entirely the profiler's own `clock_gettime`
pairs (each idle stage shows the same 17 ns); with the profiler off the
whole legacy overhead on `/yield/20` is under 0.4 µs per switch.

Why the O(K) term is structural, not an implementation choice: every value a
request wrote sits in a table the VM reads *directly* (symbol table buckets,
`CE_STATIC_MEMBERS` slots, `static_variables_ptr` arrays), and PHP has no read
barrier on those paths, so a peer would see the leftover value unless it is
moved out before the peer runs. Moving K values is O(K). The two ways to make a
switch O(1) both change where the cost lands:

1. **Swap the containers by pointer** (per-coroutine `CG(map_ptr_base)`,
   per-coroutine symbol table header). The map_ptr base couples the static
   tables with every function's run-time cache, which caches raw `zval*` into
   those tables; the per-coroutine swap was prototyped and crashed under
   interleaving (`perf-o1-baseswap-research.md`, §6f). Making it safe needs an
   engine change (a separate base for statics, or self-healing rtcache
   slots), not an extension change.
2. **Read barriers** (user opcode handlers on the static-property fetch
   opcodes and on `$GLOBALS`/`global` reads, JIT off for them) so leftovers
   are swapped on first access by a different coroutine. That is O(1) per
   switch but adds ~20–40 ns to every static read, which costs more than the
   switch for any request with fewer than a few thousand written items.

For the apps legacy mode exists for (a few hundred globals, a few dozen static
classes, 5–20 yields per request) the O(K) term is 50–200 µs per request
against request times of tens of milliseconds, i.e. under 1 %.

## Not done / ideas

- Per-coroutine *symbol table swap* (true O(1) for $GLOBALS without hooks) —
  rejected for now: `&EG(symbol_table)` is an address held by frames and by
  `$GLOBALS` fetches, and swapping its contents under them is a different
  risk class.
- The ini stage still walks `EG(modified_ini_directives)` (O(modified ini),
  already small).
- A struct lane for the superglobals (seven zvals in one record instead of a
  PHP array per yield) would take another ~100 ns off the switch.
- `zealphp_fn_statics_refresh_one` still deep-copies template values per
  reset (O(vars of dirty functions)).

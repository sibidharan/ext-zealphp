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

## Not done / ideas

- Per-coroutine *symbol table swap* (true O(1) for $GLOBALS without hooks) —
  rejected for now: `&EG(symbol_table)` is an address held by frames and by
  `$GLOBALS` fetches, and swapping its contents under them is a different
  risk class.
- The ini stage still walks `EG(modified_ini_directives)` (O(modified ini),
  already small).
- `zealphp_fn_statics_refresh_one` still deep-copies template values per
  reset (O(vars of dirty functions)).

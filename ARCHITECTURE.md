# ext-zealphp Architecture

> **Stage naming:** canonical isolation-stage names live in the framework repo at
> `docs/architecture/isolation-stages.md` — S1 Superglobals, S2 Globals,
> S3 Redeclare (3a opcodes / 3b define intercept / 3c compile CG-swap),
> S5 Function statics, S7 Include-once, S8 Global scope, S9 Process settings,
> S10 Constants, S11 Request resets, S12 Exit hook (S4/S6 retired tombstones).
> The "gen-1/gen-2" terms below are implementation GENERATIONS of S2, not stages.

Authoritative reference for the C internals — what each subsystem does, where the code lives, and why the design ended up this shape. Companion to the high-level README and to ZealPHP's `docs/architecture/state-isolation-reference.md`.

> Last updated 2026-05-28 · v0.3.8 · PHP 8.3 / 8.4 verified

---

## What this extension does

ext-zealphp gives a long-running PHP server (ZealPHP on OpenSwoole) **the parts of FPM's "fresh process per request" semantic that matter for compatibility, without actually forking a process**:

1. **Per-coroutine isolation** of `$_GET` / `$_POST` / `$_SESSION` / `$GLOBALS` / `define()` constants / `ini_set()` values / static properties — so two coroutines serving different requests don't race process-wide state.
2. **Per-request function-handler overrides** of ~50 PHP built-ins (`header`, `setcookie`, `session_*`, `exec`, …) — so legacy code calling those built-ins gets routed to ZealPHP's per-request response object instead of writing to a long-dead SAPI.
3. **Silent-redeclare hooks** — top-level `function foo() {}` / `class Bar {}` in legacy code that re-runs on request N+1 no longer fires `E_COMPILE_ERROR`. First declaration wins.

Everything is opt-in from PHP. With the extension loaded but no `zealphp_*` functions called, the engine behaves identically to vanilla PHP.

---

## File layout

| File | Role |
|---|---|
| `zealphp.c` | The entire implementation — ~1900 lines, single translation unit. Sections delimited by `── Title ──` banner comments. |
| `php_zealphp.h` | Module entry declarations + `PHP_FUNCTION` prototypes. Version macro lives here. |
| `config.m4` / `config.w32` | phpize / build-system glue. |
| `tests/*.phpt` | The phpt suite — one file per behaviour. Numbered chronologically. |
| `README.md` | User-facing install + API summary. |
| `ARCHITECTURE.md` | This file. |

The extension is intentionally a single `.c` file. Splitting it into per-subsystem .c files would buy ~10% navigational convenience and cost the static-analysis simplicity of "everything is in one TU, the compiler sees the whole picture." The banner-comment sections (search for `── ` in `zealphp.c`) are the unit of navigation.

---

## Subsystems, top to bottom

The list below mirrors the source-file order. Read it like a reading guide — each row maps to a section in `zealphp.c` you can jump to.

### 1. Function-handler override family — `zealphp.c:27–943`

Banner: `── Storage ──` through `── Restore ──`

**What:** A PHP-callable function (`zealphp_override(string $name, callable $cb)`) replaces a PHP built-in's C-level handler with our `zealphp_dispatch` thunk that calls back into PHP via the saved closure. `zealphp_restore($name)` puts the original handler back. `zealphp_restore_all()` does the lot.

**Why:** ZealPHP's `header()` and `session_start()` etc. need to write to per-request response and session objects living in coroutine context. The existing PHP built-ins write to SAPI globals that don't exist in OpenSwoole's worker. Overriding the handler lets legacy code call `header('Location: /next')` without modification — our thunk routes it to the per-coroutine response.

**Storage:**
- `zealphp_orig_handlers` (line 30) — `name → original zif_handler` ptr.
- `zealphp_callbacks` (line 33) — `name → zval(closure)`.

**Allowlist (`zealphp.c:722–796`):** Only ~50 functions ZealPHP needs can be overridden — no `eval`, no `assert`, no `unserialize`, no class manipulation. The matcher (`zealphp_is_allowed`, `zealphp.c:788`) does length-checked `memcmp` — no substring/prefix bypass.

**Restore protocol:** Saved handlers are kept in the persistent table and re-installed on `zealphp_restore` or `MSHUTDOWN`. (The security review flagged this as needing a compare-and-restore step to avoid clobbering downstream extensions that hooked the same slot after us — tracked as a HIGH finding for a follow-up release.)

**Public PHP API:**

| Function | Lines |
|---|---|
| `zealphp_override(string $name, callable $cb): bool` | `838–900` |
| `zealphp_restore(string $name): bool` | `902–930` |
| `zealphp_restore_all(): void` | `932–943` |

---

### 2. Per-coroutine superglobal isolation — `zealphp.c:35–67, 1005–1098`

Banner: `── Per-coroutine superglobal isolation ──` and `── Save/restore ──`

**What:** When `superglobals(true) + enableCoroutine(true)` is in play, multiple coroutines share the process-wide `$_GET` / `$_POST` / `$_SESSION` arrays. Without intervention, coroutine A's `$_SESSION['user_id'] = 99` is visible to coroutine B serving a different request. The extension swaps the contents of these arrays on yield/resume so each coroutine sees its own.

**Storage:**
- `zealphp_coro_snapshots` (line 38) — `cid → zval(array of 7 superglobal arrays)`.

**Hooking the scheduler:** OpenSwoole's `PHPCoroutine::set_on_yield` / `set_on_resume` / `set_on_close` are resolved at MINIT via `dlsym` against `dlopen(NULL, RTLD_LAZY)`. Three symbol-name variants are probed (clean C export, OpenSwoole 22/26 C++ mangled, Swoole C++ mangled) — see `zealphp.c:1700–1731`. Original callbacks are saved and chained, because `set_on_yield` *replaces* the callback — failing to chain would break OpenSwoole's own EG/CG context switch and SIGSEGV.

**Save/restore (`zealphp.c:1005–1098`):**
- `zealphp_superglobals_save(cid)` — snapshot the 7 superglobals into `zealphp_coro_snapshots[cid]`.
- `zealphp_superglobals_restore(cid)` — load the snapshot back into the live superglobals.
- `zealphp_superglobals_clear()` — zero the live arrays (between requests).

The yield/resume hook (`zealphp_on_yield` / `zealphp_on_resume`, `zealphp.c:672–718`) save before yielding, restore after resuming. The close hook deletes the snapshot.

**Public PHP API:**

| Function | Behaviour |
|---|---|
| `zealphp_superglobals_set($get, $post, $cookie, $server, $files, $request, $session)` | Wholesale-replace live superglobals (per-request entry point) |
| `zealphp_superglobals_clear()` | Reset live superglobals to empty |
| `zealphp_superglobals_save($cid)` / `zealphp_superglobals_restore($cid)` | Manual snapshot / restore |
| `zealphp_coroutine_superglobals(bool)` | Toggle the yield/resume auto-snapshot |

---

### 3. Per-coroutine constant + ini + static-property isolation — `zealphp.c:117–334`

Banner: `── Level X: Per-coroutine X isolation ──` (three of them)

**What:** Same pattern as superglobals but for three other process-wide state categories:

- **`define()` constants** — `zealphp_coro_constant_snapshots` (line 62), helpers at `117–202`.
- **`ini_set()` values** — `zealphp_coro_ini_snapshots` (line 65), helpers at `210–259`.
- **Static class properties** — `zealphp_coro_static_snapshots` (line 67), helpers at `266–334`.

**How:** At yield, walk the relevant Zend table (`EG(zend_constants)` / `EG(ini_directives)` / class entries) and copy any user-set values into the snapshot. At resume, restore.

**Why not S2-style COW like $GLOBALS?** These categories don't accumulate writes the way `$GLOBALS` does — constants are write-once, `ini_set` calls are rare, static-property mutations are bounded. Deep-copy at yield is acceptable.

**Public PHP API:**

| Function | Behaviour |
|---|---|
| `zealphp_constants_clear()` | Clear user-defined constants (between requests) |
| `zealphp_ini_restore()` | Restore `ini_set()` mutations to file defaults |
| `zealphp_define_hook()` | Track `define()` calls into the per-coroutine snapshot |

---

### 4. Per-coroutine `$GLOBALS` — canonical stage **S2 Globals** (gen-1 deep-copy → gen-2 COW) — `zealphp.c:70–712, 1151–1238`

Banner: `── Per-coroutine full $GLOBALS / EG(symbol_table) isolation ──`

**The problem:** `$GLOBALS['app_state'] = 'foo'` in coroutine A is read by coroutine B as `'foo'` mid-request. Classic race. Workaround was "use `$g->app_state`" — but that requires user-code discipline. The framework wanted automatic isolation.

**gen-1 (v0.3.6, REPLACED):** Deep-copy every non-superglobal slot of `EG(symbol_table)` per coroutine via `ZVAL_DUP`. Worked. Cost: O(N keys) memory per active coroutine.

**gen-2 (v0.3.7, CURRENT):** Copy-on-write parent + per-coroutine delta. Storage at `zealphp.c:91–97`:

- `zealphp_coro_globals_parent` — shared baseline HashTable. Snapshotted ONCE at activation. Read-mostly.
- `zealphp_coro_globals_deltas` — `cid → zval-array of slots the coroutine wrote that differ from parent`.
- `zealphp_coro_globals_tombstones` — `cid → zval-array of parent keys the coroutine `unset()`'d`. Stored as `IS_LONG 1` dummies because Zend's `IS_UNDEF` sentinel makes `ZEND_HASH_FOREACH` silently skip the entry.

**Save flow (`zealphp_globals_snapshot_save`, `zealphp.c:493–548`):**
1. Walk `EG(symbol_table)` non-SG keys. For each, compare against parent via `zealphp_globals_zval_identical` (`zealphp.c:365`). Emit to delta only if different.
2. Walk parent. Emit tombstone for any key absent from EG.
3. `zealphp_globals_reset_to_parent` (`zealphp.c:436`) — clear non-SG keys from EG, reinstall parent baseline.

**Restore flow (`zealphp_globals_snapshot_restore`, `zealphp.c:550–592`):**
1. `reset_to_parent` (belt-and-suspenders).
2. Apply `deltas[cid]` over baseline.
3. `zend_hash_del` each key in `tombstones[cid]`.

**Memory characteristics:** 50 coroutines × 5 unique writes each = ~2 MB peak. gen-1 was O(N × coros).

**The IS_UNDEF tombstone bug worth knowing about:** Early gen-2 stored tombstones as `IS_UNDEF` inside the delta array. `ZEND_HASH_FOREACH_*` macros silently skip `IS_UNDEF` because that's Zend's internal "deleted bucket" marker — tombstones were invisible. Fix at `zealphp.c:91`: separate tombstones HashTable with non-UNDEF dummies.

**Public PHP API:**

| Function | Behaviour |
|---|---|
| `zealphp_globals_snapshot()` | One-shot snapshot for non-coroutine modes |
| `zealphp_globals_clean()` | Reset to parent (mid-request cleanup) |
| `zealphp_coroutine_globals(bool)` | Toggle S2 Globals (gen-2 COW) on/off |

---

### 5. Function-table / class-table / require_once snapshot+clean — `zealphp.c:1262–1392`

Banner: `── Process-state snapshot/clean (FPM-style) ──`

**What:** For Mode 3 (sync, single-coroutine) and Mode 1 Pool: at boot, snapshot the names of every function, class, and `require_once`'d file. After each request, walk the live tables and remove anything not in the snapshot. Gives "fresh process per request" function/class table semantics without the fork cost.

**Storage:**
- `zealphp_snapshot_files`, `zealphp_snapshot_classes`, `zealphp_snapshot_functions` (`zealphp.c:1564–1566`).

**Critical guard at the framework layer:** Apps with their own Composer autoloader registered MUST NOT clean function/class tables — the autoloader's lazy class map would point at deleted classes. The framework's `SessionManager::safeForFunctionIsolation()` does the check; the extension just provides the primitive.

**Public PHP API:**

| Function | Behaviour |
|---|---|
| `zealphp_process_state_snapshot()` | Boot-time: capture name sets |
| `zealphp_process_state_clean(int $flags)` | Per-request: remove anything not snapshotted. `$flags` bitmask: 1=funcs, 2=classes, 4=includes, OR-able. |
| `zealphp_protect_classes($names)` | Mark specific classes as protected from cleanup |

---

### 6. Stage 3a silent-redeclare opcode hooks — `zealphp.c:1547–1645`

Banner: `── Stage 3a: silent-redeclare opcode hooks ──`

**The problem:** Legacy PHP code wrapping `function foo() {}` inside `if (PHP_VERSION_ID > 80000) { ... }` (a conditional declaration) re-runs on every request in a long-lived worker. Second request fires `E_COMPILE_ERROR: Cannot redeclare foo()`. FPM doesn't hit this because each request gets a fresh process.

**The hook:**
- `zealphp_declare_function_handler` (`zealphp.c:1583`) — looks up the target name (op1 of the opline) in `EG(function_table)`. If exists, `EX(opline)++` and return `ZEND_USER_OPCODE_CONTINUE` — silently skip the bind. If not, `ZEND_USER_OPCODE_DISPATCH` — fall through to the original handler.
- `zealphp_declare_class_handler` (`zealphp.c:1602`) — same for `CG(class_table)`.
- `zealphp_declare_class_delayed_handler` (`zealphp.c:1626`) — same for `ZEND_DECLARE_CLASS_DELAYED` (classes with parent that get bound after parent autoload).

**Registration (`zealphp.c:1748–1757`):** `zend_set_user_opcode_handler(ZEND_DECLARE_FUNCTION, ...)` and two siblings. Installation is unconditional in MINIT — handlers gate themselves on `zealphp_silent_redeclare_enabled`, so the cost when the feature is off is one branch per opcode.

**Stage 3a limitation:** Catches RUNTIME declarations (inside if/fn/method scope). Top-level (file-scope) declarations are bound at COMPILE time via `zend_register_top_func` — no runtime opcode. Stage 3c covers that.

**Public PHP API:**

| Function | Behaviour |
|---|---|
| `zealphp_silent_redeclare(?bool $on = true): bool` | Toggle. Returns previous state. |

---

### 7. Stage 3c compile-time silent-redeclare via CG-table swap — `zealphp.c:1647–1746`

Banner: `── Stage 3c: compile-time silent-redeclare via CG-table swap ──`

**The problem:** Top-level `function foo() {}` / `class Bar {}` at file scope are bound to `CG(function_table)` / `CG(class_table)` at COMPILE time by `zend_register_top_func` / `zend_register_top_class`. Stage 3a's opcode hook never sees them. Second include of the same file fires `E_COMPILE_ERROR`.

**Why the first attempt failed:** Naively walking the entire user symbol table per compile and detaching with refcount bumps gave O(N×M) cumulative cost across nested compiles (autoloader chains do M nested compiles per request, each walking N symbols). Workers timed out before serving any request — every cell in the 32-app sweep returned `X` (timeout). Lab disaster, rolled back to commit `226e9e3` for archival.

**The shipped design (`zealphp_compile_file_hook`, `zealphp.c:1681`):** Swap `CG(function_table)` / `CG(class_table)` pointers to scratch tables for the duration of `zend_compile_file`. Then restore + first-wins merge.

```c
real_fn = CG(function_table);                  // save
CG(function_table) = &scratch_fn;              // swap
result = zealphp_original_compile_file(...);   // compile writes to scratch
CG(function_table) = real_fn;                  // restore
// Merge scratch into real, first-wins
ZEND_HASH_FOREACH_STR_KEY_PTR(&scratch_fn, key, ptr) {
    if (!zend_hash_exists(real_fn, key))  zend_hash_add_ptr(real_fn, key, ptr);
    else                                  destroy_zend_function((zend_function*)ptr);
} ZEND_HASH_FOREACH_END();
```

**Why this works:**

- Compile WRITES via `CG(function_table)` — diverted to scratch, so `zend_register_top_func`'s dup-check sees an empty slot and never errors.
- Code lookups during compile use `EG(function_table)` — still the real global table, so internal-function resolution and earlier user definitions stay visible.
- Cost per compile: **O(K)** where K = symbols this file declares. Independent of total user-symbol count.
- Re-entrant safe: the swap is stack-local. Nested `compile_file` calls get their own scratch tables. Restore happens in reverse order on stack unwind.

**Cleanup:** First-wins merge inserts into real only when the key isn't there. Losers (compile-added duplicates whose name already lives in real) are torn down via `destroy_zend_function` / `destroy_zend_class` so no op_array body leaks. Scratch tables use NULL dtor — their internal bucket free doesn't double-free entries that were moved out.

**Registration (`zealphp.c:1758–1764`):**
```c
zealphp_original_compile_file = zend_compile_file;
zend_compile_file = zealphp_compile_file_hook;
```

`zend_compile_file` is a public function pointer (declared `ZEND_API` in `Zend/zend_globals.h`), so we can swap it at MINIT and the engine calls our wrapper for every compile.

---

### 8. MINIT / MSHUTDOWN / MINFO + module entry — `zealphp.c:1769–1846`

**MINIT** initializes all the persistent hash tables, resolves OpenSwoole's `dlsym` chain, and installs the opcode + compile-file hooks. Single source of truth for "what gets wired at boot."

**MSHUTDOWN is intentionally a no-op** (`zealphp.c:1791`). The comment explains: on PHP 8.5+ the shutdown order changed — `CG(function_table)` and refcounted objects (Closures stored in `zealphp_callbacks`) may already be freed by the time MSHUTDOWN runs. Calling `zend_hash_destroy` on tables whose zval dtors touch freed memory crashes the process at exit. The OS reclaims everything on process exit anyway. This matches what opcache and other core extensions do.

**MINFO** emits the `phpinfo()` table row.

**Module entry (`zealphp.c:1839–1846`):** Standard `zend_module_entry` with the function entries from the `zealphp_functions[]` table (`zealphp.c:1797–1822`).

---

## Function-call graph (the 50,000-foot view)

```
                ┌─────────────────────────────┐
                │  PHP user code              │
                │  (header(), session_start, │
                │   global $foo, etc.)        │
                └──────────────┬──────────────┘
                               │
              ┌────────────────┴───────────────┐
              ▼                                ▼
   ┌─────────────────────┐         ┌──────────────────────┐
   │ Overridden built-in │         │ Symbol-table reads   │
   │ → zealphp_dispatch  │         │ ($_GET, $GLOBALS)    │
   │   → user closure    │         │ → live arrays        │
   └─────────────────────┘         └──────────────────────┘
                                              ▲
                                              │  swapped at
                                              │  yield/resume
                                  ┌──────────────────────┐
                                  │ zealphp_on_yield     │
                                  │  → snapshot_save     │
                                  │ zealphp_on_resume    │
                                  │  → snapshot_restore  │
                                  └──────────────────────┘
                                              ▲
                                              │  registered via dlsym chain in MINIT
                                  ┌──────────────────────┐
                                  │ OpenSwoole scheduler │
                                  │ PHPCoroutine::*      │
                                  └──────────────────────┘
```

For Stage 3a/3c (silent-redeclare):

```
                ┌─────────────────────────────┐
                │ require '/some/file.php'    │
                └──────────────┬──────────────┘
                               │
                ┌──────────────▼──────────────┐
                │ zealphp_compile_file_hook   │   ← Stage 3c
                │  swap CG tables → scratch   │
                │  call original_compile_file │
                │   (compile writes scratch)  │
                │  restore CG tables          │
                │  merge first-wins           │
                └──────────────┬──────────────┘
                               │
                ┌──────────────▼──────────────┐
                │ Interpreter runs op_array   │
                │  hits ZEND_DECLARE_FUNCTION │
                │   → zealphp_declare_…       │   ← Stage 3a
                │      handler (skip if dup)  │
                └─────────────────────────────┘
```

---

## How to read the test suite

`tests/*.phpt` filename prefix tells you which subsystem:

| Range | Subsystem | Section above |
|---|---|---|
| 001–006 | Function overrides | §1 |
| 007–012 | Superglobals save/restore | §2 |
| 013–016 | Coroutine yield/resume | §2 |
| 017 | $GLOBALS S2 (gen-2 COW) | §4 |
| 018–019 | Stage 3a silent-redeclare | §6 |
| 020 | Stage 3c compile-time silent-redeclare | §7 |

Tests 007–016 are coroutine-runtime-dependent and skip-fail on the CLI test harness (no OpenSwoole scheduler) — they pass under the live framework integration tests.

Run a specific test:
```bash
TEST_PHP_EXECUTABLE=$(which php) make test TESTS="tests/018-silent-redeclare-function.phpt"
```

---

## Known design tradeoffs and current limitations

### Top-level limitation list (read before adding features)

| Item | Status | Notes |
|---|---|---|
| Persistent hash tables hold per-request refcounted zvals | HIGH security finding | `zealphp_callbacks` etc. are `persistent=1` but store request-heap closures. Latent UAF if usage moves to per-request registration. Fix: `persistent=0` + add `RINIT`/`RSHUTDOWN`. |
| Restore protocol overwrites later-installed handlers | HIGH security finding | `zealphp_restore` puts the saved pointer back unconditionally — clobbers extensions that hooked the slot after us. Fix: compare-and-restore. |
| `global $foo;` reference binding across yield | Documented limitation | S2 (Globals) swaps `EG(symbol_table)` contents on yield/resume; a `global` keyword's by-reference binding becomes stale. Workaround: use `$g->foo` (per-coroutine `RequestContext`). |
| User functions/classes are still process-wide | By design | `CG(function_table)` and `CG(class_table)` are shared. Autoloaders depend on this. Per-coroutine tables would defeat autoloading. S3a + S3c silent-redeclare are the pragmatic ceiling. |
| MSHUTDOWN is a no-op | Intentional | PHP 8.5+ shutdown ordering issue. OS reclaims memory at exit. Matches opcache. |

The security review at the bottom of `zealphp.c`'s git history (see commits `9b8111b`, `428ef60`, `892f979`) tracked these issues honestly — none are critical, but the HIGH-severity ones should land before any new public-API surface is added.

---

## Memory management — does it leak under long-running workloads?

ZealPHP workers can serve millions of requests over days without restart. ext-zealphp is on the hot path for nearly every one of those — superglobal save/restore on yield/resume, `$GLOBALS` deltas, function overrides, opcode hooks, etc. Memory pressure was an active concern during design.

**Four cleanup tiers, layered defense-in-depth.** Nothing accumulates past tier 4.

### Tier 1 — per-coroutine (when a single coroutine ends)

`zealphp_on_close()` (`zealphp.c` near line 708) deletes every per-`cid` entry:

- `zealphp_coro_snapshots[cid]` — superglobals zval-array
- `zealphp_coro_constant_snapshots[cid]` — user `define()` constants
- `zealphp_coro_ini_snapshots[cid]` — `ini_set()` overrides
- `zealphp_coro_static_snapshots[cid]` — static class properties
- `zealphp_coro_globals_deltas[cid]` + `zealphp_coro_globals_tombstones[cid]` — S2 Globals deltas

Each entry's `ZVAL_PTR_DTOR` fires → refcounts drop → engine frees the values. **Typical size: 1–10 KB per coroutine**. At 1000 coros/sec that's ~10 MB/sec churn, all reclaimed immediately. The COW design here matters: deltas only contain what the coroutine ACTUALLY wrote — not a full deep-copy of `$GLOBALS` per coroutine (which was gen-1's design and the reason it was replaced).

### Tier 2 — per-HTTP-request (framework explicitly calls cleanup)

ZealPHP's `CoSessionManager::__invoke` (or `SessionManager::__invoke` in superglobals mode) runs in a `finally` after each handler. It calls into the extension:

- `zealphp_globals_clean()` (`zealphp.c:1414`) — drops `$GLOBALS` user vars back to parent baseline.
- `zealphp_constants_clear()` (`zealphp.c:1469`) — clears user-defined constants.
- `zealphp_ini_restore()` (`zealphp.c:1487`) — undoes `ini_set()` calls.
- `zealphp_process_state_clean($flags)` (`zealphp.c:1305`) — Mode 1/3 only: removes added functions/classes/included files (when no app autoloader is registered).

Each of these walks the relevant Zend table, frees the per-request entries via the engine's standard dtors. The COW parent table is untouched — so the NEXT request starts from the same clean baseline.

### Tier 3 — per-worker (PHP RINIT/RSHUTDOWN cycle)

`PHP_RINIT_FUNCTION` and `PHP_RSHUTDOWN_FUNCTION` (`zealphp.c:1900+`) manage four `persistent=0` tables: `zealphp_orig_handlers`, `zealphp_callbacks`, `zealphp_request_constants`, `zealphp_globals_snapshot`. Under OpenSwoole's default config, RINIT fires once at worker boot and RSHUTDOWN once at worker exit — so these tables effectively live for the worker's lifetime. Under CLI / FPM / CGI, they cycle per PHP-request.

The RSHUTDOWN sequence is order-sensitive:

1. **Restore the `define()` handler** if `zealphp_define_hook(true)` was called — otherwise the intercept handler keeps writing into a just-destroyed table.
2. **Compare-and-restore the override handlers** so the dispatch thunk isn't installed on built-ins with no callback table behind it.
3. **Reset static-flag state** (`zealphp_globals_snapshotted` etc.) so the next request starts clean.
4. **`zend_hash_destroy` each per-request table** — `ZVAL_PTR_DTOR` releases every closure / zval refcount.

This ordering was hardened after the v0.3.9 security review flagged the original sequence as racing with in-flight dispatches. See `tests/021-security-restore-isolation.phpt` for the pinned regression.

### Tier 4 — periodic worker recycle (the safety net)

OpenSwoole's `max_request` setting (ZealPHP's typical config: 10,000–50,000) forks a new worker process after N requests. The old worker exits cleanly → OS reclaims the entire process address space → any leak in the C code or user PHP code is **bounded by the recycle threshold**. This is the same backstop FPM uses (`pm.max_requests`).

### What can still leak (and what we do about it)

- **Closure refcount cycles** in `zealphp_callbacks` if a user closure captures another closure that captures back. PHP's cycle collector runs every ~10,000 zval allocs and breaks these. No extension-side action needed.
- **OpenSwoole coroutine context** if a coroutine is hard-killed (server crash, OOM). Tier 4 covers this — process exit reclaims everything.
- **opcache shared memory** — persists across worker recycles. By design. `opcache_reset()` clears it; ZealPHP doesn't automate this.

### What the security review changed

The v0.3.9 follow-up moved THREE tables from `persistent=1` to RINIT/RSHUTDOWN — they had held request-heap closures and zend_string keys, which the persistent table outlived. Cross-request UAF window closed. The fourth table (`zealphp_orig_handlers`) joined the same lifecycle when the reviewer found that even raw `zif_handler` pointers could go stale across SAPI module-reload cycles.

The `zealphp_dispatch` thunk now `ZVAL_COPY`-pins the callback before invoking it, so a concurrent RSHUTDOWN destroying the callbacks table during an in-flight call no longer dangles the cb pointer. The Stage 3c compile-file hook wraps the original compile in `zend_try`/`zend_catch` so `E_COMPILE_ERROR` / OOM bailout still restores `CG(function_table)` properly.

**Net effect:** at idle, the extension's heap footprint is bounded by the worker's `max_request` count plus the size of the active coroutines' deltas. There is no monotonic growth across requests. The Docker lab has been left running 48+ hour soak tests on Modes 4/5 and RSS stays flat after the initial worker warm-up.

---

## Conventions

- **All persistent storage uses `static HashTable`** at file scope. No globals exposed via `php_zealphp.h`.
- **Banner-comment sections** (`── Title ──`) are the unit of navigation. Add a new banner when you add a new subsystem.
- **Public PHP functions** are at the bottom of `zealphp.c` (the `PHP_FUNCTION` block) and declared in `php_zealphp.h`. New ones must be added to both the prototype list AND the `zealphp_functions[]` entries array.
- **Memory safety:** every `zend_hash_init` either uses `ZVAL_PTR_DTOR` (engine handles cleanup) or NULL (we manage manually). Mixing them silently leaks; pick one per table.
- **Refcount discipline:** when stashing a `zend_function*` or `zend_class_entry*` outside its owning hash, bump the refcount. The engine's destructors gate on refcount reaching zero — leaving extra references prevents premature free. Stage 3c's failed first attempt is the cautionary tale (commit `226e9e3`).

---

## Cross-references

- **README.md** — install + user-facing API summary.
- **ZealPHP `docs/architecture/state-isolation-reference.md`** — the framework view of how this extension's features map to ZealPHP's 5 execution modes, plus the migration ladder of releases.
- **ZealPHP `.claude/CLAUDE.md`** — per-section descriptions of how the framework wires each ext-zealphp feature into request lifecycle (search for "ext-zealphp" or "zealphp_").
- **PHP internals reference** — `Zend/zend_compile.h`, `Zend/zend_vm.h`, `Zend/zend_API.h` (look for `ZEND_API` exports we depend on).

If you're adding a new subsystem, add a new section to this file AND extend `zealphp_functions[]` AND drop a numbered `tests/0NN-*.phpt` exercising it. Three places, every time.

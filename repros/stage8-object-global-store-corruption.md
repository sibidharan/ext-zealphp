# Stage-8 global-scope object globals + concurrent mysqlnd teardown → object-store corruption

**Status:** FIXED in 0.3.47 (root-caused + fixed 2026-06-11). Distinct from ext#44 (mysqlnd `orig_path` allocator shim) — that shim is loaded and does NOT fix this.

## Symptom
Under `MODE_COROUTINE_LEGACY` + `App::globalScopeInclude(true)` (Stage 8), a `require_once`-legacy app with a **process-global database connection object** (`$ydb`/`$wpdb`/`$pdo` at file scope) segfaults workers under concurrent load (~3-8 deaths per 6-way `xargs -P6` burst of 20 requests). Single-user / sequential traffic is 100% clean.

## Backtraces (4/4 identical signature — process-wide object store, not the DB code)
All crash in `zend_objects_store_put` @ `Zend/zend_objects_API.c:151` (the free-list pop), reached from *unrelated* later object creation in a peer coroutine:
- `zend_closure_new` (call_user_func dispatch)
- `zend_objects_clone_obj` (ZEND_CLONE)
- `user_stream_create_object` ← `fopen` (userspace stream wrapper)

i.e. a prior bad teardown poisons `EG(objects_store)`'s free list; the **next `new` in ANY coroutine** trips it. The DB object is the poisoner, not the victim site.

## Bisection matrix (zext, PHP 8.4.21, ext 0.3.46, ZealPHP v0.4.8, 6-way ×20 burst)
| Variant | Result |
|---|---|
| `$pdo`/`$mysqli` global, mysql, Stage 8 ON | **CRASH** (3-8 deaths/burst) |
| `$pdo` global, **sqlite**, Stage 8 ON | clean (0) |
| constants only, no DB | clean (0) |
| MODE_COROUTINE (superglobals false) | clean (0) |
| **`globalScopeInclude(false)`** | clean (0) |
| `$pdo` **function-scoped** (not a global) | clean (0) |
| `$pdo = null;` explicit unset | still CRASH |
| `coroutineGlobalsIsolation` (Stage 2) OFF | still CRASH |
| **mixed mode** (sequential, no coroutine overlap) | clean (0) |

**Trigger = Stage 8 ON + object-valued FILE-SCOPE global + mysqlnd-family teardown + concurrency.** All three of {Stage 8, global (not function) scope, mysqlnd} are necessary.

## Root cause (engine internals)
`zealphp_require_global()` pushes a `ZEND_CALL_TOP_CODE` frame with `symbol_table = &EG(symbol_table)` — the ONE process-wide table, shared by every coroutine's global-scope frame. `zend_attach_symbol_table()` makes each top-frame CV (`$pdo`) an `IS_INDIRECT` bucket in `EG(symbol_table)` pointing at **that coroutine's frame CV slot**; the object zval lives in the frame slot.

When two coroutines run the global-scope include concurrently (front-controller apps run the WHOLE request inside it, so the frame never leaves until request end), the second coroutine's `zend_attach_symbol_table` finds the first's live `IS_INDIRECT` "pdo" bucket and **`ZVAL_COPY_VALUE`s the object pointer (no addref) into its own frame slot, then repoints the bucket**. Two frame CVs now own one object at refcount 1. Whichever unwinds/overwrites first runs the destructor → frees the mysqlnd connection + recycles the object handle → the survivor's handle dangles → object-store free-list poisoned.

The per-coroutine `$GLOBALS` isolation (0.3.23, object-aware) operates on the symbol-table **bucket** and explicitly **skips `IS_INDIRECT` non-baseline slots** (`if (!pv && indirect) continue;`, the #10/#033 master-frame-CV guard). Stage-8 object globals ARE exactly that shape, so they are never isolated. A bucket-based isolation cannot fix it: after attach, each coroutine reads its OWN frame CV directly, but the bucket can only point at one frame — a restore-through-bucket would write into the wrong coroutine's frame slot.

## Fix (shipped in 0.3.47)
Implemented **option 3** below — frame-CV-addressed isolation. A per-coroutine registry (`zealphp_coro_indirect_objs`, cid→array(key→object)) captures each Stage-8 object global on yield (`snapshot_save` pass 1b) and NULLs its owning frame CV; on resume (`snapshot_restore` step 2b) the object is written back into the resuming coroutine's OWN frame CV — re-derived by walking its live execute_data chain for the top-code frame that declares the CV (`zealphp_find_request_frame_cv`) — and the shared bucket is repointed `IS_INDIRECT`→that slot. The discriminator `zealphp_indirect_in_request_frame()` walks the current coroutine's frames for one sharing `&EG(symbol_table)`, which naturally excludes the master boot frame (`$app`), so genuine master-frame CVs are never touched. `reset_to_parent` (the most-called, most-delicate function) is UNCHANGED. **Validated:** minimal repro 0 deaths (was 3-8/burst), real YOURLS 0 deaths, PDO-mysql + mysqli globals both 0 deaths, 58/58 phpt green, valgrind ERROR SUMMARY 0 errors.

## Original design options considered
Stage-8 writable global scope must not share one symbol table's frame-CV indirection across concurrent coroutines. Options:
1. **Per-coroutine global symbol table for the request's Stage-8 frame**, reconciled into `$GLOBALS` reads — heaviest, most correct.
2. **Continuously detach** Stage-8 top-frame object CVs into real `EG(symbol_table)` zvals (so the existing 0.3.23 object-global isolation, which works on real zvals, covers them) instead of leaving them as live `IS_INDIRECT` frame CVs for the request duration.
3. **Frame-CV-addressed isolation**: record the (key → resuming-frame CV slot) on save and write the restored value back into the correct coroutine's frame slot on resume (re-derived by walking the resuming coroutine's execute_data chain for the top-code frame whose `symbol_table == &EG(symbol_table)`), repointing the bucket back.

## Relationship to the documented boundary
This is the Stage-8 manifestation of the already-documented "**one connection per coroutine; don't share a single mysqlnd handle across coroutines**" rule (CLAUDE.md / `docs/db-connection-pool.md`). A process-global connection under Stage-8 is effectively shared across coroutines via the symbol-table-frame collision. Production answer today: use the per-coroutine `DbConnectionPool`, or run these specific apps in the sequential fallback modes (`legacy-cgi` / `mixed`) for concurrent load.

## Repro
`repros/stage8-object-global-store-corruption.sh` — builds a ~15-line YOURLS-class app (config.php with `define()`s + `public/index.php` doing `new PDO(mysql)`), boots it in coroutine-legacy + Stage 8, runs the 6-way burst, greps boot.log for `abnormal exit`. Reproduces in 1-2 rounds.

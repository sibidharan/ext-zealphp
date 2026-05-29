/*
 * ext-zealphp — per-request function overrides for long-running PHP servers.
 *
 * Replaces ~15 PHP built-in functions (header, session_start, exec, etc.)
 * with user-supplied callbacks so each coroutine/request gets its own
 * response/session state. Drop-in replacement for uopz_set_return() with
 * a much smaller attack surface — allowlist-only, no class manipulation.
 *
 * API:
 *   zealphp_override(string $name, callable $cb): bool
 *   zealphp_restore(string $name): bool
 *   zealphp_restore_all(): void
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "zend_vm.h"
#include "php_zealphp.h"
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

/* ZTS refusal. The extension uses process-wide `static` storage for state
 * (zealphp_request_tables_live, zealphp_silent_redeclare_enabled, the
 * persistent hash tables, the OpenSwoole dlsym function pointers, ...).
 * None of it is TSRM-managed. On ZTS builds two threads racing on those
 * statics produce UAFs and double-frees (HIGH from the v0.3.9 security
 * review). Rather than ship a half-thread-safe build that's hazardous in
 * Apache mpm-worker / mod_php-zts, fail at compile-time so the user knows
 * to use an NTS build of PHP (which is the production default for FPM,
 * OpenSwoole, and ZealPHP anyway). */
#ifdef ZTS
#error "ext-zealphp does not support ZTS PHP builds. Use NTS — PHP's production default for FPM / OpenSwoole / ZealPHP. See ARCHITECTURE.md."
#endif

/* ── Storage ─────────────────────────────────────────────────────────── */

/* original handler pointers, keyed by lowercase function name */
static HashTable zealphp_orig_handlers;

/* PHP callable callbacks, keyed by lowercase function name */
static HashTable zealphp_callbacks;

/* ── Per-coroutine superglobal isolation ─────────────────────────────── */

/* Per-coroutine snapshots: coro_id → zval (array of 6 superglobal arrays) */
static HashTable zealphp_coro_snapshots;
static bool zealphp_coro_hooks_active = false;

/* OpenSwoole C-level hook typedefs (resolved via dlsym at runtime) */
typedef void (*coro_switch_fn_t)(void (*)(void *));
typedef long (*coro_get_cid_fn_t)(void);

static coro_switch_fn_t os_set_on_yield  = NULL;
static coro_switch_fn_t os_set_on_resume = NULL;
static coro_switch_fn_t os_set_on_close  = NULL;
static coro_get_cid_fn_t os_get_cid      = NULL;

/* Original OpenSwoole callbacks — must be chained, not replaced.
 * set_on_yield REPLACES the callback; PHPCoroutine::on_yield handles
 * PHP executor context switching (EG/CG swap). Without chaining,
 * PHP state is corrupted on coroutine resume → SIGSEGV. */
typedef void (*coro_callback_fn_t)(void *);
static coro_callback_fn_t orig_on_yield  = NULL;
static coro_callback_fn_t orig_on_resume = NULL;
static coro_callback_fn_t orig_on_close  = NULL;

/* True once our on_yield/on_resume/on_close wrappers have been installed
 * (and chained to OpenSwoole's originals). Guards the single install path
 * shared by all three isolation features. */
static bool zealphp_coro_wrappers_installed = false;

static const char *sg_names[] = {"_GET","_POST","_COOKIE","_SERVER","_FILES","_REQUEST","_SESSION", NULL};

/* Per-coroutine constant snapshots: coro_id → HashTable(name → zval value) */
static HashTable zealphp_coro_constant_snapshots;

/* Per-coroutine ini_set snapshots: coro_id → HashTable(name → string value) */
static HashTable zealphp_coro_ini_snapshots;

/* Per-coroutine static property snapshots: coro_id → HashTable(class_name → zval[]) */
static HashTable zealphp_coro_static_snapshots;

/* Per-coroutine FUNCTION-local static snapshots: coro_id → HashTable(
 * (zend_ulong) op_array->static_variables [template ptr, stable & unique per
 * function/method with statics] → HashTable(var_name → zval value) ).
 * Opt-in (heavier walk than the others) — Stage 5, gated behind the flag. */
static HashTable zealphp_coro_fn_static_snapshots;
static bool zealphp_fn_statics_active = false;

/* Touched-set registry: (zend_ulong) op_array->static_variables (template ptr)
 * → zend_op_array*. Populated by the ZEND_BIND_STATIC opcode hook as functions
 * first instantiate their statics, so the per-yield snapshot iterates ONLY the
 * functions that actually use statics (a small fraction of the table) instead
 * of walking every function + method. Worker-global; NEVER cleared per request
 * (only on explicit deactivation / worker shutdown). Stores only stable,
 * table-resident op_arrays (named functions + methods) — closures and
 * eval/top-level code are excluded because their op_arrays have per-instance
 * heap lifetime and would dangle. This is exactly the population the full-table
 * walk covers, so it's semantic parity. */
static HashTable zealphp_fn_static_registry;
typedef int (*zealphp_user_opcode_t)(zend_execute_data *);
static zealphp_user_opcode_t zealphp_prev_bind_static = NULL;
static bool zealphp_bind_static_installed = false;
static int zealphp_bind_static_handler(zend_execute_data *execute_data); /* fwd */

/* Previous user-opcode handlers, captured at MINIT before we install ours, so
 * we CHAIN instead of clobbering a coexisting profiler/instrumentation
 * extension (uopz, datadog, blackfire, …). Every fall-through path (feature
 * gated off, or we chose not to act) must defer to the prior handler rather
 * than blindly DISPATCH — DISPATCH skips the prior handler entirely. The paths
 * where WE own the decision (skip a duplicate decl via opline++ + CONTINUE)
 * are terminal and do not chain. */
static zealphp_user_opcode_t zealphp_prev_declare_function = NULL;
static zealphp_user_opcode_t zealphp_prev_declare_class = NULL;
static zealphp_user_opcode_t zealphp_prev_declare_class_delayed = NULL;
static zealphp_user_opcode_t zealphp_prev_include_eval = NULL;

/* Defer to a captured prior handler, or DISPATCH the original opcode if none. */
static zend_always_inline int zealphp_chain_or_dispatch(
        zealphp_user_opcode_t prev, zend_execute_data *execute_data)
{
    return prev ? prev(execute_data) : ZEND_USER_OPCODE_DISPATCH;
}

/* ── Per-coroutine full $GLOBALS / EG(symbol_table) isolation ───────── */

/* Per-coroutine $GLOBALS Stage 2 (COW deltas). Activated by
 * zealphp_coroutine_globals(true).
 *
 * Design:
 *   - At first activation we deep-copy EG(symbol_table) into a shared parent
 *     HashTable (zealphp_coro_globals_parent). All coroutines share it.
 *   - On yield, compute DELTA = entries that differ from parent (adds +
 *     overrides) stored in zealphp_coro_globals_deltas[cid].
 *     Keys present in parent but absent from EG are stored as a TOMBSTONE
 *     SET in zealphp_coro_globals_tombstones[cid] (key presence = tombstone;
 *     values are dummy IS_LONG 1 so ZEND_HASH_FOREACH never skips them).
 *     We cannot encode tombstones inside the delta array as IS_UNDEF zvals
 *     because ZEND_HASH_FOREACH_STR_KEY_VAL silently skips IS_UNDEF slots —
 *     that is Zend's own marker for a deleted hash bucket.
 *   - After saving, reset EG to parent baseline so the next coroutine
 *     (including ones launched inline via Coroutine::create) starts clean.
 *   - On resume: reset EG → parent, apply delta overrides, delete tombstones.
 *
 * Memory: parent (1×) + N × O(delta_keys) instead of N × O(all_keys). */
static HashTable zealphp_coro_globals_deltas;      /* cid → zval-array (live overrides) */
static HashTable zealphp_coro_globals_tombstones;  /* cid → zval-array (set of deleted keys) */
static bool zealphp_coro_globals_hooks_active = false;

/* Shared parent snapshot. */
static HashTable zealphp_coro_globals_parent;
static bool zealphp_coro_globals_parent_set = false;

/* Stage 3/4 silent-redeclare master flag. Hoisted to file-scope here
 * (vs co-located with the Stage 3 storage block farther down) so the
 * zealphp_define_intercept hook below can read it. */
static bool zealphp_silent_redeclare_enabled = false;

/* Stage 7: include_isolation. When enabled, the ZEND_INCLUDE_OR_EVAL
 * opcode handler converts require_once/include_once to require/include
 * for files NOT in the snapshot — so per-request code re-executes while
 * bootstrap code stays cached. Zero cleanup needed per-request. */
static bool zealphp_include_isolation_enabled = false;

/* Stage 7 once-per-request guard: set of files force-re-included this request,
 * keyed by coroutine id (os_get_cid). require_once's contract is "once per
 * process"; Stage 7 relaxes it to "once per REQUEST" (re-execute across
 * requests). Without a per-request guard, a re-entrant or circular require_once
 * within ONE request would be force-re-included repeatedly → infinite
 * re-inclusion → 1 GB OOM + worker crash (phpmyadmin's sodium_compat autoload,
 * nextcloud). A monotonic cid per request means each request starts with a
 * fresh (absent) set; on_close drops the entry when the coroutine ends. */
static HashTable zealphp_coro_reincluded;

/* ── Per-request define() isolation ─────────────────────────────────── */

/* Track constants defined during the current request so they can be
 * removed on request end. Keys = constant name (case-sensitive zend_string). */
static HashTable zealphp_request_constants;
static bool zealphp_define_hooked = false;
static zif_handler zealphp_orig_define_handler = NULL;

/* ── Per-request $GLOBALS isolation ─────────────────────────────────── */

/* Snapshot of EG(symbol_table) keys at boot. Keys added after the snapshot
 * are considered request-scoped and removed by zealphp_globals_clean(). */
static HashTable zealphp_globals_snapshot;
static bool zealphp_globals_snapshotted = false;

/* Save request-scoped constants for this coroutine. Copies the constant
 * values from EG(zend_constants) and removes them so the next coroutine
 * starts with a clean constant table (only boot-time constants remain). */
static void zealphp_constants_snapshot_save(long cid)
{
    if (!zealphp_define_hooked || zend_hash_num_elements(&zealphp_request_constants) == 0) {
        return;
    }

    zval snapshot;
    array_init(&snapshot);

    zend_string *name;
    ZEND_HASH_FOREACH_STR_KEY(&zealphp_request_constants, name) {
        if (name) {
            zend_constant *c = zend_hash_find_ptr(EG(zend_constants), name);
            if (c) {
                zval val_copy;
                ZVAL_DUP(&val_copy, &c->value);
                zend_hash_update(Z_ARRVAL(snapshot), name, &val_copy);
                /* Remove from EG so next coroutine doesn't see it */
                zend_hash_del(EG(zend_constants), name);
            }
        }
    } ZEND_HASH_FOREACH_END();

    /* Also save the tracked names set so we know which to restore */
    zval names_snapshot;
    array_init(&names_snapshot);
    ZEND_HASH_FOREACH_STR_KEY(&zealphp_request_constants, name) {
        if (name) {
            zval one;
            ZVAL_LONG(&one, 1);
            zend_hash_update(Z_ARRVAL(names_snapshot), name, &one);
        }
    } ZEND_HASH_FOREACH_END();

    /* Store both under this coroutine's ID */
    zval pair;
    array_init(&pair);
    zend_hash_str_update(Z_ARRVAL(pair), "values", sizeof("values") - 1, &snapshot);
    zend_hash_str_update(Z_ARRVAL(pair), "names", sizeof("names") - 1, &names_snapshot);
    zend_hash_index_update(&zealphp_coro_constant_snapshots, (zend_ulong)cid, &pair);

    /* Clear the process-wide tracker */
    zend_hash_clean(&zealphp_request_constants);
}

/* Restore this coroutine's request-scoped constants into EG(zend_constants). */
static void zealphp_constants_snapshot_restore(long cid)
{
    zval *pair = zend_hash_index_find(&zealphp_coro_constant_snapshots, (zend_ulong)cid);
    if (!pair || Z_TYPE_P(pair) != IS_ARRAY) return;

    zval *values = zend_hash_str_find(Z_ARRVAL_P(pair), "values", sizeof("values") - 1);
    zval *names = zend_hash_str_find(Z_ARRVAL_P(pair), "names", sizeof("names") - 1);
    if (!values || Z_TYPE_P(values) != IS_ARRAY) return;

    /* Re-register each constant in EG(zend_constants) */
    zend_string *name;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(values), name, val) {
        if (name && !zend_hash_exists(EG(zend_constants), name)) {
            zend_constant c;
            ZVAL_DUP(&c.value, val);
            c.name = zend_string_copy(name);
            ZEND_CONSTANT_SET_FLAGS(&c, 0, PHP_USER_CONSTANT);
            /* On failure (e.g. a constant of this name appeared between the
             * exists() check above and now), zend_register_constant releases
             * both c.name and c.value itself — nothing to clean up here (M5). */
            (void) zend_register_constant(&c);
        }
    } ZEND_HASH_FOREACH_END();

    /* Restore the tracked names into the process-wide tracker */
    if (names && Z_TYPE_P(names) == IS_ARRAY) {
        zend_hash_clean(&zealphp_request_constants);
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(names), name, val) {
            if (name) {
                zval one;
                ZVAL_LONG(&one, 1);
                zend_hash_update(&zealphp_request_constants, name, &one);
            }
        } ZEND_HASH_FOREACH_END();
    }
}

/* Clean up this coroutine's constant snapshot (on close). */
static void zealphp_constants_snapshot_delete(long cid)
{
    zend_hash_index_del(&zealphp_coro_constant_snapshots, (zend_ulong)cid);
}

/* ── Level 3: Per-coroutine ini_set isolation ──────────────────────── */

/* Save modified ini entries for this coroutine and restore originals.
 * EG(modified_ini_directives) tracks entries changed via ini_set().
 * We save the current values and restore orig_value so the next
 * coroutine sees unmodified ini state. On resume, re-apply. */
static void zealphp_ini_snapshot_save(long cid)
{
    if (!EG(modified_ini_directives) ||
        zend_hash_num_elements(EG(modified_ini_directives)) == 0) {
        return;
    }

    zval snapshot;
    array_init(&snapshot);

    zend_ini_entry *ini_entry;
    zend_string *name;
    ZEND_HASH_FOREACH_STR_KEY_PTR(EG(modified_ini_directives), name, ini_entry) {
        if (name && ini_entry && ini_entry->value) {
            zval val;
            ZVAL_STR_COPY(&val, ini_entry->value);
            zend_hash_update(Z_ARRVAL(snapshot), name, &val);
            /* Restore original value. zend_alter_ini_entry_ex's first arg is
             * the entry name (zend_string*), not the entry pointer — older
             * code accidentally passed the entry struct, which was emitting
             * an -Wincompatible-pointer-types warning that hardens to error
             * on stricter PHP build flags. */
            if (ini_entry->orig_value) {
                zend_string *orig = zend_string_copy(ini_entry->orig_value);
                zend_alter_ini_entry_ex(name, orig,
                    ini_entry->modifiable, ZEND_INI_STAGE_RUNTIME, 1);
                zend_string_release(orig);
            }
        }
    } ZEND_HASH_FOREACH_END();

    zend_hash_index_update(&zealphp_coro_ini_snapshots, (zend_ulong)cid, &snapshot);
}

static void zealphp_ini_snapshot_restore(long cid)
{
    zval *snapshot = zend_hash_index_find(&zealphp_coro_ini_snapshots, (zend_ulong)cid);
    if (!snapshot || Z_TYPE_P(snapshot) != IS_ARRAY) return;

    zend_string *name;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(snapshot), name, val) {
        if (name && Z_TYPE_P(val) == IS_STRING) {
            zend_string *value = Z_STR_P(val);
            zend_alter_ini_entry(name, value,
                ZEND_INI_USER, ZEND_INI_STAGE_RUNTIME);
        }
    } ZEND_HASH_FOREACH_END();
}

static void zealphp_ini_snapshot_delete(long cid)
{
    zend_hash_index_del(&zealphp_coro_ini_snapshots, (zend_ulong)cid);
}

/* ── Level 2: Per-coroutine static property isolation ─────────────── */

/* Save static properties of user classes that have been accessed.
 * Only snapshots classes where CE_STATIC_MEMBERS(ce) is non-NULL
 * (lazy — unaccessed classes are skipped). */
static void zealphp_statics_snapshot_save(long cid)
{
    zval snapshot;
    array_init(&snapshot);
    bool has_statics = false;

    zend_string *class_name;
    zval *cls_zv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(class_table), class_name, cls_zv) {
        if (!class_name) continue;
        zend_class_entry *ce = Z_PTR_P(cls_zv);
        if (!ce || ce->type != ZEND_USER_CLASS) continue;
        if (ce->default_static_members_count == 0) continue;

        zval *statics = CE_STATIC_MEMBERS(ce);
        if (!statics) continue;

        /* Snapshot each static property value */
        zval class_snapshot;
        array_init_size(&class_snapshot, ce->default_static_members_count);
        for (int i = 0; i < ce->default_static_members_count; i++) {
            zval copy;
            ZVAL_DUP(&copy, &statics[i]);
            zend_hash_index_add_new(Z_ARRVAL(class_snapshot), i, &copy);
        }
        zend_hash_update(Z_ARRVAL(snapshot), class_name, &class_snapshot);
        has_statics = true;
    } ZEND_HASH_FOREACH_END();

    if (has_statics) {
        zend_hash_index_update(&zealphp_coro_static_snapshots, (zend_ulong)cid, &snapshot);
    } else {
        zval_ptr_dtor(&snapshot);
    }
}

static void zealphp_statics_snapshot_restore(long cid)
{
    zval *snapshot = zend_hash_index_find(&zealphp_coro_static_snapshots, (zend_ulong)cid);
    if (!snapshot || Z_TYPE_P(snapshot) != IS_ARRAY) return;

    zend_string *class_name;
    zval *class_snapshot;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(snapshot), class_name, class_snapshot) {
        if (!class_name || Z_TYPE_P(class_snapshot) != IS_ARRAY) continue;

        zval *cls_zv = zend_hash_find(CG(class_table), class_name);
        if (!cls_zv) continue;
        zend_class_entry *ce = Z_PTR_P(cls_zv);
        if (!ce || ce->type != ZEND_USER_CLASS) continue;

        zval *statics = CE_STATIC_MEMBERS(ce);
        if (!statics) continue;

        zval *val;
        zend_ulong idx;
        ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(class_snapshot), idx, val) {
            if ((int)idx < ce->default_static_members_count) {
                zval_ptr_dtor(&statics[idx]);
                ZVAL_DUP(&statics[idx], val);
            }
        } ZEND_HASH_FOREACH_END();
    } ZEND_HASH_FOREACH_END();
}

static void zealphp_statics_snapshot_delete(long cid)
{
    zend_hash_index_del(&zealphp_coro_static_snapshots, (zend_ulong)cid);
}

/* ── Stage 5: Per-coroutine FUNCTION-local static isolation ──────────────
 *
 * PHP 8 stores a function's runtime static vars in a per-process HashTable
 * resolved via ZEND_MAP_PTR_GET(op_array->static_variables_ptr); the
 * op_array->static_variables pointer is the immutable *template*. The earlier
 * attempt swapped CG(map_ptr_base) per coroutine — that corrupts run_time_cache
 * (CE-identity "X, X given" TypeErrors) and needs a coroutine-create hook the
 * scheduler doesn't expose. This instead rides the SAME snapshot/restore model
 * already proven for class statics, constants and ini: on yield, snapshot each
 * instantiated function's live static table; on resume, restore THIS
 * coroutine's values. Cooperative scheduling makes it correct — every coroutine
 * writes its statics after its own restore and reads them before its next
 * yield, so values never bleed across coroutines.
 *
 * Touches only the per-process live static HashTable (never run_time_cache,
 * never the map_ptr base), so it is opcache-mode-agnostic and crash-safe.
 *
 * static vars are stored as IS_REFERENCE once ZEND_BIND_STATIC binds them to a
 * CV (the executing frame's CV shares the slot), so we deref on save and
 * write-THROUGH the reference on restore — same discipline as the superglobal
 * snapshot. Clobbering the reference wrapper would desync the running CV. */

typedef void (*zealphp_opa_static_cb)(zend_op_array *opa, HashTable *live, void *ctx);

/* Visit every user function / method whose statics have been instantiated
 * (live table exists and differs from the immutable template). Inherited
 * non-overridden methods share one op_array/template, so the same template key
 * may be visited once — idempotent for both save and restore. */
static void zealphp_walk_fn_statics(zealphp_opa_static_cb cb, void *ctx)
{
    zval *zv;
    /* Global user functions. */
    ZEND_HASH_FOREACH_VAL(CG(function_table), zv) {
        zend_function *fn = Z_PTR_P(zv);
        if (!fn || fn->type != ZEND_USER_FUNCTION) continue;
        zend_op_array *opa = &fn->op_array;
        if (!opa->static_variables) continue;
        HashTable *live = ZEND_MAP_PTR_GET(opa->static_variables_ptr);
        if (!live || live == opa->static_variables) continue;
        cb(opa, live, ctx);
    } ZEND_HASH_FOREACH_END();

    /* Methods of user classes. */
    zval *czv;
    ZEND_HASH_FOREACH_VAL(CG(class_table), czv) {
        zend_class_entry *ce = Z_PTR_P(czv);
        if (!ce || ce->type != ZEND_USER_CLASS) continue;
        zend_function *mfn;
        ZEND_HASH_FOREACH_PTR(&ce->function_table, mfn) {
            if (!mfn || mfn->type != ZEND_USER_FUNCTION) continue;
            zend_op_array *opa = &mfn->op_array;
            if (!opa->static_variables) continue;
            HashTable *live = ZEND_MAP_PTR_GET(opa->static_variables_ptr);
            if (!live || live == opa->static_variables) continue;
            cb(opa, live, ctx);
        } ZEND_HASH_FOREACH_END();
    } ZEND_HASH_FOREACH_END();
}

/* Registry walk — the hot path. Visits ONLY the op_arrays the ZEND_BIND_STATIC
 * hook recorded (functions that actually instantiated statics), so per-yield
 * cost scales with static-using functions, not total functions. Re-derives the
 * live table each call (so a not-yet-instantiated entry is skipped, and a
 * re-instantiated table is picked up). */
static void zealphp_walk_fn_statics_registry(zealphp_opa_static_cb cb, void *ctx)
{
    zend_op_array *opa;
    ZEND_HASH_FOREACH_PTR(&zealphp_fn_static_registry, opa) {
        if (!opa || !opa->static_variables) continue;
        HashTable *live = ZEND_MAP_PTR_GET(opa->static_variables_ptr);
        if (!live || live == opa->static_variables) continue;
        cb(opa, live, ctx);
    } ZEND_HASH_FOREACH_END();
}

/* One-time registry seed at activation: add every already-instantiated
 * function/method static to the registry via the full walk. Closes the gap
 * where a static with a non-constant initializer was first-bound during
 * bootstrap BEFORE the BIND_STATIC hook went live (opcode 203
 * BIND_INIT_STATIC_OR_JMP then JMPs past 183 on every later call, so the hook
 * would never fire for it). The full walk only visits CG(function_table) +
 * class methods — all table-resident, no closures — so this adds nothing
 * unstable. */
static void zealphp_fn_static_seed_cb(zend_op_array *opa, HashTable *live, void *ctx)
{
    (void)live; (void)ctx;
    zend_ulong key = (zend_ulong)(uintptr_t)opa->static_variables;
    if (!zend_hash_index_exists(&zealphp_fn_static_registry, key)) {
        zend_hash_index_add_ptr(&zealphp_fn_static_registry, key, opa);
    }
}

static void zealphp_fn_statics_save_cb(zend_op_array *opa, HashTable *live, void *ctx)
{
    zval *outer = (zval *)ctx;
    zend_ulong key = (zend_ulong)(uintptr_t)opa->static_variables;

    zval inner;
    array_init(&inner);
    zend_string *vn;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(live, vn, val) {
        if (!vn) continue;
        zval *src = Z_ISREF_P(val) ? Z_REFVAL_P(val) : val;
        zval copy;
        ZVAL_DUP(&copy, src);
        zend_hash_update(Z_ARRVAL(inner), vn, &copy);
    } ZEND_HASH_FOREACH_END();
    /* update (not add_new): an op_array may be reached twice via inheritance. */
    zend_hash_index_update(Z_ARRVAL_P(outer), key, &inner);
}

static void zealphp_fn_statics_restore_cb(zend_op_array *opa, HashTable *live, void *ctx)
{
    zval *outer = (zval *)ctx;
    zend_ulong key = (zend_ulong)(uintptr_t)opa->static_variables;

    zval *inner = zend_hash_index_find(Z_ARRVAL_P(outer), key);
    if (!inner || Z_TYPE_P(inner) != IS_ARRAY) return;

    zend_string *vn;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(inner), vn, val) {
        if (!vn) continue;
        zval *slot = zend_hash_find(live, vn);
        if (!slot) {
            zval copy;
            ZVAL_DUP(&copy, val);
            zend_hash_add(live, vn, &copy);
            continue;
        }
        /* Write THROUGH an IS_REFERENCE so the executing frame's CV (which
         * shares the reference) sees this coroutine's value. */
        zval *target = Z_ISREF_P(slot) ? Z_REFVAL_P(slot) : slot;
        zval old;
        ZVAL_COPY_VALUE(&old, target);
        ZVAL_DUP(target, val);
        zval_ptr_dtor(&old);
    } ZEND_HASH_FOREACH_END();
}

static void zealphp_fn_statics_snapshot_save(long cid)
{
    zval outer;
    array_init(&outer);
    zealphp_walk_fn_statics_registry(zealphp_fn_statics_save_cb, &outer);
    if (zend_hash_num_elements(Z_ARRVAL(outer)) > 0) {
        zend_hash_index_update(&zealphp_coro_fn_static_snapshots, (zend_ulong)cid, &outer);
    } else {
        zval_ptr_dtor(&outer);
    }
}

static void zealphp_fn_statics_snapshot_restore(long cid)
{
    zval *outer = zend_hash_index_find(&zealphp_coro_fn_static_snapshots, (zend_ulong)cid);
    if (!outer || Z_TYPE_P(outer) != IS_ARRAY) return;
    zealphp_walk_fn_statics_registry(zealphp_fn_statics_restore_cb, outer);
}

static void zealphp_fn_statics_snapshot_delete(long cid)
{
    zend_hash_index_del(&zealphp_coro_fn_static_snapshots, (zend_ulong)cid);
}

/* ── Full $GLOBALS snapshot save/restore/delete ──────────────────────── */

/* Set of EG(symbol_table) keys we should NOT snapshot — the superglobals
 * are handled by the existing zealphp_coro_snapshots mechanism. Skipping
 * them here avoids duplicate work and keeps the two layers cleanly
 * separated: superglobals_on_yield owns the 7 SG slots, globals_on_yield
 * owns everything else (user globals, $GLOBALS-injected keys, etc.). */
static bool zealphp_globals_is_superglobal_key(const char *key, size_t len)
{
    for (const char **n = sg_names; *n; n++) {
        if (strlen(*n) == len && memcmp(*n, key, len) == 0) {
            return true;
        }
    }
    /* "GLOBALS" itself is a special self-referential alias — skip to
     * avoid infinite-recursion-style copies in the snapshot. PHP's
     * engine maintains GLOBALS as a reference to symbol_table. */
    if (len == sizeof("GLOBALS") - 1 && memcmp("GLOBALS", key, len) == 0) {
        return true;
    }
    return false;
}

/* Identity check used to decide whether a zval needs to be in the delta.
 * Returns true when `a` and `b` would be indistinguishable from PHP code
 * — same type, same scalar value, OR same refcounted storage pointer
 * (string/array/object). Two arrays with the same contents but different
 * arData pointers are NOT identical (matches PHP's `===` semantics for
 * objects and the COW refcount-share invariant for arrays/strings). */
static bool zealphp_globals_zval_identical(const zval *a, const zval *b)
{
    if (!a || !b) return false;
    if (Z_TYPE_P(a) != Z_TYPE_P(b)) return false;
    switch (Z_TYPE_P(a)) {
        case IS_NULL:
        case IS_TRUE:
        case IS_FALSE:
            return true;
        case IS_LONG:
            return Z_LVAL_P(a) == Z_LVAL_P(b);
        case IS_DOUBLE:
            return Z_DVAL_P(a) == Z_DVAL_P(b);
        case IS_STRING:
            /* Same zend_string pointer (COW share) OR same content. */
            if (Z_STR_P(a) == Z_STR_P(b)) return true;
            if (Z_STRLEN_P(a) != Z_STRLEN_P(b)) return false;
            return memcmp(Z_STRVAL_P(a), Z_STRVAL_P(b), Z_STRLEN_P(a)) == 0;
        case IS_ARRAY:
            /* Same arData pointer = COW share. Two arrays with identical
             * content but different arData are considered DIFFERENT here
             * so any user mutation lands in the delta. */
            return Z_ARRVAL_P(a) == Z_ARRVAL_P(b);
        case IS_OBJECT:
            return Z_OBJ_P(a) == Z_OBJ_P(b);
        case IS_RESOURCE:
            return Z_RES_P(a) == Z_RES_P(b);
        case IS_REFERENCE:
            return Z_REF_P(a) == Z_REF_P(b);
        default:
            return false;
    }
}

/* Stage 2 only isolates COPYABLE value globals (scalars, strings, arrays).
 * Objects, resources, and references have identity + lifecycle that cannot be
 * safely snapshot/restored by refcount juggling across coroutine yields:
 * ZVAL_DUP/ZVAL_COPY + the per-yield reset/reinstall can drive a shared
 * object/resource refcount to zero at the wrong point (firing a __destruct
 * mid-restore, re-entering PHP from a C scheduler callback) or double-free a
 * resource handle — a latent use-after-free of the same class as the Stage 6
 * compile-cache UAF. Such globals are left in EG untouched (process-shared),
 * matching the documented "process-level state is shared" boundary —
 * request-scoped objects belong in $g, not $GLOBALS. (Confirmed by the v0.3.12
 * security review, H1/H2.) */
static bool zealphp_globals_isolatable(zval *v)
{
    if (Z_TYPE_P(v) == IS_REFERENCE) {
        return false;  /* a $GLOBALS ref binds two slots; snapshotting one desyncs it */
    }
    switch (Z_TYPE_P(v)) {
        case IS_OBJECT:
        case IS_RESOURCE:
            return false;
        default:
            return true;
    }
}

/* Take the shared parent snapshot from current EG(symbol_table). Called
 * once when zealphp_coroutine_globals(true) flips on. Deep-copies every
 * non-superglobal user var so the parent owns its storage. */
static void zealphp_globals_parent_snapshot(void)
{
    if (zealphp_coro_globals_parent_set) return;
    if (!EG(symbol_table).nTableMask) return;

    zend_string *key;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(&EG(symbol_table), key, val) {
        if (!key) continue;
        if (zealphp_globals_is_superglobal_key(ZSTR_VAL(key), ZSTR_LEN(key))) {
            continue;
        }
        if (!zealphp_globals_isolatable(val)) continue;  /* leave objects/resources/refs shared */
        zval copy;
        ZVAL_DUP(&copy, val);
        zend_hash_add_new(&zealphp_coro_globals_parent, key, &copy);
    } ZEND_HASH_FOREACH_END();

    zealphp_coro_globals_parent_set = true;
}

/* Release the shared parent snapshot. Called on disable so the next enable
 * re-snapshots a fresh parent. Storage is freed via the table's ZVAL_PTR_DTOR. */
static void zealphp_globals_parent_clear(void)
{
    if (!zealphp_coro_globals_parent_set) return;
    zend_hash_clean(&zealphp_coro_globals_parent);
    zealphp_coro_globals_parent_set = false;
}

/* Reset EG(symbol_table) to the shared parent baseline:
 *   - Remove every non-superglobal key currently present.
 *   - Reinstall every parent key (refcount-shared via ZVAL_COPY).
 * Used by both snapshot_save (post-yield reset so the next coroutine
 * inherits clean state) and snapshot_restore (pre-delta cleanup). */
static void zealphp_globals_reset_to_parent(void)
{
    if (!EG(symbol_table).nTableMask) return;

    /* Pass 1: collect & remove non-SG keys. We cannot mutate while
     * iterating the hash table. */
    zend_string **to_delete = NULL;
    uint32_t delete_count = 0;
    uint32_t delete_cap = 32;
    to_delete = emalloc(sizeof(zend_string *) * delete_cap);

    zend_string *key;
    zval *rv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(&EG(symbol_table), key, rv) {
        if (key && !zealphp_globals_is_superglobal_key(ZSTR_VAL(key), ZSTR_LEN(key))
            && zealphp_globals_isolatable(rv)) {   /* leave objects/resources/refs in place */
            if (delete_count >= delete_cap) {
                delete_cap *= 2;
                to_delete = erealloc(to_delete, sizeof(zend_string *) * delete_cap);
            }
            zend_string_addref(key);
            to_delete[delete_count++] = key;
        }
    } ZEND_HASH_FOREACH_END();

    for (uint32_t i = 0; i < delete_count; i++) {
        zend_hash_del(&EG(symbol_table), to_delete[i]);
        zend_string_release(to_delete[i]);
    }
    efree(to_delete);

    /* Pass 2: reinstall parent baseline. */
    if (zealphp_coro_globals_parent_set) {
        zval *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(&zealphp_coro_globals_parent, key, val) {
            if (!key) continue;
            zval copy;
            ZVAL_COPY(&copy, val);
            zend_hash_add_new(&EG(symbol_table), key, &copy);
        } ZEND_HASH_FOREACH_END();
    }
}

/* Save EG(symbol_table) into per-coroutine delta + tombstone tables.
 *
 * Delta table  (zealphp_coro_globals_deltas[cid]):
 *   Keys whose value differs from parent (added or overridden).
 *   Stored via ZVAL_COPY — refcount-shared for arrays/strings/objects.
 *
 * Tombstone table (zealphp_coro_globals_tombstones[cid]):
 *   Keys present in parent but absent from EG (coroutine ran unset()).
 *   Values are IS_LONG 1 — only key membership matters.
 *   We use a SEPARATE table instead of encoding IS_UNDEF inside the delta
 *   because ZEND_HASH_FOREACH_STR_KEY_VAL silently skips IS_UNDEF slots
 *   (that type is Zend's internal "deleted bucket" marker).
 *
 * After saving, resets EG to parent baseline so the next coroutine
 * (even one started inline via Coroutine::create before any channel yield)
 * always inherits a clean parent state rather than leftover writes. */
static void zealphp_globals_snapshot_save(long cid)
{
    if (!EG(symbol_table).nTableMask) return;

    zval delta;
    array_init(&delta);

    /* Pass 1: record adds and overrides relative to parent. */
    zend_string *key;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(&EG(symbol_table), key, val) {
        if (!key) continue;
        if (zealphp_globals_is_superglobal_key(ZSTR_VAL(key), ZSTR_LEN(key))) continue;
        if (!zealphp_globals_isolatable(val)) continue;  /* objects/resources/refs stay shared */
        if (zealphp_coro_globals_parent_set) {
            zval *pv = zend_hash_find(&zealphp_coro_globals_parent, key);
            if (pv && zealphp_globals_zval_identical(val, pv)) continue;
        }
        zval copy;
        ZVAL_COPY(&copy, val);
        zend_hash_add_new(Z_ARRVAL(delta), key, &copy);
    } ZEND_HASH_FOREACH_END();

    zend_hash_index_update(&zealphp_coro_globals_deltas, (zend_ulong)cid, &delta);

    /* Pass 2: tombstone parent keys that are now absent from EG. */
    if (zealphp_coro_globals_parent_set) {
        zval tombstones;
        array_init(&tombstones);
        bool has_tombs = false;

        zval *pv;
        ZEND_HASH_FOREACH_STR_KEY_VAL(&zealphp_coro_globals_parent, key, pv) {
            (void)pv;
            if (!key) continue;
            if (zend_hash_exists(&EG(symbol_table), key)) continue;
            /* dummy value — key presence is the tombstone signal */
            zval one;
            ZVAL_LONG(&one, 1);
            zend_hash_add_new(Z_ARRVAL(tombstones), key, &one);
            has_tombs = true;
        } ZEND_HASH_FOREACH_END();

        if (has_tombs) {
            zend_hash_index_update(&zealphp_coro_globals_tombstones, (zend_ulong)cid, &tombstones);
        } else {
            zval_ptr_dtor(&tombstones);
        }
    }

    /* Reset EG to parent baseline so the next coroutine starts clean. */
    zealphp_globals_reset_to_parent();
}

/* Restore EG(symbol_table) for coroutine `cid`:
 *   1. Reset to parent baseline.
 *   2. Apply delta overrides (writes).
 *   3. Delete tombstoned keys. */
static void zealphp_globals_snapshot_restore(long cid)
{
    if (!EG(symbol_table).nTableMask) return;

    /* Step 1 — parent baseline (belt-and-suspenders: on_yield reset it too,
     * but on_resume may fire without a preceding on_yield if OpenSwoole
     * resumes a coroutine that was never explicitly yielded). */
    zealphp_globals_reset_to_parent();

    /* Step 2 — apply delta (adds / overrides). */
    zval *delta = zend_hash_index_find(&zealphp_coro_globals_deltas, (zend_ulong)cid);
    if (delta && Z_TYPE_P(delta) == IS_ARRAY) {
        zend_string *key;
        zval *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(delta), key, val) {
            if (!key) continue;
            zval *existing = zend_hash_find(&EG(symbol_table), key);
            if (existing) {
                zval old;
                ZVAL_COPY_VALUE(&old, existing);
                ZVAL_COPY(existing, val);
                zval_ptr_dtor(&old);
            } else {
                zval copy;
                ZVAL_COPY(&copy, val);
                zend_hash_add_new(&EG(symbol_table), key, &copy);
            }
        } ZEND_HASH_FOREACH_END();
    }

    /* Step 3 — delete tombstoned keys (parent keys this coroutine unset). */
    zval *tombs = zend_hash_index_find(&zealphp_coro_globals_tombstones, (zend_ulong)cid);
    if (tombs && Z_TYPE_P(tombs) == IS_ARRAY) {
        zend_string *key;
        zval *dummy;
        ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(tombs), key, dummy) {
            (void)dummy;
            if (!key) continue;
            zend_hash_del(&EG(symbol_table), key);
        } ZEND_HASH_FOREACH_END();
    }
}

/* Delete both per-coroutine tables on coroutine close. */
static void zealphp_globals_snapshot_delete(long cid)
{
    zend_hash_index_del(&zealphp_coro_globals_deltas,     (zend_ulong)cid);
    zend_hash_index_del(&zealphp_coro_globals_tombstones, (zend_ulong)cid);
}

static void zealphp_snapshot_save(long cid)
{
    /* Guard: EG(symbol_table) may be invalid during coroutine teardown.
     * Check that the table has a valid nTableMask (non-zero when initialized). */
    if (!EG(symbol_table).nTableMask) return;

    zval snapshot;
    array_init(&snapshot);
    for (const char **n = sg_names; *n; n++) {
        zval *sg = zend_hash_str_find(&EG(symbol_table), *n, strlen(*n));
        if (!sg) continue;
        /* $_GET et al. may be wrapped in IS_REFERENCE — ZealPHP's
         * RequestContext &__get() hands back $GLOBALS['_GET'] BY REFERENCE,
         * flipping the symbol-table slot to a reference. Deref so we snapshot
         * the underlying array. Without this the `== IS_ARRAY` gate skipped a
         * referenced superglobal entirely, dropping it on resume (the
         * "$_GET cleared after yield once code touches $g->get" leak). */
        zval *arr = Z_ISREF_P(sg) ? Z_REFVAL_P(sg) : sg;
        if (Z_TYPE_P(arr) == IS_ARRAY) {
            /* Deep copy the array to avoid sharing zvals with the live
             * symbol table — the original may be modified or freed between
             * yield and resume/close. */
            zval copy;
            ZVAL_DUP(&copy, arr);
            add_assoc_zval(&snapshot, *n, &copy);
        }
    }
    zend_hash_index_update(&zealphp_coro_snapshots, (zend_ulong)cid, &snapshot);
}

static void zealphp_snapshot_restore(long cid)
{
    zval *snapshot = zend_hash_index_find(&zealphp_coro_snapshots, (zend_ulong)cid);
    if (!snapshot || Z_TYPE_P(snapshot) != IS_ARRAY) return;

    for (const char **n = sg_names; *n; n++) {
        zval *val = zend_hash_str_find(Z_ARRVAL_P(snapshot), *n, strlen(*n));
        if (val) {
            /* In-place zval swap with ZVAL_DUP: same pattern as
             * zealphp_set_superglobal. Preserves the zval memory address
             * (CV cache compatible) AND creates a full copy with refcount=1
             * (no COW separation on write). */
            zval *existing = zend_hash_str_find(&EG(symbol_table), *n, strlen(*n));
            if (existing) {
                /* If the live slot is an IS_REFERENCE (a $g->get alias bound
                 * via &__get), update the value the reference points at rather
                 * than clobbering the reference wrapper — keeps the alias valid
                 * while restoring this coroutine's array. */
                zval *slot = Z_ISREF_P(existing) ? Z_REFVAL_P(existing) : existing;
                zval old;
                ZVAL_COPY_VALUE(&old, slot);
                ZVAL_DUP(slot, val);
                zval_ptr_dtor(&old);
            } else {
                zval copy;
                ZVAL_DUP(&copy, val);
                zend_hash_str_add_new(&EG(symbol_table), *n, strlen(*n), &copy);
            }
        }
    }
}

/* OpenSwoole scheduler callbacks — called from C, no PHP stack needed.
 * Use the Coroutine* arg pointer itself as the unique key — guaranteed
 * unique per coroutine, no need to extract cid from the struct. */
/* Per-coroutine identity in the scheduler callbacks: we key every snapshot hash
 * on (uintptr_t)arg — the OpenSwoole Coroutine* pointer passed to the callback —
 * NOT os_get_cid(). This is deliberate and load-bearing, not an oversight:
 *
 *   EMPIRICALLY VERIFIED (cid-probe, 3 concurrent coroutines, HOOK_ALL):
 *     on_yield : os_get_cid() == the yielding coroutine's cid   (reliable)
 *     on_resume: os_get_cid() == -1  ON EVERY RESUME            (UNRELIABLE)
 *     on_close : os_get_cid() == the closing coroutine's cid    (reliable)
 *
 * on_resume fires BEFORE the scheduler has installed the resuming coroutine as
 * "current", so os_get_cid() returns -1 there. Keying the snapshot RESTORE on
 * os_get_cid() would therefore look up hash[-1] for every resume and silently
 * restore nothing — cross-coroutine state corruption. The Coroutine* arg is the
 * ONLY identity that is correct in all three callbacks, so the snapshots use it.
 * Pointer reuse is not a hazard: on_close(arg) deletes this coroutine's snapshot
 * as part of teardown, before the engine can free and reassign the struct.
 *
 * Stage 7's reincluded set is the deliberate exception — it is populated from the
 * ZEND_INCLUDE_OR_EVAL opcode handler, which runs in PHP-execution context (NOT a
 * scheduler callback), where os_get_cid() IS the running coroutine's cid; and it
 * is cleaned in on_close, where os_get_cid() is also reliable (see above). It can
 * only ever see os_get_cid(), never arg, so it consistently keys on cid — a
 * separate hash from the snapshots, so the two schemes never collide. */
static void zealphp_on_yield(void *arg)
{
    if (!arg) return;
    zealphp_snapshot_save((zend_long)(uintptr_t)arg);
    zealphp_constants_snapshot_save((zend_long)(uintptr_t)arg);
    zealphp_ini_snapshot_save((zend_long)(uintptr_t)arg);
    zealphp_statics_snapshot_save((zend_long)(uintptr_t)arg);
    if (zealphp_fn_statics_active) {
        zealphp_fn_statics_snapshot_save((zend_long)(uintptr_t)arg);
    }
    /* Full $GLOBALS snapshot: runs AFTER superglobals save so the
     * snapshot reflects whatever the request handler last wrote. The
     * is_superglobal_key filter inside this call deliberately skips the
     * 7 SG slots — those are owned by zealphp_snapshot_save above. */
    if (zealphp_coro_globals_hooks_active) {
        zealphp_globals_snapshot_save((zend_long)(uintptr_t)arg);
    }
    /* Chain to OpenSwoole's PHPCoroutine::on_yield — handles EG/CG swap */
    if (orig_on_yield) orig_on_yield(arg);
}

static void zealphp_on_resume(void *arg)
{
    /* Chain to OpenSwoole's PHPCoroutine::on_resume FIRST — restores EG/CG
     * so EG(symbol_table) is valid when we read/write superglobals */
    if (orig_on_resume) orig_on_resume(arg);
    if (!arg) return;
    /* NB: os_get_cid() == -1 here (see the identity-rationale comment on
     * zealphp_on_yield) — we MUST key on arg, never os_get_cid(), in this path. */
    /* Full $GLOBALS restore runs BEFORE superglobals restore so that
     * the superglobals layer can overwrite the 7 SG slots last and win
     * any race against stale snapshot data. */
    if (zealphp_coro_globals_hooks_active) {
        zealphp_globals_snapshot_restore((zend_long)(uintptr_t)arg);
    }
    zealphp_snapshot_restore((zend_long)(uintptr_t)arg);
    zealphp_constants_snapshot_restore((zend_long)(uintptr_t)arg);
    zealphp_ini_snapshot_restore((zend_long)(uintptr_t)arg);
    zealphp_statics_snapshot_restore((zend_long)(uintptr_t)arg);
    if (zealphp_fn_statics_active) {
        zealphp_fn_statics_snapshot_restore((zend_long)(uintptr_t)arg);
    }
}

static void zealphp_on_close(void *arg)
{
    /* Chain to OpenSwoole's PHPCoroutine::on_close FIRST */
    if (orig_on_close) orig_on_close(arg);
    if (!arg) return;
    zend_hash_index_del(&zealphp_coro_snapshots, (zend_ulong)(uintptr_t)arg);
    zealphp_constants_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_ini_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_statics_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_fn_statics_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_globals_snapshot_delete((zend_long)(uintptr_t)arg);
    /* Stage 7: drop this coroutine's force-re-included set. Keyed by os_get_cid()
     * (the handler's key); during a coroutine's own close callback os_get_cid()
     * still returns its cid. Prevents the per-request set from accumulating
     * across the worker's lifetime (cids are monotonic). */
    if (os_get_cid) {
        zend_hash_index_del(&zealphp_coro_reincluded, (zend_ulong)os_get_cid());
    }
}

/* Resolve OpenSwoole's current Coroutine::on_yield/on_resume/on_close
 * callbacks via dlsym and chain our wrappers in front of them. These
 * originals (PHPCoroutine::on_*) perform the PHP executor context switch
 * (EG/CG swap) on every coroutine switch — we MUST chain to them.
 *
 * C2 (security review): if we cannot capture all three originals we REFUSE
 * to install. Calling os_set_on_*() with our wrappers but a NULL original
 * chain would skip OpenSwoole's executor context switching entirely and
 * silently corrupt every coroutine switch — far worse than the feature
 * being unavailable. Better to fail loudly (E_WARNING + return false) and
 * let the caller surface "isolation unavailable".
 *
 * Idempotent: once installed, later calls are a no-op returning true. The
 * single shared install path also de-duplicates what were four identical
 * dlsym blocks (M4). */
static bool zealphp_install_coro_hooks(void)
{
    if (zealphp_coro_wrappers_installed) {
        return true;
    }

    void *handle = dlopen(NULL, RTLD_LAZY);
    if (handle) {
        static const char *yield_var_names[] = {
            "_ZN10openswoole9Coroutine8on_yieldE",
            "_ZN6swoole9Coroutine8on_yieldE", NULL
        };
        static const char *resume_var_names[] = {
            "_ZN10openswoole9Coroutine9on_resumeE",
            "_ZN6swoole9Coroutine9on_resumeE", NULL
        };
        static const char *close_var_names[] = {
            "_ZN10openswoole9Coroutine8on_closeE",
            "_ZN6swoole9Coroutine8on_closeE", NULL
        };
        for (const char **n = yield_var_names; *n && !orig_on_yield; n++) {
            coro_callback_fn_t *p = (coro_callback_fn_t *)dlsym(handle, *n);
            if (p) orig_on_yield = *p;
        }
        for (const char **n = resume_var_names; *n && !orig_on_resume; n++) {
            coro_callback_fn_t *p = (coro_callback_fn_t *)dlsym(handle, *n);
            if (p) orig_on_resume = *p;
        }
        for (const char **n = close_var_names; *n && !orig_on_close; n++) {
            coro_callback_fn_t *p = (coro_callback_fn_t *)dlsym(handle, *n);
            if (p) orig_on_close = *p;
        }
        dlclose(handle);
    }

    if (!orig_on_yield || !orig_on_resume || !orig_on_close) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: could not resolve OpenSwoole's coroutine "
            "on_yield/on_resume/on_close callbacks to chain through. "
            "Refusing to install per-coroutine isolation hooks — replacing "
            "OpenSwoole's callbacks without chaining would corrupt coroutine "
            "context switching.");
        return false;
    }

    os_set_on_yield(zealphp_on_yield);
    os_set_on_resume(zealphp_on_resume);
    os_set_on_close(zealphp_on_close);
    zealphp_coro_wrappers_installed = true;
    return true;
}

/* ── Allowlist ───────────────────────────────────────────────────────── */

static const char *zealphp_allowed[] = {
    /* response */
    "header",
    "header_remove",
    "headers_list",
    "headers_sent",
    "setcookie",
    "setrawcookie",
    "http_response_code",
    "header_register_callback",
    /* output control */
    "flush",
    "ob_flush",
    "ob_end_flush",
    "ob_implicit_flush",
    "output_add_rewrite_var",
    "output_reset_rewrite_vars",
    /* process/connection */
    "set_time_limit",
    "ignore_user_abort",
    "connection_status",
    "connection_aborted",
    "register_shutdown_function",
    /* error handling */
    "error_log",
    "error_reporting",
    "set_error_handler",
    "restore_error_handler",
    "set_exception_handler",
    "restore_exception_handler",
    /* file upload */
    "is_uploaded_file",
    "move_uploaded_file",
    /* info */
    "phpinfo",
    "php_sapi_name",
    /* input filtering */
    "filter_input",
    "filter_input_array",
    /* environment — per-coroutine putenv/getenv via the request-scoped $g store */
    "putenv",
    "getenv",
    /* session */
    "session_start",
    "session_id",
    "session_status",
    "session_name",
    "session_write_close",
    "session_destroy",
    "session_unset",
    "session_regenerate_id",
    "session_get_cookie_params",
    "session_set_cookie_params",
    "session_cache_limiter",
    "session_cache_expire",
    "session_commit",
    "session_abort",
    "session_encode",
    "session_decode",
    "session_save_path",
    "session_module_name",
    /* exec family */
    "shell_exec",
    "exec",
    "system",
    "passthru",
    NULL
};

static bool zealphp_is_allowed(const char *name, size_t len)
{
    for (const char **p = zealphp_allowed; *p; p++) {
        if (strlen(*p) == len && memcmp(*p, name, len) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Generic handler ─────────────────────────────────────────────────── */

static ZEND_NAMED_FUNCTION(zealphp_dispatch)
{
    zend_string *fname = execute_data->func->common.function_name;
    zend_string *lc = zend_string_tolower(fname);

    zval *cb_view = zend_hash_find(&zealphp_callbacks, lc);
    zend_string_release(lc);

    if (!cb_view || Z_TYPE_P(cb_view) == IS_UNDEF) {
        RETURN_NULL();
    }

    /* PIN the callback. Under OpenSwoole's coroutine model the dispatch
     * thunk can be suspended mid-call (a yield inside the user callback).
     * If RSHUTDOWN fires during that suspension and destroys
     * zealphp_callbacks, the closure object is freed and the cb_view
     * pointer dangles. ZVAL_COPY bumps the closure's refcount so OUR
     * reference survives the table destroy; zval_ptr_dtor below drops it.
     * Closes HIGH H2 from the v0.3.9 security review. */
    zval cb;
    ZVAL_COPY(&cb, cb_view);

    uint32_t argc = ZEND_CALL_NUM_ARGS(execute_data);
    zval *args = NULL;
    if (argc > 0) {
        args = ZEND_CALL_ARG(execute_data, 1);
    }

    zval retval;
    ZVAL_UNDEF(&retval);

    if (call_user_function(NULL, NULL, &cb, &retval, argc, args) == SUCCESS) {
        if (Z_TYPE(retval) != IS_UNDEF) {
            ZVAL_COPY_VALUE(return_value, &retval);
        } else {
            zval_ptr_dtor(&retval);
        }
    } else {
        zval_ptr_dtor(&retval);
        if (!EG(exception)) {
            php_error_docref(NULL, E_WARNING,
                "ext-zealphp: callback for %s failed", ZSTR_VAL(fname));
        }
    }
    zval_ptr_dtor(&cb);
}

/* ── zealphp_override(string $name, callable $cb): bool ──────────── */

PHP_FUNCTION(zealphp_override)
{
    zend_string *func_name;
    zend_fcall_info fci;
    zend_fcall_info_cache fcc;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(func_name)
        Z_PARAM_FUNC(fci, fcc)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *lc = zend_string_tolower(func_name);

    /* allowlist check */
    if (!zealphp_is_allowed(ZSTR_VAL(lc), ZSTR_LEN(lc))) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: overriding '%s' is not allowed", ZSTR_VAL(func_name));
        zend_string_release(lc);
        RETURN_FALSE;
    }

    /* already overridden? */
    if (zend_hash_exists(&zealphp_orig_handlers, lc)) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: '%s' is already overridden — restore first",
            ZSTR_VAL(func_name));
        zend_string_release(lc);
        RETURN_FALSE;
    }

    /* find original */
    zend_function *func = zend_hash_find_ptr(CG(function_table), lc);
    if (!func) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: function '%s' not found", ZSTR_VAL(func_name));
        zend_string_release(lc);
        RETURN_FALSE;
    }

    if (func->type != ZEND_INTERNAL_FUNCTION) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: can only override internal (C) functions, not user functions");
        zend_string_release(lc);
        RETURN_FALSE;
    }

    /* save original handler */
    zend_hash_add_new_ptr(&zealphp_orig_handlers, lc,
                          (void *)func->internal_function.handler);

    /* save callback */
    zval cb_copy;
    ZVAL_COPY(&cb_copy, &fci.function_name);
    zend_hash_update(&zealphp_callbacks, lc, &cb_copy);

    /* swap handler */
    func->internal_function.handler = zealphp_dispatch;

    zend_string_release(lc);
    RETURN_TRUE;
}

/* ── zealphp_restore(string $name): bool ─────────────────────────── */

PHP_FUNCTION(zealphp_restore)
{
    zend_string *func_name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(func_name)
    ZEND_PARSE_PARAMETERS_END();

    zend_string *lc = zend_string_tolower(func_name);

    void *orig = zend_hash_find_ptr(&zealphp_orig_handlers, lc);
    if (!orig) {
        zend_string_release(lc);
        RETURN_FALSE;
    }

    /* COMPARE-AND-RESTORE: only put the original handler back if the
     * current handler is still our dispatch thunk. If another extension
     * (uopz, datadog APM, etc.) hooked the same slot after us, leave it
     * alone — clobbering their hook silently is exactly the integrity
     * failure flagged in the v0.3.8 security review. We still drop our
     * bookkeeping so a future zealphp_override of the same name works. */
    zend_function *func = zend_hash_find_ptr(CG(function_table), lc);
    if (func && func->internal_function.handler == zealphp_dispatch) {
        func->internal_function.handler = (zif_handler)orig;
    } else if (func) {
        /* Notice is generic on purpose — function name might be attacker-
         * controlled (e.g., user PHP doing zealphp_restore($_GET['x']))
         * and we don't want it landing in production logs. The bookkeeping
         * is still cleared so a future override of the same name works. */
        php_error_docref(NULL, E_NOTICE,
            "ext-zealphp: refusing to restore — handler was swapped by "
            "another extension after override (bookkeeping cleared)");
    }

    zend_hash_del(&zealphp_orig_handlers, lc);
    zend_hash_del(&zealphp_callbacks, lc);

    zend_string_release(lc);
    RETURN_TRUE;
}

/* ── zealphp_restore_all(): void ─────────────────────────────────── */

PHP_FUNCTION(zealphp_restore_all)
{
    zend_string *key;
    void *orig;

    ZEND_PARSE_PARAMETERS_NONE();

    ZEND_HASH_FOREACH_STR_KEY_PTR(&zealphp_orig_handlers, key, orig) {
        zend_function *func = zend_hash_find_ptr(CG(function_table), key);
        /* Same compare-and-restore guard as zealphp_restore — don't
         * clobber downstream extensions' hooks on bulk restore. */
        if (func && func->internal_function.handler == zealphp_dispatch) {
            func->internal_function.handler = (zif_handler)orig;
        }
    } ZEND_HASH_FOREACH_END();

    zend_hash_clean(&zealphp_orig_handlers);
    zend_hash_clean(&zealphp_callbacks);
}

/* ── Superglobals management ─────────────────────────────────────── */

/* Map superglobal name to a PG(http_globals) storage slot.
 * Returns -1 for names with no backing slot in the array.
 *
 * IMPORTANT: PG(http_globals) is declared `zval http_globals[6]` (valid
 * indices 0-5: POST/GET/COOKIE/SERVER/ENV/FILES). TRACK_VARS_REQUEST is 6,
 * which is OUT OF BOUNDS for that array — PHP never stores $_REQUEST there;
 * it builds $_REQUEST from GET+POST+COOKIE and keeps it only in the symbol
 * table. So _REQUEST (and _SESSION/_ENV) must return -1: the symbol-table
 * write in zealphp_set_superglobal is what makes them visible. Returning 6
 * here read/wrote past the end of the struct and segfaulted in zval_ptr_dtor. */
static int zealphp_track_vars_index(const char *name)
{
    if (name[0] != '_') return -1;
    if (strcmp(name, "_GET") == 0)     return TRACK_VARS_GET;
    if (strcmp(name, "_POST") == 0)    return TRACK_VARS_POST;
    if (strcmp(name, "_COOKIE") == 0)  return TRACK_VARS_COOKIE;
    if (strcmp(name, "_SERVER") == 0)  return TRACK_VARS_SERVER;
    if (strcmp(name, "_FILES") == 0)   return TRACK_VARS_FILES;
    /* _REQUEST has no http_globals slot (index 6 is OOB) — symbol table only. */
    return -1;
}

/* Helper: overwrite a superglobal in the executor symbol table AND
 * update PG(http_globals) so PHP's auto-global JIT resolves correctly.
 *
 * EG(symbol_table): in-place zval swap via ZVAL_DUP preserves the zval
 * memory ADDRESS (CV cache compatible) with refcount=1 (no COW on write).
 *
 * PG(http_globals): PHP's auto-global JIT callback reads from this
 * process-wide array. In CLI SAPI these slots start as IS_UNDEF —
 * must check type before zval_ptr_dtor to avoid segfault. */
static void zealphp_set_superglobal(const char *name, size_t name_len, zval *value)
{
    zval *existing = zend_hash_str_find(&EG(symbol_table), name, name_len);
    if (existing) {
        zval old;
        ZVAL_COPY_VALUE(&old, existing);
        ZVAL_DUP(existing, value);
        zval_ptr_dtor(&old);
    } else {
        zval copy;
        ZVAL_DUP(&copy, value);
        zend_hash_str_add_new(&EG(symbol_table), name, name_len, &copy);
        existing = zend_hash_str_find(&EG(symbol_table), name, name_len);
    }

    /* Sync PG(http_globals) — the source auto-global JIT reads from.
     * CLI SAPI never initializes these slots (they're IS_UNDEF), so
     * guard the dtor. After this, the JIT callback returns our data. */
    int idx = zealphp_track_vars_index(name);
    if (idx >= 0 && existing) {
        if (Z_TYPE(PG(http_globals)[idx]) != IS_UNDEF) {
            zval_ptr_dtor(&PG(http_globals)[idx]);
        }
        ZVAL_COPY(&PG(http_globals)[idx], existing);
    }
}

/* zealphp_superglobals_set(array $g, $p, $c, $s, $f, $r, $sess): void
 * Overwrites $_GET, $_POST, $_COOKIE, $_SERVER, $_FILES, $_REQUEST, $_SESSION
 * with the supplied arrays. Called by ZealPHP at request start. */
PHP_FUNCTION(zealphp_superglobals_set)
{
    zval *get, *post, *cookie, *server, *files, *request, *session;

    ZEND_PARSE_PARAMETERS_START(7, 7)
        Z_PARAM_ARRAY(get)
        Z_PARAM_ARRAY(post)
        Z_PARAM_ARRAY(cookie)
        Z_PARAM_ARRAY(server)
        Z_PARAM_ARRAY(files)
        Z_PARAM_ARRAY(request)
        Z_PARAM_ARRAY(session)
    ZEND_PARSE_PARAMETERS_END();

    zealphp_set_superglobal("_GET",     sizeof("_GET")-1,     get);
    zealphp_set_superglobal("_POST",    sizeof("_POST")-1,    post);
    zealphp_set_superglobal("_COOKIE",  sizeof("_COOKIE")-1,  cookie);
    zealphp_set_superglobal("_SERVER",  sizeof("_SERVER")-1,  server);
    zealphp_set_superglobal("_FILES",   sizeof("_FILES")-1,   files);
    zealphp_set_superglobal("_REQUEST", sizeof("_REQUEST")-1, request);
    zealphp_set_superglobal("_SESSION", sizeof("_SESSION")-1, session);
}

/* zealphp_superglobals_clear(): void
 * Resets all superglobals to empty arrays. Called at request end
 * to prevent cross-request leakage in coroutine mode. */
PHP_FUNCTION(zealphp_superglobals_clear)
{
    ZEND_PARSE_PARAMETERS_NONE();

    for (const char **n = sg_names; *n; n++) {
        zval empty;
        array_init(&empty);
        zealphp_set_superglobal(*n, strlen(*n), &empty);
        zval_ptr_dtor(&empty);
    }
}

/* zealphp_superglobals_save(): array
 * Snapshots all 6 superglobals into one array for coroutine storage. */
PHP_FUNCTION(zealphp_superglobals_save)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);
    for (const char **n = sg_names; *n; n++) {
        zval *sg = zend_hash_str_find(&EG(symbol_table), *n, strlen(*n));
        if (sg) {
            zval copy;
            ZVAL_COPY(&copy, sg);
            add_assoc_zval(return_value, *n, &copy);
        }
    }
}

/* zealphp_superglobals_restore(array $snapshot): void
 * Restores superglobals from a snapshot saved by zealphp_superglobals_save(). */
PHP_FUNCTION(zealphp_superglobals_restore)
{
    zval *snapshot;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(snapshot)
    ZEND_PARSE_PARAMETERS_END();

    for (const char **n = sg_names; *n; n++) {
        zval *val = zend_hash_str_find(Z_ARRVAL_P(snapshot), *n, strlen(*n));
        if (val) {
            zealphp_set_superglobal(*n, strlen(*n), val);
        }
    }
}

/* ── zealphp_coroutine_superglobals(bool $enable): bool ──────────── */
/* Activates per-coroutine superglobal save/restore via OpenSwoole's
 * yield/resume/close hooks. Returns true if hooks were registered,
 * false if OpenSwoole scheduler hooks aren't available. */
PHP_FUNCTION(zealphp_coroutine_superglobals)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    if (!enable) {
        zealphp_coro_hooks_active = false;
        zend_hash_clean(&zealphp_coro_snapshots);
        RETURN_TRUE;
    }

    if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
            "Per-coroutine superglobal isolation requires OpenSwoole.");
        RETURN_FALSE;
    }

    if (!zealphp_coro_hooks_active) {
        if (!zealphp_install_coro_hooks()) {
            RETURN_FALSE;
        }
        zealphp_coro_hooks_active = true;
    }

    RETURN_TRUE;
}

/* ── zealphp_coroutine_globals(bool $enable): bool ─────────────────── */
/* Activates per-coroutine full $GLOBALS (EG(symbol_table)) save/restore.
 * Sits on top of the same OpenSwoole yield/resume/close hooks installed
 * by zealphp_coroutine_superglobals — both flags can be toggled
 * independently. Returns true on success, false if the underlying
 * scheduler hooks are unavailable. */
PHP_FUNCTION(zealphp_coroutine_globals)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    if (!enable) {
        zealphp_coro_globals_hooks_active = false;
        zend_hash_clean(&zealphp_coro_globals_deltas);
        zend_hash_clean(&zealphp_coro_globals_tombstones);
        /* Drop the shared parent so the next enable re-snapshots fresh. */
        zealphp_globals_parent_clear();
        RETURN_TRUE;
    }

    if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
            "Per-coroutine $GLOBALS isolation requires OpenSwoole.");
        RETURN_FALSE;
    }

    /* First activation — snapshot the parent baseline. Idempotent guard
     * inside zealphp_globals_parent_snapshot() handles the case where the
     * extension is enabled twice in succession without a disable. */
    zealphp_globals_parent_snapshot();

    /* Ensure the scheduler callbacks chain through our on_yield/on_resume/
     * on_close wrappers. Idempotent — if another isolation feature already
     * installed them this is a no-op; refuses (false) if the originals can't
     * be chained (C2). */
    if (!zealphp_install_coro_hooks()) {
        RETURN_FALSE;
    }

    zealphp_coro_globals_hooks_active = true;
    RETURN_TRUE;
}

/* Stage 5 — per-coroutine function-local static isolation (opt-in).
 * Sets the flag the on_yield/on_resume hooks check. The scheduler callbacks
 * are normally already chained (coroutine-legacy enables superglobals/globals
 * isolation first); if called standalone, install them idempotently. */
PHP_FUNCTION(zealphp_coroutine_statics)
{
    bool enable;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    if (!enable) {
        zealphp_fn_statics_active = false;
        zend_hash_clean(&zealphp_coro_fn_static_snapshots);
        /* Clear the touched-set so a later re-enable re-seeds fresh. The
         * BIND_STATIC handler stays installed but is gated by the flag. */
        zend_hash_clean(&zealphp_fn_static_registry);
        RETURN_TRUE;
    }

    if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
            "Per-coroutine function-static isolation requires OpenSwoole.");
        RETURN_FALSE;
    }

    /* Install the ZEND_BIND_STATIC opcode hook ONCE (chain-aware vs uopz),
     * then seed the registry with everything already instantiated. Installing
     * at activation (runtime, after all extension MINITs) means we capture
     * whatever handler is already there and never get clobbered by load order;
     * BIND_STATIC stays completely untouched for apps that don't use Stage 5. */
    if (!zealphp_bind_static_installed) {
        zealphp_prev_bind_static = zend_get_user_opcode_handler(ZEND_BIND_STATIC);
        zend_set_user_opcode_handler(ZEND_BIND_STATIC, zealphp_bind_static_handler);
        zealphp_bind_static_installed = true;
    }

    if (!zealphp_install_coro_hooks()) {
        RETURN_FALSE;
    }

    /* Flag ON, then seed the registry with everything already instantiated. */
    zealphp_fn_statics_active = true;
    zealphp_walk_fn_statics(zealphp_fn_static_seed_cb, NULL);
    RETURN_TRUE;
}

/* ── Process-state snapshot/clean (included_files + classes + functions) ── */

/* Full process-state snapshot for pool workers. Captures:
 * - EG(included_files) keys — require_once cache
 * - CG(class_table) user-class keys
 * - CG(function_table) user-function keys
 * Cleaning removes entries added after the snapshot, giving the pool worker
 * fresh-process semantics without the proc_open cost. */
static HashTable zealphp_snapshot_files;
static HashTable zealphp_snapshot_classes;
static HashTable zealphp_snapshot_functions;
static bool zealphp_state_snapshotted = false;

/* Add class names to the snapshot so they survive process_state_clean.
 * Used to preserve autoloader-referenced classes (spl_autoload_functions)
 * that get registered lazily during request handling.
 */
PHP_FUNCTION(zealphp_protect_classes)
{
    zval *names;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(names)
    ZEND_PARSE_PARAMETERS_END();

    if (!zealphp_state_snapshotted) return;

    zval one;
    ZVAL_LONG(&one, 1);

    zval *val;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(names), val) {
        if (Z_TYPE_P(val) == IS_STRING) {
            zend_string *key = zend_string_tolower(Z_STR_P(val));
            zend_hash_update(&zealphp_snapshot_classes, key, &one);
            zend_string_release(key);
        }
    } ZEND_HASH_FOREACH_END();
}

PHP_FUNCTION(zealphp_process_state_snapshot)
{
    zend_string *key;

    if (zealphp_state_snapshotted) {
        zend_hash_clean(&zealphp_snapshot_files);
        zend_hash_clean(&zealphp_snapshot_classes);
        zend_hash_clean(&zealphp_snapshot_functions);
    }

    zval one;
    ZVAL_LONG(&one, 1);

    /* Included files */
    ZEND_HASH_FOREACH_STR_KEY(&EG(included_files), key) {
        if (key) zend_hash_update(&zealphp_snapshot_files, key, &one);
    } ZEND_HASH_FOREACH_END();

    /* User classes only */
    zval *cls_zv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(class_table), key, cls_zv) {
        if (key) {
            zend_class_entry *ce = Z_PTR_P(cls_zv);
            if (ce && ce->type == ZEND_USER_CLASS) {
                zend_hash_update(&zealphp_snapshot_classes, key, &one);
            }
        }
    } ZEND_HASH_FOREACH_END();

    /* User functions only */
    zval *fn_zv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(function_table), key, fn_zv) {
        if (key) {
            zend_function *func = Z_PTR_P(fn_zv);
            if (func && func->type == ZEND_USER_FUNCTION) {
                zend_hash_update(&zealphp_snapshot_functions, key, &one);
            }
        }
    } ZEND_HASH_FOREACH_END();

    zealphp_state_snapshotted = true;
}

PHP_FUNCTION(zealphp_process_state_clean)
{
    zend_string *key;
    zval *val;
    zend_long flags = 7; /* default: all (1=files, 2=classes, 4=functions) */

    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL Z_PARAM_LONG(flags)
    ZEND_PARSE_PARAMETERS_END();

    if (!zealphp_state_snapshotted) return;

    /* --- Included files: remove entries not in snapshot --- */
    if (!(flags & 1)) goto skip_files;
    zend_string **del_files = NULL;
    uint32_t df_count = 0, df_cap = 64;
    del_files = emalloc(sizeof(zend_string *) * df_cap);

    ZEND_HASH_FOREACH_STR_KEY(&EG(included_files), key) {
        if (key && !zend_hash_exists(&zealphp_snapshot_files, key)) {
            if (df_count >= df_cap) { df_cap *= 2; del_files = erealloc(del_files, sizeof(zend_string *) * df_cap); }
            del_files[df_count++] = key;
        }
    } ZEND_HASH_FOREACH_END();
    for (uint32_t i = 0; i < df_count; i++) zend_hash_del(&EG(included_files), del_files[i]);
    efree(del_files);
skip_files:

    /* --- User classes: remove entries not in snapshot --- */
    if (!(flags & 2)) goto skip_classes;
    /* SAFETY: skip classes that have initialized static members (runtime
     * copy of default_static_members_table). Removing such classes via
     * zend_hash_del leaves a zombie entry (class_exists returns true but
     * statics are inaccessible → segfault). See ext-zealphp#1.
     * Classes without statics or with never-accessed statics are safe. */
    zend_string **del_cls = NULL;
    uint32_t dc_count = 0, dc_cap = 64;
    del_cls = emalloc(sizeof(zend_string *) * dc_cap);

    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(class_table), key, val) {
        if (key && !zend_hash_exists(&zealphp_snapshot_classes, key)) {
            zend_class_entry *ce = Z_PTR_P(val);
            if (ce && ce->type == ZEND_USER_CLASS) {
                /* H4: actually implement the safety the comment above promises.
                 * A class whose runtime static-members table is allocated
                 * (CE_STATIC_MEMBERS non-NULL) must NOT be hash_del'd — that
                 * leaves a zombie (class_exists() true, statics inaccessible →
                 * segfault, ext-zealphp#1). Skip it; it leaks one entry until
                 * worker recycle, which is bounded. */
                if (ce->default_static_members_count > 0 &&
                    CE_STATIC_MEMBERS(ce) != NULL) {
                    continue;
                }
                /* H3: purge Stage-5 registry entries for this class's methods
                 * before the class (and its method op_arrays) are freed.
                 * No-op when the registry is empty (Stage 5 inactive). */
                if (zend_hash_num_elements(&zealphp_fn_static_registry) > 0) {
                    zend_function *m;
                    ZEND_HASH_FOREACH_PTR(&ce->function_table, m) {
                        if (m && m->type == ZEND_USER_FUNCTION &&
                            m->op_array.static_variables) {
                            zend_hash_index_del(&zealphp_fn_static_registry,
                                (zend_ulong)(uintptr_t)m->op_array.static_variables);
                        }
                    } ZEND_HASH_FOREACH_END();
                }
                if (dc_count >= dc_cap) { dc_cap *= 2; del_cls = erealloc(del_cls, sizeof(zend_string *) * dc_cap); }
                del_cls[dc_count++] = key;
            }
        }
    } ZEND_HASH_FOREACH_END();
    {
        dtor_func_t saved = CG(class_table)->pDestructor;
        CG(class_table)->pDestructor = NULL;
        for (uint32_t i = 0; i < dc_count; i++) zend_hash_del(CG(class_table), del_cls[i]);
        CG(class_table)->pDestructor = saved;
    }
    efree(del_cls);
skip_classes:

    /* --- User functions: remove entries not in snapshot --- */
    if (!(flags & 4)) goto skip_functions;
    zend_string **del_fn = NULL;
    uint32_t dfn_count = 0, dfn_cap = 64;
    del_fn = emalloc(sizeof(zend_string *) * dfn_cap);

    ZEND_HASH_FOREACH_STR_KEY_VAL(CG(function_table), key, val) {
        if (key && !zend_hash_exists(&zealphp_snapshot_functions, key)) {
            zend_function *func = Z_PTR_P(val);
            if (func && func->type == ZEND_USER_FUNCTION) {
                /* H3: this op_array may be referenced by the Stage 5 fn-static
                 * registry (keyed by op_array->static_variables). Purge it
                 * before the function is freed so a later yield-time walk can't
                 * deref freed memory. No-op when Stage 5 is inactive (registry
                 * empty) — the modes are mutually exclusive, this is defense in
                 * depth. */
                if (func->op_array.static_variables) {
                    zend_hash_index_del(&zealphp_fn_static_registry,
                        (zend_ulong)(uintptr_t)func->op_array.static_variables);
                }
                if (dfn_count >= dfn_cap) { dfn_cap *= 2; del_fn = erealloc(del_fn, sizeof(zend_string *) * dfn_cap); }
                del_fn[dfn_count++] = key;
            }
        }
    } ZEND_HASH_FOREACH_END();
    /* Disable destructor to avoid freeing OPcache SHM op_arrays.
     * Non-immutable entries leak negligibly per request; the worker
     * recycle (ZEALPHP_POOL_MAX_REQUESTS) bounds total growth. */
    {
        dtor_func_t saved = CG(function_table)->pDestructor;
        CG(function_table)->pDestructor = NULL;
        for (uint32_t i = 0; i < dfn_count; i++) zend_hash_del(CG(function_table), del_fn[i]);
        CG(function_table)->pDestructor = saved;
    }
    efree(del_fn);
skip_functions:
    ;
}

/* ── $GLOBALS snapshot/clean ─────────────────────────────────────── */

/* zealphp_globals_snapshot(): void — save current EG(symbol_table) keys */
PHP_FUNCTION(zealphp_globals_snapshot)
{
    zend_string *key;

    if (zealphp_globals_snapshotted) {
        zend_hash_clean(&zealphp_globals_snapshot);
    }

    ZEND_HASH_FOREACH_STR_KEY(&EG(symbol_table), key) {
        if (key) {
            zval one;
            ZVAL_LONG(&one, 1);
            zend_hash_update(&zealphp_globals_snapshot, key, &one);
        }
    } ZEND_HASH_FOREACH_END();

    zealphp_globals_snapshotted = true;
}

/* zealphp_globals_clean(): void — remove keys not in the snapshot */
PHP_FUNCTION(zealphp_globals_clean)
{
    zend_string *key;
    zval *val;

    if (!zealphp_globals_snapshotted) {
        return;
    }

    /* Collect keys to delete (can't delete while iterating) */
    zend_string **to_delete = NULL;
    uint32_t delete_count = 0;
    uint32_t delete_cap = 64;
    to_delete = emalloc(sizeof(zend_string *) * delete_cap);

    ZEND_HASH_FOREACH_STR_KEY_VAL(&EG(symbol_table), key, val) {
        if (key && !zend_hash_exists(&zealphp_globals_snapshot, key)) {
            if (delete_count >= delete_cap) {
                delete_cap *= 2;
                to_delete = erealloc(to_delete, sizeof(zend_string *) * delete_cap);
            }
            to_delete[delete_count++] = key;
        }
    } ZEND_HASH_FOREACH_END();

    for (uint32_t i = 0; i < delete_count; i++) {
        zend_hash_del(&EG(symbol_table), to_delete[i]);
    }

    efree(to_delete);
}

/* ── define() interception ───────────────────────────────────────── */

/* Intercept define() to track per-request constants. The real define()
 * runs first; if it succeeds, we record the name so zealphp_constants_clear()
 * can remove it at request end. */
static ZEND_NAMED_FUNCTION(zealphp_define_intercept)
{
    /* Stage 3.5 — silent-define-redeclare. When silent_redeclare is on
     * and define(NAME, VAL) targets a name that's already defined, we
     * return true WITHOUT calling the real define(). Suppresses PHP's
     * "Constant X already defined" E_WARNING — exactly the legacy-app
     * boot pattern (`define('ROOT_DIR', __DIR__)` at top of every
     * include) that crashes coroutine-mode workers on request 2.
     *
     * First-declaration wins (matches FPM's fresh-proc semantics). */
    if (zealphp_silent_redeclare_enabled) {
        zval *arg1 = ZEND_CALL_ARG(execute_data, 1);
        if (arg1 && Z_TYPE_P(arg1) == IS_STRING
            && zend_hash_exists(EG(zend_constants), Z_STR_P(arg1))) {
            RETURN_TRUE;
        }
    }

    /* Forward to the real define() */
    zealphp_orig_define_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);

    /* If define() succeeded (returned true), track the constant name so
     * zealphp_constants_clear() can remove it at request end. */
    if (Z_TYPE_P(return_value) == IS_TRUE) {
        zval *arg1 = ZEND_CALL_ARG(execute_data, 1);
        if (arg1 && Z_TYPE_P(arg1) == IS_STRING) {
            zval one;
            ZVAL_LONG(&one, 1);
            zend_hash_update(&zealphp_request_constants, Z_STR_P(arg1), &one);
        }
    }
}

/* ── zealphp_constants_clear(): void ─────────────────────────────── */

PHP_FUNCTION(zealphp_constants_clear)
{
    zend_string *name;
    ZEND_HASH_FOREACH_STR_KEY(&zealphp_request_constants, name) {
        if (name) {
            /* zend_hash_del on EG(zend_constants) removes the constant.
             * Case-sensitive constants use the exact name as key. */
            zend_hash_del(EG(zend_constants), name);
        }
    } ZEND_HASH_FOREACH_END();
    zend_hash_clean(&zealphp_request_constants);
}

/* ── zealphp_ini_restore(): void ──────────────────────────────────── */
/* Restore all ini entries modified by ini_set() during this request.
 * Called at request end from SessionManager/CoSessionManager. For
 * sequential requests (no yield), the coroutine hooks don't fire, so
 * this is the cleanup path. */
PHP_FUNCTION(zealphp_ini_restore)
{
    ZEND_PARSE_PARAMETERS_NONE();

    if (!EG(modified_ini_directives) ||
        zend_hash_num_elements(EG(modified_ini_directives)) == 0) {
        return;
    }

    /* Collect names first — zend_restore_ini_entry may modify the hash */
    uint32_t count = 0, cap = 32;
    zend_string **names = emalloc(sizeof(zend_string *) * cap);

    zend_string *name;
    ZEND_HASH_FOREACH_STR_KEY(EG(modified_ini_directives), name) {
        if (name) {
            if (count >= cap) { cap *= 2; names = erealloc(names, sizeof(zend_string *) * cap); }
            names[count++] = name;
        }
    } ZEND_HASH_FOREACH_END();

    for (uint32_t i = 0; i < count; i++) {
        zend_restore_ini_entry(names[i], ZEND_INI_STAGE_RUNTIME);
    }

    efree(names);
}

/* ── zealphp_define_hook(bool $enable): bool ─────────────────────── */

PHP_FUNCTION(zealphp_define_hook)
{
    bool enable;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

    if (enable && !zealphp_define_hooked) {
        zend_string *name = zend_string_init("define", sizeof("define") - 1, 0);
        zend_function *func = zend_hash_find_ptr(CG(function_table), name);
        if (func && func->type == ZEND_INTERNAL_FUNCTION) {
            zealphp_orig_define_handler = func->internal_function.handler;
            func->internal_function.handler = zealphp_define_intercept;
            zealphp_define_hooked = true;
        }
        zend_string_release(name);
    } else if (!enable && zealphp_define_hooked) {
        zend_string *name = zend_string_init("define", sizeof("define") - 1, 0);
        zend_function *func = zend_hash_find_ptr(CG(function_table), name);
        if (func && zealphp_orig_define_handler) {
            func->internal_function.handler = zealphp_orig_define_handler;
            zealphp_orig_define_handler = NULL;
            zealphp_define_hooked = false;
        }
        zend_string_release(name);
    }

    RETURN_BOOL(zealphp_define_hooked);
}

/* ── Module lifecycle ────────────────────────────────────────────── */

/* ── Stage 3: silent-redeclare opcode hooks ───────────────────────────
 *
 * Top-level `function foo() {}` / `class Bar {}` in legacy PHP fires
 * E_COMPILE_ERROR ("Cannot redeclare ...") on the SECOND request in a
 * long-running worker — the symbol from request 1 is still in
 * EG(function_table) / CG(class_table). FPM doesn't hit this because each
 * request gets a fresh PHP process; ZealPHP's Mode 1 Pool sidesteps it via
 * subprocess scope. But Mode 3/4/5 share one process.
 *
 * The opcodes that bind a compiled declaration into the runtime symbol
 * table are:
 *   - ZEND_DECLARE_FUNCTION        (top-level fn declared in a compiled file)
 *   - ZEND_DECLARE_CLASS           (top-level class without parent)
 *   - ZEND_DECLARE_CLASS_DELAYED   (class WITH parent — bind deferred until
 *                                   parent class is autoloaded)
 *
 * Hook strategy:
 *   - Check if the target symbol already exists in the global table.
 *   - If yes → SKIP the opcode (ZEND_USER_OPCODE_CONTINUE advances IP past
 *     the do_bind_*() call that would E_COMPILE_ERROR).
 *   - If no  → dispatch to the original handler.
 *
 * Semantics: "first declaration wins" — matches what FPM produces by
 * recycling the process every request (whatever declared on request N
 * stays for request N+1+...). User code that intentionally redefines a
 * symbol must use uopz / closures / autoloaders instead.
 */

/* zealphp_silent_redeclare_enabled is hoisted to the top of this file
 * so the zealphp_define_intercept hook can read it. */

/* Extract the lowercased target name from a DECLARE_FUNCTION / DECLARE_CLASS
 * opline. Both opcodes use op1 as the "destination key" string constant. */
static zend_string *zealphp_decl_target_lcname(const zend_op *opline, zend_execute_data *execute_data)
{
    zval *src = NULL;
    if (opline->op1_type == IS_CONST) {
        src = RT_CONSTANT(opline, opline->op1);
    }
    if (!src || Z_TYPE_P(src) != IS_STRING) {
        return NULL;
    }
    /* DECLARE_FUNCTION / DECLARE_CLASS op1 is already-lowercased per the
     * compiler — see zend_compile_func_decl / zend_compile_class_decl in
     * Zend/zend_compile.c. We can use the string as-is. */
    return Z_STR_P(src);
}

static int zealphp_declare_function_handler(zend_execute_data *execute_data)
{
    if (!zealphp_silent_redeclare_enabled) {
        return zealphp_chain_or_dispatch(zealphp_prev_declare_function, execute_data);
    }
    const zend_op *opline = EX(opline);
    zend_string *lcname = zealphp_decl_target_lcname(opline, execute_data);
    if (lcname && zend_hash_exists(EG(function_table), lcname)) {
        /* Already declared — WE own this: skip do_bind_function (terminal). */
        EX(opline)++;
        return ZEND_USER_OPCODE_CONTINUE;
    }
    return zealphp_chain_or_dispatch(zealphp_prev_declare_function, execute_data);
}

static int zealphp_declare_class_handler(zend_execute_data *execute_data)
{
    if (!zealphp_silent_redeclare_enabled) {
        return zealphp_chain_or_dispatch(zealphp_prev_declare_class, execute_data);
    }
    const zend_op *opline = EX(opline);
    zend_string *lcname = zealphp_decl_target_lcname(opline, execute_data);
    if (lcname && zend_hash_exists(CG(class_table), lcname)) {
        EX(opline)++;
        return ZEND_USER_OPCODE_CONTINUE;
    }
    return zealphp_chain_or_dispatch(zealphp_prev_declare_class, execute_data);
}

static int zealphp_declare_class_delayed_handler(zend_execute_data *execute_data)
{
    if (!zealphp_silent_redeclare_enabled) {
        return zealphp_chain_or_dispatch(zealphp_prev_declare_class_delayed, execute_data);
    }
    /* DELAYED variant uses op2 for the destination (the parent-required key
     * lives in op1). Conservative behaviour: dispatch normally if we can't
     * confirm a duplicate. */
    const zend_op *opline = EX(opline);
    zval *src = NULL;
    if (opline->op2_type == IS_CONST) {
        src = RT_CONSTANT(opline, opline->op2);
    }
    if (src && Z_TYPE_P(src) == IS_STRING
        && zend_hash_exists(CG(class_table), Z_STR_P(src))) {
        EX(opline)++;
        return ZEND_USER_OPCODE_CONTINUE;
    }
    return zealphp_chain_or_dispatch(zealphp_prev_declare_class_delayed, execute_data);
}

/* Stage 5 touched-set: ZEND_BIND_STATIC (opcode 183) fires when a function
 * binds a `static $x`. We record the executing op_array in the registry so the
 * per-yield snapshot iterates only static-using functions. Idempotent (skip if
 * already present), so the per-call cost is one hash lookup.
 *
 * Excludes closures (ZEND_ACC_CLOSURE) and eval/top-level code
 * (function_name == NULL): their op_arrays have per-instance/eval heap lifetime
 * and are NOT in the walked tables, so a stored pointer would dangle (UAF at a
 * later yield). This is the exact population the full-table walk covers — pure
 * semantic parity, not a regression (closure statics were never isolated).
 *
 * We deliberately do NOT hook opcode 203 (ZEND_BIND_INIT_STATIC_OR_JMP): the
 * very first call to any static-using function always reaches 183 (203 only
 * JMPs past 183 once the live table exists), so 183 alone catches every
 * function exactly once. The activation-time seed walk covers anything bound
 * before this hook went live.
 *
 * Chain-aware: if another extension (e.g. uopz's uopz_set_static) already
 * installed a BIND_STATIC handler, we invoke it rather than clobber it. */
static int zealphp_bind_static_handler(zend_execute_data *execute_data)
{
    if (zealphp_fn_statics_active) {
        zend_function *fn = EX(func);
        if (fn && fn->common.function_name != NULL
            && !(fn->common.fn_flags & ZEND_ACC_CLOSURE)) {
            zend_op_array *opa = &fn->op_array;
            if (opa->static_variables) {
                zend_ulong key = (zend_ulong)(uintptr_t)opa->static_variables;
                if (!zend_hash_index_exists(&zealphp_fn_static_registry, key)) {
                    zend_hash_index_add_ptr(&zealphp_fn_static_registry, key, opa);
                }
            }
        }
    }
    if (zealphp_prev_bind_static) {
        return zealphp_prev_bind_static(execute_data);
    }
    return ZEND_USER_OPCODE_DISPATCH;
}

/* ── Stage 7: smart require_once via ZEND_INCLUDE_OR_EVAL opcode hook ──
 *
 * The fundamental problem: PHP's require_once cache (EG(included_files))
 * is process-wide. In persistent servers, files included via require_once
 * on request 1 become no-ops on request 2+. This breaks apps that put
 * per-request routing/rendering logic in require_once'd files (WordPress's
 * template-loader.php, Joomla's dispatcher, etc.).
 *
 * Solution: hook the ZEND_INCLUDE_OR_EVAL opcode. For require_once/
 * include_once, check if the resolved file path is in the boot snapshot
 * (zealphp_snapshot_files). If YES → standard require_once (cached).
 * If NO → remove from EG(included_files) so the standard handler
 * re-includes it. Bootstrap stays fast, per-request code re-executes.
 *
 * Combined with Stage 3 (silent function/class redeclare) and Stage 3.5
 * (silent define redeclare), the re-included file's declarations are
 * silently skipped while its per-request logic runs fresh. */

static int zealphp_include_eval_handler(zend_execute_data *execute_data)
{
    if (!zealphp_include_isolation_enabled || !zealphp_state_snapshotted) {
        return zealphp_chain_or_dispatch(zealphp_prev_include_eval, execute_data);
    }

    const zend_op *opline = EX(opline);

    /* Only intercept require_once / include_once */
    if (opline->extended_value != ZEND_INCLUDE_ONCE
        && opline->extended_value != ZEND_REQUIRE_ONCE) {
        return zealphp_chain_or_dispatch(zealphp_prev_include_eval, execute_data);
    }

    /* Get the filename operand */
    zval *inc_filename = NULL;
    if (opline->op1_type == IS_CONST) {
        inc_filename = RT_CONSTANT(opline, opline->op1);
    } else if (opline->op1_type == IS_CV || opline->op1_type == IS_TMP_VAR
               || opline->op1_type == IS_VAR) {
        inc_filename = EX_VAR(opline->op1.var);
    }
    if (!inc_filename || Z_TYPE_P(inc_filename) != IS_STRING) {
        return zealphp_chain_or_dispatch(zealphp_prev_include_eval, execute_data);
    }

    /* Resolve the full path (same resolution PHP uses internally) */
    zend_string *resolved = zend_resolve_path(Z_STR_P(inc_filename));
    if (!resolved) {
        /* Can't resolve → let the standard handler deal with the error */
        return zealphp_chain_or_dispatch(zealphp_prev_include_eval, execute_data);
    }

    /* Bootstrap file (in snapshot) → normal require_once (cached) */
    if (zend_hash_exists(&zealphp_snapshot_files, resolved)) {
        zend_string_release(resolved);
        return zealphp_chain_or_dispatch(zealphp_prev_include_eval, execute_data);
    }

    /* Per-request file (not in snapshot) → re-include it — but ONLY ONCE per
     * request. Re-deleting on a re-entrant / circular require_once (the file
     * is being included right now, or was already re-included this request)
     * would re-execute it mid-inclusion → unbounded recursion → OOM. Track the
     * set of files already force-re-included this request, keyed by coroutine
     * id; if we've already re-included this file this request, leave it cached
     * (standard require_once no-op) to preserve within-request idempotency. */
    if (os_get_cid) {
        zend_long cid = os_get_cid();
        zval *seen = zend_hash_index_find(&zealphp_coro_reincluded, (zend_ulong)cid);
        if (!seen) {
            zval z;
            array_init(&z);
            seen = zend_hash_index_update(&zealphp_coro_reincluded, (zend_ulong)cid, &z);
        }
        HashTable *seen_ht = Z_ARRVAL_P(seen);
        if (zend_hash_exists(seen_ht, resolved)) {
            /* already re-included this request — keep it cached (no-op) */
            zend_string_release(resolved);
            return zealphp_chain_or_dispatch(zealphp_prev_include_eval, execute_data);
        }
        zval one;
        ZVAL_LONG(&one, 1);
        zend_hash_add(seen_ht, resolved, &one);
    }

    zend_hash_del(&EG(included_files), resolved);
    zend_string_release(resolved);

    /* We mutated EG(included_files) so the engine re-includes this file. Chain
     * to any prior handler (a profiler may want to observe the include), then
     * fall through to DISPATCH so the include actually happens. */
    return zealphp_chain_or_dispatch(zealphp_prev_include_eval, execute_data);
}

/* ── Stage 4: compile-time silent-redeclare via CG-table swap ──────────
 *
 * Top-level `function foo() {}` / `class Bar {}` at file scope are bound
 * to CG(function_table) / CG(class_table) at COMPILE time by
 * zend_register_top_func / zend_register_top_class — they never emit a
 * runtime ZEND_DECLARE_* opcode for Stage 3 to intercept.
 *
 * The earlier attempt (snapshot-detach-compile-restore the WHOLE user
 * symbol space per compile) deadlocked production: every nested
 * compile_file walks + mutates O(N) entries; with autoloader chains
 * doing M nested compiles per request the cumulative cost was O(N*M)
 * and worker recycle timeouts fired before any request completed.
 *
 * New design: SWAP CG(function_table) / CG(class_table) pointers to
 * empty scratch tables for the duration of compile. The dup check in
 * zend_register_top_func reads CG(function_table) — pointed at an
 * empty hash, it sees no dup and succeeds. Reads during compile use
 * EG(function_table) (PHP convention) so internal-function resolution
 * and earlier user definitions stay visible. After compile, merge
 * scratch into the real table with first-wins semantic.
 *
 * Cost per compile: O(K) where K = symbols this file declares.
 * Independent of how many symbols exist process-wide. Re-entrant safe:
 * the swap is stack-local; nested compiles get their own scratch
 * tables, restore in reverse order.
 *
 * First-wins matches FPM's "fresh process per request" semantic — the
 * symbol the user originally got back is the symbol the user keeps
 * getting back.
 */
static zend_op_array *(*zealphp_original_compile_file)(zend_file_handle *file_handle, int type) = NULL;

/* Stage 6 — Track-and-Replay per-file declaration map.
 *
 * Key: realpath string of the source file. Value: HashTable<name, IS_LONG 1>
 * recording every top-level function and class this file declared on its
 * FIRST successful compile.
 *
 * Used on the warm path (request 2+): before calling the engine's
 * original compile_file (which goes through opcache and would hit
 * "Cannot redeclare" on the persistent dup-check), we pre-remove the
 * tracked symbols from EG. opcache loads cleanly into the empty slots.
 * After load, we destroy the new (compile-added) duplicates and reinstall
 * our originals — first-wins, identical semantics to Stage 4 but applied
 * surgically to KNOWN-conflicting symbols instead of swap-the-whole-table.
 *
 * Process-wide table (persistent=1) — same lifecycle as zealphp_orig_handlers. */
static HashTable zealphp_file_decls;
static bool zealphp_file_decls_initialized = false;

/* Helper: extract a stable file-path key from a zend_file_handle.
 * ALWAYS prefers filename (set at hook entry, before the engine
 * resolves opened_path) for consistency between cache-save (cold
 * compile) and cache-lookup (warm compile). */
static zend_string *zealphp_file_handle_path(zend_file_handle *file_handle)
{
    if (file_handle->filename) {
        return zend_string_copy(file_handle->filename);
    }
    if (file_handle->opened_path) {
        return zend_string_copy(file_handle->opened_path);
    }
    return NULL;
}

static zend_op_array *zealphp_compile_file_hook(zend_file_handle *file_handle, int type)
{
    if (!zealphp_silent_redeclare_enabled || !zealphp_original_compile_file) {
        return zealphp_original_compile_file
            ? zealphp_original_compile_file(file_handle, type)
            : NULL;
    }

    /* Lazy-init the per-file decl tracker. */
    if (!zealphp_file_decls_initialized) {
        zend_hash_init(&zealphp_file_decls, 64, NULL, ZVAL_PTR_DTOR, 1);
        zealphp_file_decls_initialized = true;
    }

    /* Stage 6.2 — cold op_array cache. On cold compile, save the
     * op_array. On warm compile of same file, return the saved
     * op_array directly, bypassing opcache's hot-path bind that
     * would dup-error.
     *
     * Per-worker storage (no SMA needed): each worker pays the cold
     * cost ONCE per redeclare-prone file (~50 files for WordPress),
     * then serves from process-local cache for the rest of its
     * lifetime. Memory bounded: ~10 KB × 50 files = ~500 KB per worker. */
    zend_string *file_key = zealphp_file_handle_path(file_handle);
    /* Stage 6 compile-cache REMOVED. It cached the compiled op_array and, on a
     * later compile of the same file, returned a memcpy'd shell with
     * (*shell->refcount)++. But the engine destroys the compiled op_array after
     * executing it, so the cached `refcount` pointer DANGLES — the ++ is a
     * use-after-free → worker SIGSEGV under load (phpmyadmin on the 50-app
     * sweep; gdb: zealphp_compile_file_hook at "(*shell->refcount)++"). An
     * op_array cannot be safely shared by shallow memcpy + refcount across
     * requests. Stage 4's CG-table swap below already handles top-level
     * redeclaration correctly on EVERY compile, so the cache was only a
     * re-compile optimization — not worth a UAF. */
    /* Save real table pointers — restore on exit (nested-safe via stack). */
    HashTable *real_cg_fn = CG(function_table);
    HashTable *real_cg_cl = CG(class_table);
    HashTable *real_eg_fn = EG(function_table);
    HashTable *real_eg_cl = EG(class_table);

    /* Scratch tables with NULL dtor — we manage entry lifecycle below. */
    HashTable scratch_fn, scratch_cl;
    zend_hash_init(&scratch_fn, 8, NULL, NULL, 0);
    zend_hash_init(&scratch_cl, 8, NULL, NULL, 0);

    /* Stage 4: swap CG only. EG stays pointing at the real table so
     * internal function/class lookups during compile (Closure for type
     * hints, attribute classes, parent classes for inheritance fixup)
     * still resolve.
     *
     * Stage 4-v2 attempt — swapping EG(function_table) only, keeping
     * EG(class_table) real — also broke compiles. Even the WordPress
     * homepage that previously worked started returning 500 on the
     * second request. The function_table swap interacts with the
     * compiler's internal machinery in ways that aren't surface-
     * documented. Reverted.
     *
     * The opcache hot path (zend_accel_load_script) remains the gap.
     * Closing it cleanly needs an engine-level hook on do_bind_function
     * or a configuration the engine doesn't expose. M1 Pool for the
     * specific endpoints stays the documented FPM pair-up. */
    CG(function_table) = &scratch_fn;
    CG(class_table)    = &scratch_cl;

    /* Bailout-safe compile. E_COMPILE_ERROR / OOM / E_ERROR fire
     * zend_bailout() which longjmps to the engine's bailout setjmp WITHOUT
     * unwinding back through this function — if we leave CG pointing at
     * our stack-local scratch when the stack frame goes away, the next
     * access to CG(function_table) is a use-after-free.
     *
     * zend_try/zend_catch sets up a local bailout context. On bailout we
     * restore CG pointers, free the scratch tables, then re-raise the
     * bailout so the engine's outer handler still terminates the request.
     * Closes MEDIUM M1 from the v0.3.9 security review. */
    zend_op_array *result = NULL;
    bool bailed_out = false;
    zend_try {
        result = zealphp_original_compile_file(file_handle, type);
    } zend_catch {
        bailed_out = true;
    } zend_end_try();

    /* Restore BEFORE anything else — must hold even on the bailout path. */
    CG(function_table) = real_cg_fn;
    CG(class_table)    = real_cg_cl;
    /* EG was never swapped (Stage 4 CG-only design) — keep these
     * locals referenced so the compiler doesn't warn. */
    (void) real_eg_fn;
    (void) real_eg_cl;

    if (bailed_out) {
        /* Engine is in fatal-error state. Just free what we own and
         * re-raise. The buckets in scratch may hold partially-constructed
         * declarations from before the error; the engine's per-arena
         * teardown handles them — we just drop our wrapper. */
        zend_hash_destroy(&scratch_fn);
        zend_hash_destroy(&scratch_cl);
        zend_bailout();
    }

    /* Merge first-wins: only insert keys not already present in real. */
    zend_string *key;
    void *ptr;

    ZEND_HASH_FOREACH_STR_KEY_PTR(&scratch_fn, key, ptr) {
        if (key && !zend_hash_exists(real_cg_fn, key)) {
            zend_hash_add_ptr(real_cg_fn, key, ptr);
        } else if (ptr) {
            /* Loser: first declaration already in real. Free the dup so
             * we don't leak its op_array body. ZEND_FUNCTION_DTOR equivalent. */
            destroy_zend_function((zend_function*)ptr);
        }
    } ZEND_HASH_FOREACH_END();

    ZEND_HASH_FOREACH_STR_KEY_PTR(&scratch_cl, key, ptr) {
        if (key && !zend_hash_exists(real_cg_cl, key)) {
            zend_hash_add_ptr(real_cg_cl, key, ptr);
        } else if (ptr) {
            /* destroy_zend_class wants a zval — build one pointing at
             * the class entry. ZEND_CLASS_DTOR is the same impl. */
            zval cl_zv;
            ZVAL_PTR(&cl_zv, ptr);
            destroy_zend_class(&cl_zv);
        }
    } ZEND_HASH_FOREACH_END();

    /* Stage 6.2 cache-save REMOVED — see the cache-hit note above: a cached
     * op_array's refcount pointer dangles once the engine frees it, segfaulting
     * the next compile. Stage 4 re-compiles + first-wins-merges every time,
     * which is correct without the cache. */
    if (file_key) {
        zend_string_release(file_key);
    }

    /* Scratch tables had NULL dtor — destroy_zend_hash just frees the
     * buckets, not the values (the values are now either in real or
     * already destroyed via the loser branch above). */
    zend_hash_destroy(&scratch_fn);
    zend_hash_destroy(&scratch_cl);
    return result;
}

/* Public API: zealphp_silent_redeclare(bool $on = true): bool
 * Returns previous state. */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_silent_redeclare, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, on, _IS_BOOL, 1)
ZEND_END_ARG_INFO()

PHP_FUNCTION(zealphp_silent_redeclare)
{
    zend_bool on = 1;
    zend_bool on_is_null = 1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(on, on_is_null)
    ZEND_PARSE_PARAMETERS_END();

    bool prev = zealphp_silent_redeclare_enabled;
    if (!on_is_null) {
        zealphp_silent_redeclare_enabled = (bool)on;

        /* Auto-engage the define() intercept so the silent-define-redeclare
         * path inside zealphp_define_intercept activates. Without this hook
         * being installed, legacy apps' top-of-file `define('FOO', __DIR__)`
         * fires PHP's native "already defined" E_WARNING on request 2 in
         * coroutine mode — exactly the redeclare crash Stage 3/4 close for
         * functions and classes. The intercept stays installed until
         * explicit zealphp_define_hook(false) or RSHUTDOWN. */
        if ((bool)on && !zealphp_define_hooked) {
            zend_function *df = zend_hash_str_find_ptr(
                CG(function_table), "define", sizeof("define") - 1);
            if (df && df->type == ZEND_INTERNAL_FUNCTION) {
                zealphp_orig_define_handler = df->internal_function.handler;
                df->internal_function.handler = zealphp_define_intercept;
                zealphp_define_hooked = true;
            }
        }
    }
    RETURN_BOOL(prev);
}

/* Public API: zealphp_include_isolation(bool $on = true): bool
 * Enables Stage 7 smart require_once. Returns previous state. */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_include_isolation_reset, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_include_isolation, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, on, _IS_BOOL, 1)
ZEND_END_ARG_INFO()

PHP_FUNCTION(zealphp_include_isolation)
{
    zend_bool on = 1;
    zend_bool on_is_null = 1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(on, on_is_null)
    ZEND_PARSE_PARAMETERS_END();

    bool prev = zealphp_include_isolation_enabled;
    if (!on_is_null) {
        zealphp_include_isolation_enabled = (bool)on;
    }
    RETURN_BOOL(prev);
}

/* Mark a request boundary for Stage 7: clears the current coroutine's
 * "force-re-included this request" set, so files re-execute on the NEXT
 * request while staying idempotent WITHIN a request. In real coroutine
 * operation each request is a fresh coroutine (distinct cid) and on_close
 * clears the set automatically, so this is mainly for explicit request
 * boundaries (non-coroutine sync mode) and for tests that model multiple
 * requests in one process. */
PHP_FUNCTION(zealphp_include_isolation_reset)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (os_get_cid) {
        zend_hash_index_del(&zealphp_coro_reincluded, (zend_ulong)os_get_cid());
    }
    RETURN_TRUE;
}

PHP_MINIT_FUNCTION(zealphp)
{
    /* zealphp_orig_handlers MOVED to RINIT/RSHUTDOWN in the v0.3.9 sec
     * follow-up — even though it only stores raw C function pointers
     * (zif_handler), keeping it persistent meant the dispatch-restore
     * walk in RSHUTDOWN could see stale entries across SAPI module
     * lifecycles where shared objects unload between requests. The
     * lifecycle-aligned table closes that window. See RINIT below. */

    /* HIGH-severity finding from the v0.3.8 security review (fixed):
     * zealphp_callbacks / zealphp_request_constants / zealphp_globals_snapshot
     * were initially persistent=1 but stored request-heap values (Closure
     * zend_object*, per-request zend_string* keys) — classic cross-request
     * UAF when the table outlives the request that populated it. They've
     * been MOVED to PHP_RINIT (init) / PHP_RSHUTDOWN (destroy) so their
     * lifetime matches the PHP request lifecycle. Under OpenSwoole's
     * default config (one PHP request per worker), this still effectively
     * means worker-lifetime; under CLI/FPM/CGI it means per-request.
     * See PHP_RINIT_FUNCTION(zealphp) / PHP_RSHUTDOWN_FUNCTION(zealphp)
     * further down in this file. */

    zend_hash_init(&zealphp_coro_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_constant_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_ini_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_static_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_fn_static_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_reincluded, 256, NULL, ZVAL_PTR_DTOR, 0);
    /* Registry values are borrowed zend_op_array* — we don't own them, no dtor. */
    zend_hash_init(&zealphp_fn_static_registry, 256, NULL, NULL, 0);
    zend_hash_init(&zealphp_coro_globals_deltas,     256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_globals_tombstones, 256, NULL, ZVAL_PTR_DTOR, 0);
    /* persistent=0 — parent snapshot contents come from the per-request heap.
     * Cleared explicitly via zealphp_globals_parent_clear() on disable. */
    zend_hash_init(&zealphp_coro_globals_parent, 128, NULL, ZVAL_PTR_DTOR, 0);
    /* The three *_snapshot tables below capture name SETS (no request-heap
     * zvals — value is IS_LONG 1 throughout). Keys are zend_string* that
     * get persistently-dup'd into the bucket arena by the persistent=1
     * hash. Safe as-is. */
    zend_hash_init(&zealphp_snapshot_files, 256, NULL, ZVAL_PTR_DTOR, 1);
    zend_hash_init(&zealphp_snapshot_classes, 256, NULL, ZVAL_PTR_DTOR, 1);
    zend_hash_init(&zealphp_snapshot_functions, 256, NULL, ZVAL_PTR_DTOR, 1);

    /* Try to hook into OpenSwoole's coroutine scheduler for per-coroutine
     * superglobal isolation. Resolved at runtime via dlsym — if OpenSwoole
     * isn't loaded or the symbols aren't found, we silently skip. */
    void *handle = dlopen(NULL, RTLD_LAZY);
    if (handle) {
        /* Resolve coroutine scheduler hooks. Try multiple symbol patterns
         * to support different OpenSwoole/Swoole versions:
         * 1. Clean C exports (future/proposed)
         * 2. OpenSwoole C++ mangled (current 22.x/26.x)
         * 3. Swoole C++ mangled (for Swoole compat) */
        static const char *yield_names[] = {
            "openswoole_coroutine_set_on_yield",
            "_ZN10openswoole9Coroutine12set_on_yieldEPFvPvE",
            "_ZN6swoole9Coroutine12set_on_yieldEPFvPvE",
            NULL
        };
        static const char *resume_names[] = {
            "openswoole_coroutine_set_on_resume",
            "_ZN10openswoole9Coroutine13set_on_resumeEPFvPvE",
            "_ZN6swoole9Coroutine13set_on_resumeEPFvPvE",
            NULL
        };
        static const char *close_names[] = {
            "openswoole_coroutine_set_on_close",
            "_ZN10openswoole9Coroutine12set_on_closeEPFvPvE",
            "_ZN6swoole9Coroutine12set_on_closeEPFvPvE",
            NULL
        };
        for (const char **n = yield_names; *n && !os_set_on_yield; n++)
            os_set_on_yield = (coro_switch_fn_t)dlsym(handle, *n);
        for (const char **n = resume_names; *n && !os_set_on_resume; n++)
            os_set_on_resume = (coro_switch_fn_t)dlsym(handle, *n);
        for (const char **n = close_names; *n && !os_set_on_close; n++)
            os_set_on_close = (coro_switch_fn_t)dlsym(handle, *n);
        os_get_cid = (coro_get_cid_fn_t)dlsym(handle, "openswoole_coroutine_get_current_id");
        if (!os_get_cid)
            os_get_cid = (coro_get_cid_fn_t)dlsym(handle, "swoole_coroutine_get_current_id");
        dlclose(handle);
    }

    /* Stage 3: register silent-redeclare opcode handlers.
     *
     * Opcode handlers catch RUNTIME declarations (ZEND_DECLARE_FUNCTION /
     * ZEND_DECLARE_CLASS / _DELAYED — emitted for declarations inside an
     * if/function/method scope). They check the target name against the
     * existing CG(function_table) / CG(class_table); if it's already
     * defined, the opcode is skipped silently (first-declaration wins).
     *
     * Top-level (file-scope) `function foo() {}` declarations are bound at
     * COMPILE time via zend_register_top_func — they don't emit a runtime
     * opcode, so the opcode hook can't see them. A naive compile_file
     * intercept that snapshots+restores class_entry pointers around the
     * compile breaks class inheritance / method-table invariants
     * (validated by test failures on 019/020 when wired up). That's
     * deferred to Stage 4 — see docs/architecture/state-isolation-reference.md.
     *
     * The gate is `zealphp_silent_redeclare_enabled`, set via the public
     * `zealphp_silent_redeclare(bool)` PHP function. Handlers fall through
     * (DISPATCH) when the flag is off, so installing them in MINIT is a
     * one-time pointer write with zero runtime overhead until opted in. */
    /* Capture-and-chain (not clobber): if another extension already installed a
     * handler for these opcodes, we defer to it on every fall-through path. */
    zealphp_prev_declare_function = zend_get_user_opcode_handler(ZEND_DECLARE_FUNCTION);
    zend_set_user_opcode_handler(ZEND_DECLARE_FUNCTION,       zealphp_declare_function_handler);
    zealphp_prev_declare_class = zend_get_user_opcode_handler(ZEND_DECLARE_CLASS);
    zend_set_user_opcode_handler(ZEND_DECLARE_CLASS,          zealphp_declare_class_handler);
    zealphp_prev_declare_class_delayed = zend_get_user_opcode_handler(ZEND_DECLARE_CLASS_DELAYED);
    zend_set_user_opcode_handler(ZEND_DECLARE_CLASS_DELAYED,  zealphp_declare_class_delayed_handler);

    /* Stage 7: smart require_once. Same zero-overhead-when-off pattern as
     * Stage 3 — the handler checks zealphp_include_isolation_enabled and
     * chains/DISPATCHes immediately when disabled. */
    zealphp_prev_include_eval = zend_get_user_opcode_handler(ZEND_INCLUDE_OR_EVAL);
    zend_set_user_opcode_handler(ZEND_INCLUDE_OR_EVAL,        zealphp_include_eval_handler);

    /* Stage 4: compile-time silent-redeclare via CG-table swap.
     * Pointer-swap design — O(K) per compile (K = symbols this file
     * declares), independent of total user-symbol count. Reentrant
     * safe (scratch tables are stack-local). See the hook function's
     * docblock for the full design rationale. */
    zealphp_original_compile_file = zend_compile_file;
    zend_compile_file = zealphp_compile_file_hook;

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(zealphp)
{
    /* Process is exiting — skip ALL cleanup. On PHP 8.5+ the shutdown order
     * changed: CG(function_table) and refcounted objects (Closures stored in
     * zealphp_callbacks) may already be freed by the time MSHUTDOWN runs.
     * Calling zend_hash_destroy on tables whose zval dtors touch freed memory
     * causes SIGSEGV. The OS reclaims all process memory on exit — this is
     * the standard pattern used by opcache and other core extensions. */
    return SUCCESS;
}

/* Tracks whether per-request tables have been initialized — protects the
 * persistent=0 hashes from double-init / use-after-destroy if RINIT and
 * RSHUTDOWN fire out of order under any future SAPI weirdness. */
static bool zealphp_request_tables_live = false;

PHP_RINIT_FUNCTION(zealphp)
{
    /* Per-request init for the four tables that hold request-heap values
     * (closures, per-request zend_string keys). Initializing here — and
     * destroying in RSHUTDOWN — ensures their lifetime matches the values
     * they hold.
     *
     * zealphp_orig_handlers was promoted into this lifecycle (was
     * persistent=1 prior to v0.3.9 sec follow-up) so the RSHUTDOWN
     * dispatch-uninstall walk can never see a stale handler from a
     * previously-unloaded shared object. */
    if (!zealphp_request_tables_live) {
        zend_hash_init(&zealphp_orig_handlers,      32,  NULL, NULL,          0);
        zend_hash_init(&zealphp_callbacks,          32,  NULL, ZVAL_PTR_DTOR, 0);
        zend_hash_init(&zealphp_request_constants,  64,  NULL, ZVAL_PTR_DTOR, 0);
        zend_hash_init(&zealphp_globals_snapshot,  128,  NULL, ZVAL_PTR_DTOR, 0);
        zealphp_request_tables_live = true;
    }
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(zealphp)
{
    /* Per-request teardown. Order matters: restore mutated state FIRST so
     * no late call (shutdown handler, persistent object dtor, coroutine
     * resumption) can land on a swapped-but-orphaned handler, THEN destroy
     * the per-request tables. */
    if (zealphp_request_tables_live) {
        /* (1) Restore define() if zealphp_define_hook(true) was called.
         * The intercept handler at zealphp.c writes into
         * zealphp_request_constants — destroying that table while the
         * intercept is still wired would UAF on any post-RSHUTDOWN
         * define() call (CRITICAL C2 from the v0.3.9 security review). */
        if (zealphp_define_hooked && CG(function_table) && zealphp_orig_define_handler) {
            zend_function *df = zend_hash_str_find_ptr(
                CG(function_table), "define", sizeof("define") - 1);
            if (df && df->type == ZEND_INTERNAL_FUNCTION) {
                df->internal_function.handler = zealphp_orig_define_handler;
            }
            zealphp_orig_define_handler = NULL;
            zealphp_define_hooked = false;
        }

        /* (2) Restore any handlers we overrode via zealphp_override.
         * Compare-and-restore protects downstream extensions that hooked
         * the slot after us. */
        if (CG(function_table)) {
            zend_string *key;
            void *orig;
            ZEND_HASH_FOREACH_STR_KEY_PTR(&zealphp_orig_handlers, key, orig) {
                if (!key) continue;
                zend_function *func = zend_hash_find_ptr(CG(function_table), key);
                if (func && func->type == ZEND_INTERNAL_FUNCTION
                    && func->internal_function.handler == zealphp_dispatch) {
                    func->internal_function.handler = (zif_handler)orig;
                }
            } ZEND_HASH_FOREACH_END();
        }

        /* (3) Reset any other static flags that gate writes into the
         * about-to-be-destroyed tables, so that if another request comes
         * in with stale "we already initialized" state, we don't write
         * into a freed table. */
        zealphp_globals_snapshotted = false;

        /* (4) Destroy the per-request tables. ZVAL_PTR_DTOR releases each
         * stored closure / zval refcount. */
        zend_hash_destroy(&zealphp_orig_handlers);
        zend_hash_destroy(&zealphp_callbacks);
        zend_hash_destroy(&zealphp_request_constants);
        zend_hash_destroy(&zealphp_globals_snapshot);
        zealphp_request_tables_live = false;
    }
    return SUCCESS;
}

PHP_MINFO_FUNCTION(zealphp)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "ext-zealphp", "enabled");
    php_info_print_table_row(2, "Version", PHP_ZEALPHP_VERSION);
    php_info_print_table_row(2, "Purpose",
        "Per-request function overrides for long-running PHP servers");
    php_info_print_table_end();
}

/* ── Function entries ────────────────────────────────────────────── */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_override, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, function_name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_restore, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, function_name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_restore_all, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_set, 0, 7, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, get, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, post, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, cookie, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, server, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, files, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, session, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_clear, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_save, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_restore, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, snapshot, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_coroutine_superglobals, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_coroutine_globals, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_coroutine_statics, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_constants_clear, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_ini_restore, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_globals_snapshot, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_globals_clean, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_process_state_snapshot, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_process_state_clean, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_define_hook, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_protect_classes, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, names, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry zealphp_functions[] = {
    PHP_FE(zealphp_override,               arginfo_zealphp_override)
    PHP_FE(zealphp_restore,                arginfo_zealphp_restore)
    PHP_FE(zealphp_restore_all,            arginfo_zealphp_restore_all)
    PHP_FE(zealphp_superglobals_set,       arginfo_zealphp_superglobals_set)
    PHP_FE(zealphp_superglobals_clear,     arginfo_zealphp_superglobals_clear)
    PHP_FE(zealphp_superglobals_save,      arginfo_zealphp_superglobals_save)
    PHP_FE(zealphp_superglobals_restore,   arginfo_zealphp_superglobals_restore)
    PHP_FE(zealphp_coroutine_superglobals, arginfo_zealphp_coroutine_superglobals)
    PHP_FE(zealphp_coroutine_globals,      arginfo_zealphp_coroutine_globals)
    PHP_FE(zealphp_coroutine_statics,      arginfo_zealphp_coroutine_statics)
    PHP_FE(zealphp_constants_clear,        arginfo_zealphp_constants_clear)
    PHP_FE(zealphp_ini_restore,           arginfo_zealphp_ini_restore)
    PHP_FE(zealphp_define_hook,            arginfo_zealphp_define_hook)
    PHP_FE(zealphp_globals_snapshot,       arginfo_zealphp_globals_snapshot)
    PHP_FE(zealphp_globals_clean,          arginfo_zealphp_globals_clean)
    PHP_FE(zealphp_process_state_snapshot, arginfo_zealphp_process_state_snapshot)
    PHP_FE(zealphp_process_state_clean,    arginfo_zealphp_process_state_clean)
    PHP_FE(zealphp_protect_classes,        arginfo_zealphp_protect_classes)
    PHP_FE(zealphp_silent_redeclare,       arginfo_zealphp_silent_redeclare)
    PHP_FE(zealphp_include_isolation,     arginfo_zealphp_include_isolation)
    PHP_FE(zealphp_include_isolation_reset, arginfo_zealphp_include_isolation_reset)
    PHP_FE_END
};

/* ── Module entry ────────────────────────────────────────────────── */

zend_module_entry zealphp_module_entry = {
    STANDARD_MODULE_HEADER,
    "zealphp",
    zealphp_functions,
    PHP_MINIT(zealphp),
    PHP_MSHUTDOWN(zealphp),
    PHP_RINIT(zealphp),
    PHP_RSHUTDOWN(zealphp),
    PHP_MINFO(zealphp),
    PHP_ZEALPHP_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_ZEALPHP
ZEND_GET_MODULE(zealphp)
#endif

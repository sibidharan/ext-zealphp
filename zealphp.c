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
/* ext#44 — mysqlnd vio shim needs the mysqlnd structs; absent headers (a PHP
 * built without mysqlnd) simply compile the shim out. */
#if defined(__has_include)
# if __has_include("ext/mysqlnd/mysqlnd.h") && __has_include("ext/mysqlnd/mysqlnd_structs.h")
#  include "ext/mysqlnd/mysqlnd.h"
#  include "ext/mysqlnd/mysqlnd_structs.h"
#  define ZEALPHP_HAVE_MYSQLND_HEADERS 1
# endif
#endif
#include <unistd.h>
#include <locale.h>
#include <sys/stat.h>

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

/* Per-coroutine working-directory snapshots (framework #323): coro_ptr → cwd
 * string. chdir() is a PROCESS-level syscall, so under coroutine concurrency
 * one request's chdir() leaks into every concurrently-running peer (and the
 * framework's own executeFile() chdir-to-script-dir races itself). When
 * active: on_yield saves the coroutine's cwd and re-parks the process at the
 * worker baseline (so peers and brand-new coroutines start clean); on_resume
 * restores the coroutine's own cwd; on_close drops the entry and re-parks
 * the baseline (a coroutine can end while chdir'd, with no further yield). */
static HashTable zealphp_coro_cwd_snapshots;
static bool zealphp_cwd_isolation_active = false;
static char zealphp_cwd_baseline[MAXPATHLEN] = {0};

/* Per-coroutine setlocale() isolation (the chdir-class process-state family):
 * setlocale() is PROCESS-global (and affects strtolower/number/date formatting
 * everywhere), so one request's locale change leaks into every concurrently
 * running peer. Same shape as the CWD stage: save the coroutine's locale on
 * yield (re-parking the baseline captured at enable time), restore on resume.
 * glibc's setlocale(LC_ALL, NULL) returns a composite string when categories
 * differ ("LC_CTYPE=…;LC_NUMERIC=…") and round-trips through
 * setlocale(LC_ALL, composite) — both forms are handled transparently. */
static HashTable zealphp_coro_locale_snapshots;
static bool zealphp_locale_isolation_active = false;
static char *zealphp_locale_baseline = NULL; /* strdup'd at enable time */

/* Per-coroutine umask() isolation: umask is process-global file-mode state;
 * a request's umask(0077) changes every peer's file creation mid-request.
 * umask() is also the only READ API and it WRITES — which the save side
 * exploits: setting the baseline returns the previous value in one syscall,
 * re-parking peers in the same step. */
static HashTable zealphp_coro_umask_snapshots;
static bool zealphp_umask_isolation_active = false;
static mode_t zealphp_umask_baseline = 0;
static bool zealphp_umask_baseline_set = false;

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
/* PHP 8.4 emits ZEND_BIND_INIT_STATIC_OR_JMP (203) for an INITIALIZED static
 * (`static $x = <expr>;`) and JMPs past ZEND_BIND_STATIC (183) on every call, so
 * 183 NEVER fires for such functions and they were silently never registered →
 * never isolated (8.3 passed, 8.4 leaked). We hook 203 too. */
static zealphp_user_opcode_t zealphp_prev_bind_init_static = NULL;
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
static HashTable zealphp_coro_globals_deltas;      /* coro-ptr → zval-array (live overrides) */
static HashTable zealphp_coro_globals_tombstones;  /* coro-ptr → zval-array (set of deleted keys) */
static bool zealphp_coro_globals_hooks_active = false;

/* Stage-8 object-global isolation (object-store-corruption fix).
 * coro-ptr → zval-array(key → OBJECT). Holds, per coroutine, the OBJECT-valued
 * globals that live as IS_INDIRECT top-code frame CVs in the SHARED
 * EG(symbol_table) (the `App::globalScopeInclude(true)` / zealphp_require_global
 * path). Those slots are deliberately SKIPPED by the delta path (the #10/#033
 * IS_INDIRECT master-frame guard) and so were never isolated — two concurrent
 * coroutines' top-code frames share one symbol table, the second's
 * zend_attach_symbol_table COPY_VALUE-aliases the first's object pointer (no
 * addref) into its own frame slot, and whichever frame unwinds/overwrites first
 * frees the object out from under the peer → EG(objects_store) free-list
 * poison → SIGSEGV in the next zend_objects_store_put (a closure/clone/fopen in
 * ANY coroutine). We can't isolate via the bucket (post-attach each coroutine
 * reads its OWN frame CV directly), so we save the object here on yield + NULL
 * the owning frame CV, and on resume re-derive the resuming coroutine's frame CV
 * and write the object back into it. The registry holds one ref for the
 * suspended lifetime; request-end / on_close frees it (dtor in coroutine ctx via
 * the request-end drain, same contract as the object-delta). */
static HashTable zealphp_coro_indirect_objs;

/* os_get_cid() (integer) → Coroutine* (the pointer the deltas above are keyed by).
 * The scheduler callbacks key everything by the coroutine POINTER (arg), which is
 * NOT available in PHP execution context — but os_get_cid() IS reliable there. So
 * on_yield (where BOTH are reliable) records cid→ptr here, letting the PHP-context
 * request-end drain (zealphp_coroutine_globals_request_end) find and free THIS
 * coroutine's pointer-keyed delta so object globals destruct in-coroutine. */
static HashTable zealphp_coro_cid_to_ptr;

/* Shared parent snapshot. */
static HashTable zealphp_coro_globals_parent;
static bool zealphp_coro_globals_parent_set = false;

/* Stage 3a/3b/3c silent-redeclare master flag. Hoisted to file-scope here
 * (vs co-located with the Stage 3a storage block farther down) so the
 * zealphp_define_intercept hook below can read it. */
static bool zealphp_silent_redeclare_enabled = false;

/* HAZARD-2 fix: per-coroutine save of an IN-PROGRESS CG-swap. compile_file_hook
 * points the process-global CG(class_table)/CG(function_table) at STACK-LOCAL
 * scratch tables during a compile. If that compile yields (a nested autoload of a
 * dependency does hooked I/O), the global CG would stay pointing at THIS
 * coroutine's stack scratch while OTHER coroutines run — their compiles/binds then
 * resolve against the scratch (which has none of the established classes) →
 * "Class X not found" cascade (ASAN+probe confirmed: 101 yields-in-swap under a
 * 12-way burst, CG=coroutine-stack-scratch while EG=real). So on yield we stash
 * the swap and restore CG=EG (the real tables); on resume we re-apply it. Keyed by
 * the OpenSwoole Coroutine* arg (reliable in all scheduler callbacks), NULL dtor
 * (we only borrow the stack-local pointers, never own them). */
static HashTable zealphp_coro_cg_swap_fn;  /* cid → saved CG(function_table) scratch */
static HashTable zealphp_coro_cg_swap_cl;  /* cid → saved CG(class_table) scratch */
/* HAZARD-2: per-coroutine save of EG(in_autoload) (PHP's autoload-recursion set).
 * That set is process-wide; a coroutine suspended mid-autoload leaves the class it
 * is loading in the set, so a PEER coroutine wanting the same class hits the
 * recursion guard (zend_hash_add fails) and gets "Class not found" instead of
 * loading it. Stash+clear on yield, restore on resume, so each coroutine autoloads
 * independently; silent-redeclare first-wins reconciles duplicate compiles. */
static HashTable zealphp_coro_in_autoload;  /* cid → zval-array of class names */

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

/* ── exit()/die() → ZealPHP\HaltException (ext#47) ──────────────────── */

/* Under coroutine concurrency, OpenSwoole turns exit()/die() into a thrown
 * OpenSwoole\ExitException — which extends \Exception, so the ubiquitous
 * Apache-migration idiom `try { … exit; … } catch (\Exception $e)` catches
 * the NORMAL exit and converts it into an error response (FreshRSS 500s on
 * every redirecting POST, DokuWiki's fatal-handler swallow, CodeIgniter 4's
 * 0-byte bodies). This hook layers ABOVE OpenSwoole's: when the exit happens
 * inside a coroutine and the framework's ZealPHP\HaltException class is
 * loaded (it extends \Error BY DESIGN, so catch(\Exception) cannot grab it),
 * we throw THAT instead; in every other situation we delegate to the saved
 * handler (OpenSwoole's — ExitException in-server, true exit on plain CLI).
 * The class is looked up WITHOUT autoload (re-entering the autoloader from
 * an exit site is not safe); the framework guarantees it is loaded at boot.
 * PHP >= 8.4: exit() is a real function — chain its zif handler. PHP < 8.4:
 * chain the ZEND_EXIT user opcode handler, status extracted from op1 the
 * same way OpenSwoole's coro_exit_handler does. */
static bool zealphp_exit_hook_active = false;
#if PHP_VERSION_ID >= 80400
static zif_handler zealphp_orig_exit_handler = NULL;
#elif defined(ZEND_EXIT)
static user_opcode_handler_t zealphp_orig_exit_opcode_handler = NULL;
static bool zealphp_exit_opcode_hooked = false;
#endif

/* ── Per-request define() isolation ─────────────────────────────────── */

/* Track constants defined during the current request so they can be
 * removed on request end. Keys = constant name (case-sensitive zend_string). */
static HashTable zealphp_request_constants;
static bool zealphp_define_hooked = false;
static zif_handler zealphp_orig_define_handler = NULL;

/* #9: request constants removed by zealphp_constants_clear() in COROUTINE mode are
 * ORPHANED (removed from EG without freeing) and parked here per coroutine id, then
 * freed at coroutine close — an immediate free mid-request dangles a cached
 * FETCH_CONSTANT run_time_cache slot (process-shared under opcache) that still
 * points at the struct → use-after-free. Mirrors the deferral
 * zealphp_constants_snapshot_restore() already uses on the on_resume path. Value =
 * array of (zend_long)(uintptr_t) zend_constant*. */
static HashTable zealphp_coro_constant_deferred;

/* ── Per-request $GLOBALS isolation ─────────────────────────────────── */

/* Snapshot of EG(symbol_table) keys at boot. Keys added after the snapshot
 * are considered request-scoped and removed by zealphp_globals_clean(). */
static HashTable zealphp_globals_snapshot;
static bool zealphp_globals_snapshotted = false;

/* Save request-scoped constants for this coroutine on yield.
 *
 * PRESERVE-ADDRESSES isolation: ORPHAN each of this coroutine's request
 * constants — remove it from EG(zend_constants) WITHOUT freeing the
 * zend_constant struct — and stash the struct POINTER so the SAME struct is
 * re-inserted (at the same address) on resume. The struct address therefore
 * survives the yield, so a cached ZEND_FETCH_CONSTANT resolution (the
 * run_time_cache holds the resolved zend_constant*) stays valid across the
 * coroutine switch.
 *
 * The previous design DUP'd the value and zend_hash_del()'d (FREED) the
 * constant, then re-registered a NEW struct on resume. That MOVED the constant,
 * so a cached pointer read whatever constant had since reused the freed address
 * — e.g. WordPress is_multisite() reading DAY_IN_SECONDS (86400) for MULTISITE,
 * wrongly taking the multisite branch -> "Call to undefined function
 * get_network()". Orphaning keeps the address stable and still hides the
 * constant from peer coroutines while this one is suspended. */
/* Free an ORPHANED request constant using only public ZEND_API symbols.
 *
 * The engine's own free_zend_constant() is a STATIC (non-exported) function in
 * Zend/zend_constants.c, so referencing it from a dynamically-loaded extension
 * leaves an undefined symbol: under lazy binding the .so loads fine, but the
 * FIRST time this free path runs the dynamic linker fails to resolve it and
 * aborts the whole worker with `symbol lookup error ... undefined symbol:
 * free_zend_constant` -> exit 127. (Observed as periodic worker recycling on the
 * WordPress render path and as a dropped in-flight request under concurrency.)
 *
 * Request constants come from runtime define() and are therefore always
 * non-persistent; we mirror free_zend_constant's non-persistent branch and
 * defensively refuse to free a persistent (engine/extension) constant. */
static void zealphp_free_orphan_constant(zend_constant *c)
{
    if (!c) {
        return;
    }
    if (ZEND_CONSTANT_FLAGS(c) & CONST_PERSISTENT) {
        return;   /* never our orphan — leave engine/extension constants alone */
    }
    zval_ptr_dtor_nogc(&c->value);
    if (c->name) {
        zend_string_release_ex(c->name, 0);
    }
    efree(c);
}

static void zealphp_constants_snapshot_save(long cid)
{
    if (!zealphp_define_hooked || zend_hash_num_elements(&zealphp_request_constants) == 0) {
        return;
    }

    zval ptrs;            /* name -> (zend_long)(uintptr_t) zend_constant* (orphaned) */
    array_init(&ptrs);
    zval names_snapshot;  /* name -> 1 (the request-constant tracker) */
    array_init(&names_snapshot);

    /* Suppress the constant destructor so zend_hash_del() REMOVES the bucket
     * without freeing the zend_constant — i.e. orphan it, address intact. */
    dtor_func_t orig_dtor = EG(zend_constants)->pDestructor;
    EG(zend_constants)->pDestructor = NULL;

    zend_string *name;
    ZEND_HASH_FOREACH_STR_KEY(&zealphp_request_constants, name) {
        if (name) {
            zend_constant *c = zend_hash_find_ptr(EG(zend_constants), name);
            if (c) {
                zval p;
                ZVAL_LONG(&p, (zend_long)(uintptr_t)c);
                zend_hash_update(Z_ARRVAL(ptrs), name, &p);
                zend_hash_del(EG(zend_constants), name);   /* orphan (no free) */
                zval one;
                ZVAL_LONG(&one, 1);
                zend_hash_update(Z_ARRVAL(names_snapshot), name, &one);
            }
        }
    } ZEND_HASH_FOREACH_END();

    EG(zend_constants)->pDestructor = orig_dtor;

    /* Store the orphan pointers + tracker under this coroutine's ID */
    zval pair;
    array_init(&pair);
    zend_hash_str_update(Z_ARRVAL(pair), "ptrs", sizeof("ptrs") - 1, &ptrs);
    zend_hash_str_update(Z_ARRVAL(pair), "names", sizeof("names") - 1, &names_snapshot);
    zend_hash_index_update(&zealphp_coro_constant_snapshots, (zend_ulong)cid, &pair);

    /* Clear the process-wide tracker */
    zend_hash_clean(&zealphp_request_constants);
}

/* Restore this coroutine's request-scoped constants into EG(zend_constants) on
 * resume — re-inserting the SAME orphaned zend_constant structs (addresses
 * preserved), so cached fetches that resolved them before the yield stay valid. */
static void zealphp_constants_snapshot_restore(long cid)
{
    zval *pair = zend_hash_index_find(&zealphp_coro_constant_snapshots, (zend_ulong)cid);
    if (!pair || Z_TYPE_P(pair) != IS_ARRAY) return;

    zval *ptrs = zend_hash_str_find(Z_ARRVAL_P(pair), "ptrs", sizeof("ptrs") - 1);
    zval *names = zend_hash_str_find(Z_ARRVAL_P(pair), "names", sizeof("names") - 1);
    if (!ptrs || Z_TYPE_P(ptrs) != IS_ARRAY) return;

    /* Re-insert each orphaned request-constant. If a PEER coroutine re-declared
     * the same name while we were suspended, our orphan is unreachable -- BUT a
     * cached op_array's run_time_cache FETCH_CONSTANT slot (shared across
     * coroutines under opcache) may still point at it. Freeing it HERE (mid-
     * request, in on_resume) dangles that cache -> ZEND_FETCH_CONSTANT
     * use-after-free: the long-standing coroutine-legacy WordPress crash,
     * root-caused by ASAN (free was here; the read at ZEND_FETCH_CONSTANT; the
     * "mysqlnd teardown" zend_mm_heap corruption is just a downstream detection).
     * So DEFER such frees to coroutine close (snapshot_delete), after the request
     * has ended and its run_time_cache has been reset, where no fetch can read
     * the freed constant. */
    zval kept;
    array_init(&kept);
    zend_string *name;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(ptrs), name, val) {
        if (name) {
            zend_constant *c = (zend_constant *)(uintptr_t)Z_LVAL_P(val);
            if (!c) {
                continue;
            }
            if (!zend_hash_exists(EG(zend_constants), name)) {
                zend_hash_add_ptr(EG(zend_constants), name, c);   /* re-insert; address preserved */
            } else {
                zval p;
                ZVAL_LONG(&p, (zend_long)(uintptr_t)c);
                zend_hash_next_index_insert(Z_ARRVAL(kept), &p);  /* defer free to coroutine close */
            }
        }
    } ZEND_HASH_FOREACH_END();

    /* Keep "ptrs" holding ONLY the deferred-free orphans (re-inserted ones are now
     * owned by EG). snapshot_delete frees whatever remains here at coroutine close. */
    if (zend_hash_num_elements(Z_ARRVAL(kept)) > 0) {
        zend_hash_str_update(Z_ARRVAL_P(pair), "ptrs", sizeof("ptrs") - 1, &kept);
    } else {
        zval_ptr_dtor(&kept);
        zend_hash_str_del(Z_ARRVAL_P(pair), "ptrs", sizeof("ptrs") - 1);
    }

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

/* Clean up this coroutine's constant snapshot (on close). If the coroutine
 * closed while SUSPENDED (its constants were orphaned by snapshot_save but never
 * re-inserted by a resume), the orphaned structs are not in EG(zend_constants)
 * and would leak — free them here. (Normal path: restore already re-inserted
 * them and deleted "ptrs", so this is a no-op.) */
static void zealphp_constants_snapshot_delete(long cid)
{
    zval *pair = zend_hash_index_find(&zealphp_coro_constant_snapshots, (zend_ulong)cid);
    if (pair && Z_TYPE_P(pair) == IS_ARRAY) {
        zval *ptrs = zend_hash_str_find(Z_ARRVAL_P(pair), "ptrs", sizeof("ptrs") - 1);
        if (ptrs && Z_TYPE_P(ptrs) == IS_ARRAY) {
            zval *val;
            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(ptrs), val) {
                zend_constant *c = (zend_constant *)(uintptr_t)Z_LVAL_P(val);
                if (c) {
                    zealphp_free_orphan_constant(c);
                }
            } ZEND_HASH_FOREACH_END();
        }
    }
    zend_hash_index_del(&zealphp_coro_constant_snapshots, (zend_ulong)cid);
}

/* ── Level 3: Per-coroutine ini_set isolation ──────────────────────── */

/* Some ini directives cannot be safely re-applied on every coroutine switch.
 * `session.*` directives have a stateful on_modify (OnUpdateSession*) that
 * REJECTS changes once a session is active or headers are sent, emitting
 * "Session ini settings cannot be changed after headers have already been sent"
 * on EVERY yield (restore-to-orig) and EVERY resume (re-apply) — a per-switch
 * warning flood that also feeds the async logger, which on its own resume
 * re-triggers the warning (a logging feedback loop that can wedge a worker).
 * They are owned by the framework's per-coroutine session layer anyway, so we
 * exclude them from generic ini snapshotting. This is a documented "pattern
 * that cannot be coroutine-isolated via ini snapshot": directives whose
 * on_modify has side effects or is stage-gated. */
static bool zealphp_ini_isolatable(zend_string *name)
{
    if (!name) return false;
    if (ZSTR_LEN(name) >= sizeof("session.") - 1 &&
        memcmp(ZSTR_VAL(name), "session.", sizeof("session.") - 1) == 0) {
        return false;
    }
    return true;
}

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
        if (name && ini_entry && ini_entry->value && zealphp_ini_isolatable(name)) {
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
        if (name && Z_TYPE_P(val) == IS_STRING && zealphp_ini_isolatable(name)) {
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

/* Superglobal/process-state OWNER — defined with the superglobal snapshot
 * machinery below; forward-declared here because the process-state stages
 * (cwd/locale/umask) share the same gate: a non-owner coroutine (a go()
 * child, a service coroutine) yielding must NOT save-and-re-park state that
 * belongs to the request root. */
static long zealphp_sg_owner_cid;

/* Shared save-side owner gate for the process-state stages: returns 1 when
 * the CURRENT coroutine may snapshot+re-park process-global state (it is the
 * owner, or nobody owns — the legacy first-yielder behaviour). */
/* #37 — set of cids that claimed request state (via zealphp_request_input_set
 * or zealphp_superglobals_owner). A SINGLE owner variable cannot represent
 * several concurrently-SUSPENDED requests in one worker (request B claims
 * while A awaits I/O; A's later save/restore then mis-gates → A's state
 * lost — the 0.3.41-rc regression caught by the 12-concurrent probe). The
 * SET discriminates the only thing that matters: request coroutines save +
 * restore THEIR OWN per-cid/per-ptr snapshots (concurrency-safe by keying),
 * while unclaimed coroutines (go() children, service runners like the
 * async-log channel consumer) never touch the live request state at all.
 * Entries: claim fns add; on_close + superglobals_clear remove. Empty set =
 * no framework claims = legacy pass-all (raw/phpt usage unchanged). */
static HashTable zealphp_request_coro_cids;
/* ptr-keyed mirror, written at SAVE time (os_get_cid is reliable in on_yield,
 * NOT in on_resume) — lets the RESTORE side recognize request coroutines.
 * In framework mode (cid set non-empty) only these ptrs may restore the
 * request-state stages; a service runner that saved during the pre-claim
 * legacy era keeps its stale snapshots PARKED instead of replaying them
 * over a live request (the phpt-057 shape-B residual). */
static HashTable zealphp_request_coro_ptrs;
/* #42 — ADOPTED child coroutines (App::parallel tasks / App::go children).
 * A cid in this set gets its OWN superglobal snapshot lane: at its FIRST
 * yield the save side CAPTURES the live view (which at that moment is the
 * spawning request's state — the parent hasn't yielded yet) WITHOUT
 * clearing it, so the child inherits a copy and the parent keeps the live
 * table (no #332 steal). From the child's first resume on, the normal
 * restore/re-claim/save+clear cycle gives it a full per-coroutine lane —
 * the same machinery that isolates N concurrent requests. Kept SEPARATE
 * from zealphp_request_coro_cids so the process-state stages
 * (constants/ini/statics/$GLOBALS — zealphp_process_state_owner_ok) keep
 * skipping children; only the superglobal lane adopts. Entries are removed
 * at coroutine close. */
static HashTable zealphp_adopted_coro_cids;
static int zealphp_process_state_owner_ok(void)
{
    if (zend_hash_num_elements(&zealphp_request_coro_cids) == 0) return 1;
    if (!os_get_cid) return 1;
    long cur = os_get_cid();
    if (cur <= 0) return 1;   /* unknown context — fail open (legacy) */
    return zend_hash_index_exists(&zealphp_request_coro_cids, (zend_ulong)cur) ? 1 : 0;
}

/* ── Per-coroutine CWD isolation (framework #323) ─────────────────── */

static void zealphp_cwd_snapshot_save(zend_long cid)
{
    if (!zealphp_cwd_isolation_active || zealphp_cwd_baseline[0] == '\0') return;
    /* Owner gate (#31/#32 family): a go() child's (or service coroutine's)
     * yield must not steal the request root's process state — only the owner
     * saves + re-parks; with no owner, legacy first-yielder behaviour.
     * Lane-symmetry exception (2026-06-10) — see zealphp_setting_snapshot_save. */
    if (!zealphp_process_state_owner_ok()
        && !zend_hash_index_exists(&zealphp_coro_cwd_snapshots, (zend_ulong)cid)) return;
    char buf[MAXPATHLEN];
    if (!VCWD_GETCWD(buf, sizeof(buf))) return;
    if (strcmp(buf, zealphp_cwd_baseline) == 0) {
        /* This coroutine sits at the baseline — nothing to isolate. Drop any
         * stale entry so its next resume is a no-op (it may have chdir'd on a
         * previous slice and since returned home). */
        zend_hash_index_del(&zealphp_coro_cwd_snapshots, (zend_ulong)cid);
        return;
    }
    zval z;
    ZVAL_STRING(&z, buf);
    zend_hash_index_update(&zealphp_coro_cwd_snapshots, (zend_ulong)cid, &z);
    /* Re-park the process at the baseline so peers — and brand-new coroutines,
     * which never pass through on_resume with an entry — start clean. */
    if (VCWD_CHDIR(zealphp_cwd_baseline) != 0) { /* dir gone — nothing to do mid-switch */ }
}

static void zealphp_cwd_snapshot_restore(zend_long cid)
{
    if (!zealphp_cwd_isolation_active) return;
    zval *z = zend_hash_index_find(&zealphp_coro_cwd_snapshots, (zend_ulong)cid);
    if (!z || Z_TYPE_P(z) != IS_STRING) return;
    if (VCWD_CHDIR(Z_STRVAL_P(z)) != 0) { /* dir gone — nothing to do mid-switch */ }
}

static void zealphp_cwd_snapshot_delete(zend_long cid)
{
    bool zp_cwd_had_lane = zend_hash_index_exists(&zealphp_coro_cwd_snapshots, (zend_ulong)cid);
    zend_hash_index_del(&zealphp_coro_cwd_snapshots, (zend_ulong)cid);
    if (zealphp_cwd_isolation_active && zealphp_cwd_baseline[0] != '\0') {
        /* Lane-or-owner gate (2026-06-10): re-park when the closing coroutine
         * HAD A LANE (final teardown slice restored its cwd with no further
         * yield) or is a registered request coroutine / legacy mode. An
         * unregistered, lane-less service/timer coroutine closing MID-REQUEST
         * must not re-park the RUNNING request's cwd. */
        if (!zp_cwd_had_lane && !zealphp_process_state_owner_ok()) return;
        /* A coroutine can END while chdir'd (no further yield to re-park it).
         * Re-park the process so the next request starts at the baseline. */
        char buf[MAXPATHLEN];
        if (VCWD_GETCWD(buf, sizeof(buf)) && strcmp(buf, zealphp_cwd_baseline) != 0) {
            if (VCWD_CHDIR(zealphp_cwd_baseline) != 0) { /* dir gone — nothing to do mid-switch */ }
        }
    }
}

/* ── Per-coroutine setlocale() isolation ───────────────────────────── */

static void zealphp_locale_snapshot_save(zend_long cid)
{
    if (!zealphp_locale_isolation_active || !zealphp_locale_baseline) return;
    /* Owner gate (#31/#32 family): a go() child's (or service coroutine's)
     * yield must not steal the request root's process state — only the owner
     * saves + re-parks; with no owner, legacy first-yielder behaviour.
     * Lane-symmetry exception (2026-06-10) — see zealphp_setting_snapshot_save. */
    if (!zealphp_process_state_owner_ok()
        && !zend_hash_index_exists(&zealphp_coro_locale_snapshots, (zend_ulong)cid)) return;
    const char *cur = setlocale(LC_ALL, NULL);
    if (!cur) return;
    if (strcmp(cur, zealphp_locale_baseline) == 0) {
        /* At the baseline — nothing to isolate; drop any stale entry. */
        zend_hash_index_del(&zealphp_coro_locale_snapshots, (zend_ulong)cid);
        return;
    }
    zval z;
    ZVAL_STRING(&z, cur);
    zend_hash_index_update(&zealphp_coro_locale_snapshots, (zend_ulong)cid, &z);
    /* Re-park peers (and brand-new coroutines) at the baseline. */
    setlocale(LC_ALL, zealphp_locale_baseline);
}

static void zealphp_locale_snapshot_restore(zend_long cid)
{
    if (!zealphp_locale_isolation_active) return;
    zval *z = zend_hash_index_find(&zealphp_coro_locale_snapshots, (zend_ulong)cid);
    if (!z || Z_TYPE_P(z) != IS_STRING) return;
    setlocale(LC_ALL, Z_STRVAL_P(z));
}

static void zealphp_locale_snapshot_delete(zend_long cid)
{
    bool zp_loc_had_lane = zend_hash_index_exists(&zealphp_coro_locale_snapshots, (zend_ulong)cid);
    zend_hash_index_del(&zealphp_coro_locale_snapshots, (zend_ulong)cid);
    if (zealphp_locale_isolation_active && zealphp_locale_baseline) {
        /* Lane-or-owner gate (2026-06-10) — see zealphp_cwd_snapshot_delete. */
        if (!zp_loc_had_lane && !zealphp_process_state_owner_ok()) return;
        /* A coroutine can END with a changed locale (no further yield to
         * re-park it) — re-park so the next request starts at the baseline. */
        const char *cur = setlocale(LC_ALL, NULL);
        if (cur && strcmp(cur, zealphp_locale_baseline) != 0) {
            setlocale(LC_ALL, zealphp_locale_baseline);
        }
    }
}

/* ── Per-coroutine umask() isolation ───────────────────────────────── */

static void zealphp_umask_snapshot_save(zend_long cid)
{
    if (!zealphp_umask_isolation_active || !zealphp_umask_baseline_set) return;
    /* Owner gate (#31/#32 family): a go() child's (or service coroutine's)
     * yield must not steal the request root's process state — only the owner
     * saves + re-parks; with no owner, legacy first-yielder behaviour.
     * Lane-symmetry exception (2026-06-10) — see zealphp_setting_snapshot_save. */
    if (!zealphp_process_state_owner_ok()
        && !zend_hash_index_exists(&zealphp_coro_umask_snapshots, (zend_ulong)cid)) return;
    /* One syscall: set the baseline AND read the previous value. */
    mode_t cur = umask(zealphp_umask_baseline);
    if (cur == zealphp_umask_baseline) {
        zend_hash_index_del(&zealphp_coro_umask_snapshots, (zend_ulong)cid);
        return;
    }
    zval z;
    ZVAL_LONG(&z, (zend_long)cur);
    zend_hash_index_update(&zealphp_coro_umask_snapshots, (zend_ulong)cid, &z);
}

static void zealphp_umask_snapshot_restore(zend_long cid)
{
    if (!zealphp_umask_isolation_active) return;
    zval *z = zend_hash_index_find(&zealphp_coro_umask_snapshots, (zend_ulong)cid);
    if (!z || Z_TYPE_P(z) != IS_LONG) return;
    umask((mode_t)Z_LVAL_P(z));
}

static void zealphp_umask_snapshot_delete(zend_long cid)
{
    bool zp_um_had_lane = zend_hash_index_exists(&zealphp_coro_umask_snapshots, (zend_ulong)cid);
    zend_hash_index_del(&zealphp_coro_umask_snapshots, (zend_ulong)cid);
    if (zealphp_umask_isolation_active && zealphp_umask_baseline_set) {
        /* Lane-or-owner gate (2026-06-10) — see zealphp_cwd_snapshot_delete. */
        if (!zp_um_had_lane && !zealphp_process_state_owner_ok()) return;
        /* Re-park for the next request (a coroutine can end mid-umask). */
        umask(zealphp_umask_baseline);
    }
}

/* ── Per-coroutine PHP-setting isolation: timezone + mb encoding ─────
 *
 * date_default_timezone_set() and mb_internal_encoding() write PROCESS-
 * GLOBAL state (DATEG(default_timezone) / MBSTRG(current_internal_encoding))
 * that classic request-style PHP changes per request (WordPress sets the
 * site timezone in core boot; legacy code sets the mb encoding before
 * string work). Under coroutine concurrency request A's setting bleeds
 * into request B — measured 179/250 (tz) and 173/250 (mb) leaks at
 * 49-way concurrency on the 2026-06-10 schematic sweep.
 *
 * Same stage shape as locale/umask: baseline captured at activation;
 * owner-gated save+re-park on yield; restore on resume; re-park on close.
 * Implementation deliberately goes through the ENGINE'S OWN getter/setter
 * functions (zend_call_function on date_default_timezone_get/set and
 * mb_internal_encoding) instead of poking DATEG/MBSTRG directly — uniform
 * across PHP versions, zero module-globals ABI risk, and mbstring being
 * absent simply auto-disables the stage (function lookup fails). The
 * calls are internal functions (no user code, no yield) — safe inside the
 * scheduler callbacks, same execution context the ini stage already uses. */

static int zealphp_sg_dbg(void);
static int  zealphp_tz_isolation_active = 0;
static char *zealphp_tz_baseline = NULL;            /* malloc'd boot value */
static HashTable zealphp_coro_tz_snapshots;         /* cid → zval string */
static int  zealphp_mbenc_isolation_active = 0;
static char *zealphp_mbenc_baseline = NULL;         /* malloc'd boot value */
static HashTable zealphp_coro_mbenc_snapshots;      /* cid → zval string */

/* Call a 0-arg PHP function returning a string; caller frees via efree on
 * the returned char* (NULL on failure / non-string). */
static char *zealphp_call_string_getter(const char *fname)
{
    zval fn, rv;
    ZVAL_STRING(&fn, fname);
    char *out = NULL;
    if (call_user_function(EG(function_table), NULL, &fn, &rv, 0, NULL) == SUCCESS) {
        if (Z_TYPE(rv) == IS_STRING) {
            out = estrndup(Z_STRVAL(rv), Z_STRLEN(rv));
        }
        zval_ptr_dtor(&rv);
    }
    zval_ptr_dtor(&fn);
    return out;
}

/* Call a 1-string-arg PHP function, discarding the return value. */
static void zealphp_call_string_setter(const char *fname, const char *val)
{
    zval fn, rv, arg;
    ZVAL_STRING(&fn, fname);
    ZVAL_STRING(&arg, val);
    if (call_user_function(EG(function_table), NULL, &fn, &rv, 1, &arg) == SUCCESS) {
        zval_ptr_dtor(&rv);
    }
    zval_ptr_dtor(&arg);
    zval_ptr_dtor(&fn);
}

static void zealphp_setting_snapshot_save(zend_long cid, int active, const char *baseline,
                                          HashTable *snaps, const char *getter, const char *setter)
{
    if (!active || !baseline) return;
    /* Owner gate (#31/#32 family) — only request coroutines save + re-park.
     * SYMMETRY EXCEPTION (2026-06-10): a coroutine that already HAS a lane
     * (snapshot present) keeps save+re-park rights even after request-end
     * deregistration. Post-clear() teardown yields (session write, response
     * flush) otherwise RESTORE the lane (restore is ungated by design) but
     * never re-park it — the trace showed 8 consecutive restores with no
     * save, each leaving its tz live for the NEXT request (the 33/250
     * residual). Children never acquire a lane (their saves skip before one
     * exists), so the anti-steal property is preserved. */
    if (!zealphp_process_state_owner_ok()
        && !zend_hash_index_exists(snaps, (zend_ulong)cid)) return;
    char *cur = zealphp_call_string_getter(getter);
    if (!cur) return;
    if (strcmp(cur, baseline) == 0) {
        zend_hash_index_del(snaps, (zend_ulong)cid);
        if (zealphp_sg_dbg()) fprintf(stderr, "[SET %s] save AT-BASELINE key=%ld cur=%ld\n", getter, (long)cid, os_get_cid?os_get_cid():-99);
        efree(cur);
        return;
    }
    zval z;
    ZVAL_STRING(&z, cur);
    zend_hash_index_update(snaps, (zend_ulong)cid, &z);
    if (zealphp_sg_dbg()) fprintf(stderr, "[SET %s] save+repark key=%ld cur=%ld val=%s\n", getter, (long)cid, os_get_cid?os_get_cid():-99, cur);
    efree(cur);
    /* Re-park peers (and brand-new coroutines) at the baseline. */
    zealphp_call_string_setter(setter, baseline);
}

static void zealphp_setting_snapshot_restore(zend_long cid, int active, HashTable *snaps,
                                             const char *setter)
{
    if (!active) return;
    zval *z = zend_hash_index_find(snaps, (zend_ulong)cid);
    if (!z || Z_TYPE_P(z) != IS_STRING) {
        if (zealphp_sg_dbg()) fprintf(stderr, "[SET %s] restore NOSNAP key=%ld\n", setter, (long)cid);
        return;
    }
    if (zealphp_sg_dbg()) fprintf(stderr, "[SET %s] restore key=%ld val=%s\n", setter, (long)cid, Z_STRVAL_P(z));
    zealphp_call_string_setter(setter, Z_STRVAL_P(z));
}

static void zealphp_setting_snapshot_delete(zend_long cid, int active, const char *baseline,
                                            HashTable *snaps, const char *getter, const char *setter)
{
    bool zp_had_lane = zend_hash_index_exists(snaps, (zend_ulong)cid);
    zend_hash_index_del(snaps, (zend_ulong)cid);
    if (active && baseline) {
        /* Re-park when the closing coroutine HAD A LANE (its last resume
         * restored its setting and no further yield re-parked — the final
         * teardown slice) OR is a registered request coroutine / legacy
         * no-claims mode. An UNREGISTERED, LANE-LESS service/timer coroutine
         * closing MID-REQUEST must not re-park: the live setting at that
         * moment belongs to the RUNNING request — re-parking from a
         * Timer-channel helper's close was the original 185/250 leaker. */
        if (!zp_had_lane && !zealphp_process_state_owner_ok()) return;
        char *cur = zealphp_call_string_getter(getter);
        if (cur) {
            if (strcmp(cur, baseline) != 0) {
                /* A coroutine can END with a changed setting (no further yield
                 * to re-park it) — re-park so the next request starts clean. */
                zealphp_call_string_setter(setter, baseline);
            }
            efree(cur);
        }
    }
}

#define zealphp_tz_snapshot_save(cid)    zealphp_setting_snapshot_save((cid), zealphp_tz_isolation_active, zealphp_tz_baseline, &zealphp_coro_tz_snapshots, "date_default_timezone_get", "date_default_timezone_set")
#define zealphp_tz_snapshot_restore(cid) zealphp_setting_snapshot_restore((cid), zealphp_tz_isolation_active, &zealphp_coro_tz_snapshots, "date_default_timezone_set")
#define zealphp_tz_snapshot_delete(cid)  zealphp_setting_snapshot_delete((cid), zealphp_tz_isolation_active, zealphp_tz_baseline, &zealphp_coro_tz_snapshots, "date_default_timezone_get", "date_default_timezone_set")
#define zealphp_mbenc_snapshot_save(cid)    zealphp_setting_snapshot_save((cid), zealphp_mbenc_isolation_active, zealphp_mbenc_baseline, &zealphp_coro_mbenc_snapshots, "mb_internal_encoding", "mb_internal_encoding")
#define zealphp_mbenc_snapshot_restore(cid) zealphp_setting_snapshot_restore((cid), zealphp_mbenc_isolation_active, &zealphp_coro_mbenc_snapshots, "mb_internal_encoding")
#define zealphp_mbenc_snapshot_delete(cid)  zealphp_setting_snapshot_delete((cid), zealphp_mbenc_isolation_active, zealphp_mbenc_baseline, &zealphp_coro_mbenc_snapshots, "mb_internal_encoding", "mb_internal_encoding")

/* ── Per-coroutine libxml_use_internal_errors() isolation ────────────
 *
 * libxml's "use internal errors" FLAG is process-global; under coroutine
 * concurrency request A's enable bleeds into request B (measured 128/250
 * leaks at 49-way concurrency). The flag's backing storage (the
 * LIBXML(error_list) pointer) is NOT dynamically exported by stock PHP
 * builds, so this stage rides the userland function pair exactly like the
 * tz/mbenc stages: libxml_use_internal_errors() with no args reads the
 * flag, with a bool sets it (allocating / freeing the error list the way
 * php-src itself does).
 *
 * FIDELITY NOTE: re-parking an enabled coroutine at a yield frees its
 * COLLECTED errors (that is php-src's own disable semantic). Errors are
 * therefore preserved within a slice — parse + libxml_get_errors() with no
 * yield between, the dominant legacy pattern (local-string parsing never
 * yields) — but not across an I/O yield. The FLAG itself round-trips
 * correctly across any number of yields. Documented honest boundary. */
static int  zealphp_libxml_isolation_active = 0;
static int  zealphp_libxml_baseline = 0;          /* boot flag (normally off) */
static HashTable zealphp_coro_libxml_snapshots;   /* cid → IS_LONG flag */

/* Call libxml_use_internal_errors() — no-arg read / one-bool-arg write. */
static int zealphp_libxml_flag_get(void)
{
    zval fn, rv;
    ZVAL_STRING(&fn, "libxml_use_internal_errors");
    int out = -1;
    if (call_user_function(EG(function_table), NULL, &fn, &rv, 0, NULL) == SUCCESS) {
        if (Z_TYPE(rv) == IS_TRUE) out = 1;
        else if (Z_TYPE(rv) == IS_FALSE) out = 0;
        zval_ptr_dtor(&rv);
    }
    zval_ptr_dtor(&fn);
    /* The no-arg READ form itself returns the current value WITHOUT
     * changing it (PHP 8.x: null $use_errors keeps the state). */
    return out;
}

static void zealphp_libxml_flag_set(int on)
{
    zval fn, rv, arg;
    ZVAL_STRING(&fn, "libxml_use_internal_errors");
    ZVAL_BOOL(&arg, on ? 1 : 0);
    if (call_user_function(EG(function_table), NULL, &fn, &rv, 1, &arg) == SUCCESS) {
        zval_ptr_dtor(&rv);
    }
    zval_ptr_dtor(&fn);
}

static void zealphp_libxml_snapshot_save(zend_long cid)
{
    if (!zealphp_libxml_isolation_active) return;
    /* Owner gate + lane-symmetry exception — see zealphp_setting_snapshot_save. */
    if (!zealphp_process_state_owner_ok()
        && !zend_hash_index_exists(&zealphp_coro_libxml_snapshots, (zend_ulong)cid)) return;
    int cur = zealphp_libxml_flag_get();
    if (cur < 0) return;
    if (cur == zealphp_libxml_baseline) {
        zend_hash_index_del(&zealphp_coro_libxml_snapshots, (zend_ulong)cid);
        return;
    }
    zval z;
    ZVAL_LONG(&z, cur);
    zend_hash_index_update(&zealphp_coro_libxml_snapshots, (zend_ulong)cid, &z);
    zealphp_libxml_flag_set(zealphp_libxml_baseline);
}

static void zealphp_libxml_snapshot_restore(zend_long cid)
{
    if (!zealphp_libxml_isolation_active) return;
    zval *z = zend_hash_index_find(&zealphp_coro_libxml_snapshots, (zend_ulong)cid);
    if (!z || Z_TYPE_P(z) != IS_LONG) return;
    zealphp_libxml_flag_set((int)Z_LVAL_P(z));
}

static void zealphp_libxml_snapshot_delete(zend_long cid)
{
    bool zp_had_lane = zend_hash_index_exists(&zealphp_coro_libxml_snapshots, (zend_ulong)cid);
    zend_hash_index_del(&zealphp_coro_libxml_snapshots, (zend_ulong)cid);
    if (zealphp_libxml_isolation_active
        && (zp_had_lane || zealphp_process_state_owner_ok())) {
        int cur = zealphp_libxml_flag_get();
        if (cur >= 0 && cur != zealphp_libxml_baseline) {
            zealphp_libxml_flag_set(zealphp_libxml_baseline);
        }
    }
}

/* Re-park every active process-setting stage to its baseline. Called from
 * zealphp_superglobals_clear() — the REQUEST-END hook, which runs in the
 * request coroutine's PHP context while it is STILL REGISTERED. This is the
 * reliable end-of-request re-park: the on_close re-park can no longer cover
 * the normal path because clear() removes the cid from the request set
 * first, so the owner-gated delete skips (by design — that same gate is
 * what stops a service/timer coroutine's close wiping a RUNNING request's
 * settings). on_close remains the backstop for the fatal/early-close path,
 * where clear() never ran and the cid is still registered. */
static void zealphp_process_settings_repark(void)
{
    if (zealphp_cwd_isolation_active && zealphp_cwd_baseline[0] != '\0') {
        char zp_buf[MAXPATHLEN];
        if (VCWD_GETCWD(zp_buf, sizeof(zp_buf)) && strcmp(zp_buf, zealphp_cwd_baseline) != 0) {
            if (VCWD_CHDIR(zealphp_cwd_baseline) != 0) { /* dir gone — nothing to do */ }
        }
    }
    if (zealphp_locale_isolation_active && zealphp_locale_baseline) {
        const char *zp_cur = setlocale(LC_ALL, NULL);
        if (zp_cur && strcmp(zp_cur, zealphp_locale_baseline) != 0) {
            setlocale(LC_ALL, zealphp_locale_baseline);
        }
    }
    if (zealphp_umask_isolation_active && zealphp_umask_baseline_set) {
        umask(zealphp_umask_baseline);
    }
    if (zealphp_tz_isolation_active && zealphp_tz_baseline) {
        char *zp_cur = zealphp_call_string_getter("date_default_timezone_get");
        if (zp_cur) {
            if (strcmp(zp_cur, zealphp_tz_baseline) != 0) {
                zealphp_call_string_setter("date_default_timezone_set", zealphp_tz_baseline);
            }
            efree(zp_cur);
        }
    }
    if (zealphp_mbenc_isolation_active && zealphp_mbenc_baseline) {
        char *zp_cur = zealphp_call_string_getter("mb_internal_encoding");
        if (zp_cur) {
            if (strcmp(zp_cur, zealphp_mbenc_baseline) != 0) {
                zealphp_call_string_setter("mb_internal_encoding", zealphp_mbenc_baseline);
            }
            efree(zp_cur);
        }
    }
    if (zealphp_libxml_isolation_active) {
        int zp_cur = zealphp_libxml_flag_get();
        if (zp_cur >= 0 && zp_cur != zealphp_libxml_baseline) {
            zealphp_libxml_flag_set(zealphp_libxml_baseline);
        }
    }
}

/* ── Level 2: Per-coroutine static property isolation ─────────────── */

/* Save static properties of user classes that have been accessed.
 * Only snapshots classes where CE_STATIC_MEMBERS(ce) is non-NULL
 * (lazy — unaccessed classes are skipped). */
/* Forward decl — the shared isolatable filter (defined with the $GLOBALS stage
 * below). A zval is isolatable iff it is NOT an object/resource (those stay
 * process-shared: a DB connection etc. cannot be per-coroutine deep-copied, and
 * dtoring one during snapshot/restore could fire __destruct inside the C
 * on_resume callback). Class- and function-static snapshots reuse this so they
 * match the $GLOBALS discipline exactly — see SECURITY-FIX (objects-in-statics).*/
static bool zealphp_globals_isolatable(zval *v);

static void zealphp_statics_snapshot_save(long cid)
{
    zval snapshot;
    array_init(&snapshot);
    bool has_statics = false;

    zend_string *class_name;
    zval *cls_zv;
    /* HAZARD-2 fix: iterate EG(class_table), NOT CG(class_table). The compile-file
     * hook (Stage 3c) swaps CG(class_table) to a STACK-LOCAL scratch table on the
     * coroutine's stack during compile; EG is deliberately left pointing at the
     * real global table (see the "Stage 3c: swap CG only" note in
     * zealphp_compile_file_hook). This snapshot fires on EVERY coroutine yield —
     * including a yield mid-compile (autoload) or during bailout/teardown — so
     * iterating CG(class_table) there can read a coroutine-stack scratch that
     * becomes a use-after-free once that coroutine's stack is freed (ASAN-confirmed
     * heap-use-after-free at this line on PHP 8.4/8.5). EG(class_table) is never
     * swapped, so it is always the stable, real, process-global table. */
    ZEND_HASH_FOREACH_STR_KEY_VAL(EG(class_table), class_name, cls_zv) {
        if (!class_name) continue;
        zend_class_entry *ce = Z_PTR_P(cls_zv);
        if (!ce || ce->type != ZEND_USER_CLASS) continue;
        if (ce->default_static_members_count == 0) continue;

        zval *statics = CE_STATIC_MEMBERS(ce);
        if (!statics) continue;

        /* Snapshot each static property value */
        zval class_snapshot;
        array_init_size(&class_snapshot, ce->default_static_members_count);
        bool any = false;
        for (int i = 0; i < ce->default_static_members_count; i++) {
            /* SECURITY-FIX (objects-in-statics): leave object/resource statics
             * process-shared, exactly like the $GLOBALS path. Snapshotting them
             * would ZVAL_DUP (incref) on save and zval_ptr_dtor on restore — and
             * if the displaced object's last ref is dropped here, its __destruct
             * fires INSIDE zealphp_on_resume (a C scheduler callback where
             * os_get_cid()==-1), which can re-enter the executor / yield mid-
             * resume and corrupt the scheduler. Scalars/arrays are deep-copied
             * and safe; objects/resources are not isolatable by value. */
            if (!zealphp_globals_isolatable(&statics[i])) continue;
            zval copy;
            ZVAL_DUP(&copy, &statics[i]);
            zend_hash_index_add_new(Z_ARRVAL(class_snapshot), i, &copy);
            any = true;
        }
        if (!any) { zval_ptr_dtor(&class_snapshot); continue; }
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

        /* HAZARD-2 fix: EG(class_table), not CG — see zealphp_statics_snapshot_save. */
        zval *cls_zv = zend_hash_find(EG(class_table), class_name);
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
    /* Global user functions. HAZARD-2 fix: EG(function_table), not CG — CG is
     * swapped to coroutine-stack scratch during compile (see snapshot_save). */
    ZEND_HASH_FOREACH_VAL(EG(function_table), zv) {
        zend_function *fn = Z_PTR_P(zv);
        if (!fn || fn->type != ZEND_USER_FUNCTION) continue;
        zend_op_array *opa = &fn->op_array;
        if (!opa->static_variables) continue;
        HashTable *live = ZEND_MAP_PTR_GET(opa->static_variables_ptr);
        if (!live || live == opa->static_variables) continue;
        cb(opa, live, ctx);
    } ZEND_HASH_FOREACH_END();

    /* Methods of user classes. HAZARD-2 fix: EG(class_table), not CG. */
    zval *czv;
    ZEND_HASH_FOREACH_VAL(EG(class_table), czv) {
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
        /* SECURITY-FIX (objects-in-statics): skip object/resource function-locals
         * — same rationale as the class-static path. Leaving them process-shared
         * avoids a __destruct firing inside zealphp_on_resume. */
        if (!zealphp_globals_isolatable(src)) continue;
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
        /* Deref and judge the underlying value. `global $x; $x = ...` makes the
         * EG(symbol_table) slot IS_REFERENCE (notably on PHP 8.4+, where the
         * engine keeps the global slot as a reference); skipping refs left
         * global-keyword request state un-isolated — it leaked across coroutines
         * (CoroutineIsolationContractTest: 39/40 on 8.4). Mirror the Stage-1
         * superglobal path: isolate ref-of-scalar/array, still leave
         * ref-of-object/resource shared (handled by the switch below). */
        v = Z_REFVAL_P(v);
    }
    switch (Z_TYPE_P(v)) {
        case IS_OBJECT:
        case IS_RESOURCE:
            return false;
        default:
            return true;
    }
}

/* $GLOBALS-only variant that ALSO isolates OBJECTS (resources still excluded).
 * Used by the $GLOBALS delta/reset paths (snapshot_save + reset_to_parent) so
 * object-valued globals — the `global $wpdb; $wpdb = new wpdb()` pattern — are
 * isolated per coroutine instead of leaking across yields (scalars were already
 * isolated; objects were not — empirically 22/24 cross-coroutine leak).
 *
 * SAFETY: an isolated object's refcount is held by the per-coroutine delta while
 * the request runs, so the per-yield reset NEVER drops it to zero mid-switch
 * (the __destruct-in-scheduler-callback UAF the v0.3.12 review guarded against).
 * Its FINAL reference is released by zealphp_coroutine_globals_request_end(),
 * which the framework calls at request-end IN COROUTINE CONTEXT — so an I/O
 * __destruct (e.g. $wpdb closing MySQL under HOOK_ALL) can yield safely. Resources
 * stay excluded (a resource handle's lifecycle can't be snapshot/restored).
 *
 * Deliberately NOT used for class/function statics (451, 612) or the parent
 * baseline (784): those have no request-end drain, so an object there would fall
 * back to on_close destruction (outside a coroutine). Object request-state belongs
 * in $GLOBALS (drained here) or $g — not class/fn statics. */
static bool zealphp_globals_isolatable_obj(zval *v)
{
    if (Z_TYPE_P(v) == IS_REFERENCE) {
        v = Z_REFVAL_P(v);
    }
    return Z_TYPE_P(v) != IS_RESOURCE;   /* objects YES, resources NO */
}

/* Stage-8 helpers (object-store-corruption fix, generalized for ext#52) ──
 *
 * A CV slot of a top-code frame on the CURRENT coroutine's call chain that
 * shares &EG(symbol_table) is exactly a `zealphp_require_global` (Stage-8)
 * request global-scope var. Walking the current coroutine's execute_data chain
 * naturally EXCLUDES the master boot frame (app.php's `$app` etc.) — the
 * master frame is on the MAIN context's stack, never on a worker coroutine's
 * chain — so we isolate request globals without ever touching genuine
 * master-frame CVs.
 *
 * Per-yield hot path (ext#52 perf): snapshot_save tests EVERY IS_INDIRECT
 * bucket against the request frames; re-walking the execute_data chain per key
 * is O(keys × frames). Collect the CV slot ranges ONCE per save instead — the
 * per-key test collapses to a handful of pointer comparisons. */
typedef struct { const zval *lo; const zval *hi; } zealphp_frame_range;
#define ZEALPHP_MAX_REQ_FRAME_RANGES 64

static int zealphp_collect_request_frame_ranges(zealphp_frame_range *out, int max)
{
    int n = 0;
    zend_execute_data *ex = EG(current_execute_data);
    for (; ex && n < max; ex = ex->prev_execute_data) {
        if (!ex->func || !ZEND_USER_CODE(ex->func->type)) continue;
        if (ex->symbol_table != &EG(symbol_table)) continue;
        uint32_t nvars = ex->func->op_array.last_var;
        if (nvars == 0) continue;
        out[n].lo = ZEND_CALL_VAR_NUM(ex, 0);
        out[n].hi = ZEND_CALL_VAR_NUM(ex, nvars);
        n++;
    }
    return n;
}

static zend_always_inline bool zealphp_ptr_in_frame_ranges(
    const zval *t, const zealphp_frame_range *r, int n)
{
    for (int i = 0; i < n; i++) {
        if (t >= r[i].lo && t < r[i].hi) return true;
    }
    return false;
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
        /* #10/#14: a materialised CV / Stage-8 global-scope var is an IS_INDIRECT
         * bucket pointing at a live frame CV slot — capture the pointed-to VALUE,
         * not the indirection wrapper (DUP-ing the wrapper stores a dangling frame
         * pointer and never the real baseline, so reset_to_parent later can't
         * restore it). */
        zval *uv = val;
        if (Z_TYPE_P(uv) == IS_INDIRECT) uv = Z_INDIRECT_P(uv);
        if (Z_ISREF_P(uv)) uv = Z_REFVAL_P(uv);             /* deref global-keyword refs */
        if (Z_TYPE_P(uv) == IS_UNDEF) continue;             /* indirect to an unset CV */
        if (!zealphp_globals_isolatable(uv)) continue;      /* leave objects/resources shared */
        zval copy;
        ZVAL_DUP(&copy, uv);
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

    /* Value-in-place reset (Stage-8 safe): NEVER zend_hash_del a slot. A live
     * global-scope include frame holds an INDIRECT CV into the bucket; deleting it
     * frees the bucket the CV points at -> UAF -> heap corruption on resume.
     * Reset each slot's VALUE in place to the parent baseline (or NULL when the
     * parent has no such key) -- through Z_REFVAL for a live `global $x` binding
     * (unchanged from the proven IS_REF path: keeps `global $wpdb; $wpdb = new
     * wpdb()` whose ctor yields), directly for a plain slot. Bucket addresses stay
     * stable across save/reset/restore; the next coroutine sees NULL (isset=false),
     * and the owning coroutine's value is re-applied from its delta on resume. */
    zend_string *key;
    zval *rv;
    ZEND_HASH_FOREACH_STR_KEY_VAL(&EG(symbol_table), key, rv) {
        if (!key) continue;
        if (zealphp_globals_is_superglobal_key(ZSTR_VAL(key), ZSTR_LEN(key))) continue;
        /* #10/#14: deref an IS_INDIRECT bucket to the real CV slot so we reset the
         * VALUE in place, never the indirection wrapper. Leaving the wrapper
         * untouched left a `global $x = <scalar>` write unreset → the cross-coroutine
         * and cross-request leak. */
        bool indirect = (Z_TYPE_P(rv) == IS_INDIRECT);
        zval *target = rv;
        if (indirect) target = Z_INDIRECT_P(target);
        if (Z_ISREF_P(target)) target = Z_REFVAL_P(target);
        if (!zealphp_globals_isolatable_obj(target)) continue;
        zval *pv = zealphp_coro_globals_parent_set
            ? zend_hash_find(&zealphp_coro_globals_parent, key) : NULL;
        /* #10/033: an IS_INDIRECT slot points at a MASTER-frame CV. If it is NOT in
         * the parent baseline it is the master's own variable (created after
         * isolation was enabled, or a boot-loop local) — NOT a request global — so
         * leave it untouched; NULL-ing it would wipe master state mid-run. A request
         * global (`global $x` / $GLOBALS[]) is a plain / IS_REFERENCE slot and still
         * resets to NULL below; an IS_INDIRECT that IS in the baseline (a master
         * global a request overwrote) still resets to the baseline value. */
        if (!pv && indirect) continue;
        zval old;
        ZVAL_COPY_VALUE(&old, target);
        if (pv) {
            ZVAL_COPY(target, Z_ISREF_P(pv) ? Z_REFVAL_P(pv) : pv);
        } else {
            ZVAL_NULL(target);
        }
        zval_ptr_dtor(&old);
    } ZEND_HASH_FOREACH_END();

    /* Reinstall parent keys not present (e.g. userland unset() removed one). */
    if (zealphp_coro_globals_parent_set) {
        zval *val;
        ZEND_HASH_FOREACH_STR_KEY_VAL(&zealphp_coro_globals_parent, key, val) {
            if (!key) continue;
            if (zend_hash_exists(&EG(symbol_table), key)) continue;
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

    /* ext#52 perf: one frame-chain walk per save; per-key tests below are a
     * few pointer comparisons instead of an O(frames) re-walk each. */
    zealphp_frame_range zp_ranges[ZEALPHP_MAX_REQ_FRAME_RANGES];
    int zp_nranges = zealphp_collect_request_frame_ranges(
        zp_ranges, ZEALPHP_MAX_REQ_FRAME_RANGES);

    zval delta;
    array_init(&delta);

    /* Pass 1: record adds and overrides relative to parent. */
    zend_string *key;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(&EG(symbol_table), key, val) {
        if (!key) continue;
        if (zealphp_globals_is_superglobal_key(ZSTR_VAL(key), ZSTR_LEN(key))) continue;
        /* #10/#14: deref an IS_INDIRECT bucket (materialised CV / Stage-8 global) to
         * its real value BEFORE comparing/capturing — DUP-ing the indirection wrapper
         * into the delta stores a dangling frame pointer that severs/corrupts the CV
         * on restore. */
        bool indirect = (Z_TYPE_P(val) == IS_INDIRECT);
        zval *uv = val;
        if (indirect) {
            uv = Z_INDIRECT_P(uv);
            /* ext#52: a request-frame CV is parked through the Pass-1b registry
             * below (ALL value types as of 0.3.49, not just objects). Capturing
             * it into the delta too would double-apply on resume and write
             * through a bucket a peer's zend_attach_symbol_table may have
             * repointed at ITS frame. The registry owns request-frame slots. */
            if (zealphp_ptr_in_frame_ranges(uv, zp_ranges, zp_nranges)) continue;
        }
        if (Z_ISREF_P(uv)) uv = Z_REFVAL_P(uv);  /* deref global-keyword refs */
        if (Z_TYPE_P(uv) == IS_UNDEF) continue;  /* indirect to an unset CV — nothing to capture */
        if (!zealphp_globals_isolatable_obj(uv)) continue;  /* resources stay shared; objects isolated + drained at request-end */
        zval *pv = zealphp_coro_globals_parent_set
            ? zend_hash_find(&zealphp_coro_globals_parent, key) : NULL;
        /* #10/033: a master-frame CV (IS_INDIRECT) not in the baseline is the
         * master's own variable, not a request global — don't capture it into this
         * coroutine's delta (it would be re-applied on resume and reset on yield). */
        if (!pv && indirect) continue;
        if (pv && zealphp_globals_zval_identical(uv, pv)) continue;
        zval copy;
        ZVAL_DUP(&copy, uv);  /* deep-copy: no COW alias shared across coroutines (esp. arrays) */
        zend_hash_add_new(Z_ARRVAL(delta), key, &copy);
    } ZEND_HASH_FOREACH_END();

    zend_hash_index_update(&zealphp_coro_globals_deltas, (zend_ulong)cid, &delta);

    /* Pass 1b — Stage-8 request-frame globals (store-corruption fix, generalized
     * for ext#52). The delta pass above SKIPS IS_INDIRECT request-frame globals.
     * Every value that belongs to THIS coroutine's request global-scope frame
     * must be PARKED across the yield, because the engine's symbol-table
     * protocol (zend_attach_symbol_table / zend_detach_symbol_table / the
     * re-attach in zend_leave_helper) MOVES values between frame CVs with no
     * refcount change, assuming a single flow of control. A concurrent Stage-8
     * request's attach hijacks the shared bucket, moves OUR value into ITS
     * frame, and leaves a stale alias in our CV — our next write to that CV
     * (e.g. the next foreach iteration) then dtors a payload we no longer own
     * → over-free → heap corruption (ext#52: DokuWiki $config_group, ASAN-
     * pinned; v0.3.47 parked IS_OBJECT only — "scalars rely on the frame" was
     * exactly the bug, strings/arrays die the same way).
     *
     * Park = save the value into the per-coroutine registry (one held ref),
     * NULL the frame CV, and SEVER the bucket to a plain NULL (0.3.49 — no
     * longer left IS_INDIRECT at a parked slot). Invariant: at every suspension
     * point NO bucket is INDIRECT into any request frame, so a peer's attach
     * can only ever move a plain NULL and the parent-baseline reset never
     * writes into a parked frame slot. Re-applied to the correct frame CV (and
     * the bucket re-pointed) by restore Step 2b on resume. */
    {
        zend_string *ikey;
        zval *ival;
        zval *reg = NULL;   /* lazily created per-cid registry array */
        ZEND_HASH_FOREACH_STR_KEY_VAL(&EG(symbol_table), ikey, ival) {
            if (!ikey) continue;
            if (Z_TYPE_P(ival) != IS_INDIRECT) continue;
            zval *tgt = Z_INDIRECT_P(ival);
            /* Frame-range test on the CV SLOT pointer, BEFORE any REF deref —
             * a `global $x`-bound CV is IS_REFERENCE and its refval lives on
             * the HEAP (&ref->val), never inside a VM-stack page, so testing
             * the post-deref pointer silently exempted every REF-wrapped slot
             * from parking (the residual ext#52 crash: the unparked bucket
             * stayed INDIRECT across the yield, a peer's attach moved the REF
             * bytes, and the peer's detach-update freed the zend_reference
             * while stale aliases survived). */
            if (!zealphp_ptr_in_frame_ranges(tgt, zp_ranges, zp_nranges)) continue;  /* skip master-frame CVs */
            if (zealphp_globals_is_superglobal_key(ZSTR_VAL(ikey), ZSTR_LEN(ikey))) continue;
            if (Z_ISREF_P(tgt)) tgt = Z_REFVAL_P(tgt);
            if (Z_TYPE_P(tgt) == IS_UNDEF) continue;      /* nothing to park */

            if (!reg) {
                reg = zend_hash_index_find(&zealphp_coro_indirect_objs, (zend_ulong)cid);
                if (!reg) {
                    zval arr;
                    array_init(&arr);
                    reg = zend_hash_index_add_new(&zealphp_coro_indirect_objs, (zend_ulong)cid, &arr);
                }
            }
            zval held;
            ZVAL_COPY(&held, tgt);                          /* +1: registry holds the value */
            zend_hash_update(Z_ARRVAL_P(reg), ikey, &held); /* frees a prior entry (replaced value) */
            zval_ptr_dtor(tgt);                             /* drop the frame CV's ref */
            ZVAL_NULL(tgt);                                 /* park the frame slot */
            ZVAL_NULL(ival);                                /* sever the bucket→frame edge (plain NULL) */
        } ZEND_HASH_FOREACH_END();
    }

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

    /* #37: never saved (an unclaimed go() child / service runner whose
     * saves the owner gate skipped, or a first resume before any yield) →
     * leave the live table COMPLETELY alone. Unconditionally resetting to
     * the parent baseline here is exactly how the async-log runner's
     * mid-request resume wiped the live owner's $GLOBALS (`$g` → NULL). */
    if (!zend_hash_index_exists(&zealphp_coro_globals_deltas, (zend_ulong)cid)
        && !zend_hash_index_exists(&zealphp_coro_globals_tombstones, (zend_ulong)cid)) {
        return;
    }

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
                /* If the live slot is IS_REFERENCE (another coroutine has bound
                 * `global $key`), write THROUGH the reference instead of
                 * clobbering the wrapper. Clobbering it would detach that
                 * coroutine's CV alias from the symbol table and dangle/free the
                 * wrapper it still holds — a crash under concurrency. Mirrors the
                 * Stage-1 superglobal restore. */
                /* #10/#14: also deref an IS_INDIRECT bucket (materialised CV /
                 * Stage-8 global) to its real slot — ZVAL_DUP over the indirection
                 * wrapper would sever the live frame CV from the symbol table. */
                zval *slot = existing;
                if (Z_TYPE_P(slot) == IS_INDIRECT) slot = Z_INDIRECT_P(slot);
                if (Z_ISREF_P(slot)) slot = Z_REFVAL_P(slot);
                zval old;
                ZVAL_COPY_VALUE(&old, slot);
                ZVAL_DUP(slot, val);
                zval_ptr_dtor(&old);
            } else {
                zval copy;
                ZVAL_DUP(&copy, val);
                zend_hash_add_new(&EG(symbol_table), key, &copy);
            }
        } ZEND_HASH_FOREACH_END();
    }

    /* Step 2b — re-apply Stage-8 object globals into THIS coroutine's frame CVs
     * (object-store-corruption fix). On yield we saved each into the registry and
     * NULLed its frame CV; write it back into the correct frame slot (re-derived
     * from the resuming coroutine's live frame chain — NOT through the shared
     * bucket, which a peer may have repointed at its own frame) and re-point the
     * bucket IS_INDIRECT → our slot so $GLOBALS/symbol-table reads stay coherent.
     * If the owning frame already unwound (a since-returned require_once), fall
     * back to a real bucket zval so the value survives. */
    zval *iobjs = zend_hash_index_find(&zealphp_coro_indirect_objs, (zend_ulong)cid);
    if (iobjs && Z_TYPE_P(iobjs) == IS_ARRAY
        && zend_hash_num_elements(Z_ARRVAL_P(iobjs)) > 0) {
        HashTable *reg = Z_ARRVAL_P(iobjs);

        /* Phase 1 — frames OUTER, O(1) registry hash lookups per CV name
         * (ext#52 perf: replaces a per-key frames×vars scan that was
         * O(keys × frames × vars) per resume). Innermost frame wins, matching
         * the engine's "bucket points at the most recently attached frame".
         * Delete-on-apply: Phase 2 then sees only frame-gone leftovers, and a
         * var the request unset before its yield is never ghost-re-applied. */
        zend_execute_data *ex = EG(current_execute_data);
        for (; ex && zend_hash_num_elements(reg) > 0; ex = ex->prev_execute_data) {
            if (!ex->func || !ZEND_USER_CODE(ex->func->type)) continue;
            if (ex->symbol_table != &EG(symbol_table)) continue;
            zend_op_array *oa = &ex->func->op_array;
            for (uint32_t i = 0; i < oa->last_var; i++) {
                zval *oval = zend_hash_find(reg, oa->vars[i]);
                if (!oval) continue;
                zval *slot = ZEND_CALL_VAR_NUM(ex, i);
                /* Write THROUGH a global-bound reference (a function-level
                 * `global $x` made the frame CV IS_REFERENCE) — the park pass
                 * derefs symmetrically; clobbering the wrapper here would sever
                 * that function's binding after its yield. The bucket below
                 * still points at the CV slot itself (the wrapper). */
                zval *wslot = slot;
                if (Z_ISREF_P(wslot)) wslot = Z_REFVAL_P(wslot);
                zval old;
                ZVAL_COPY_VALUE(&old, wslot);
                ZVAL_COPY(wslot, oval);           /* +1: frame CV gets the value */
                zval_ptr_dtor(&old);              /* drop whatever the slot held (NULL → no-op) */
                zval *b = zend_hash_find(&EG(symbol_table), oa->vars[i]);
                if (b) {
                    if (Z_TYPE_P(b) != IS_INDIRECT) zval_ptr_dtor(b);
                    ZVAL_INDIRECT(b, slot);
                } else {
                    zval ind;
                    ZVAL_INDIRECT(&ind, slot);
                    zend_hash_add_new(&EG(symbol_table), oa->vars[i], &ind);
                }
                zend_hash_del(reg, oa->vars[i]);
            }
        }

        /* Phase 2 — leftovers: frame gone (a since-returned include), keep the
         * value alive as a real bucket zval. */
        if (zend_hash_num_elements(reg) > 0) {
            zend_string *okey;
            zval *oval;
            ZEND_HASH_FOREACH_STR_KEY_VAL(reg, okey, oval) {
                if (!okey) continue;
                zval *b = zend_hash_find(&EG(symbol_table), okey);
                if (b) {
                    if (Z_TYPE_P(b) == IS_INDIRECT) b = Z_INDIRECT_P(b);
                    zval old;
                    ZVAL_COPY_VALUE(&old, b);
                    ZVAL_COPY(b, oval);
                    zval_ptr_dtor(&old);
                } else {
                    zval copy;
                    ZVAL_COPY(&copy, oval);
                    zend_hash_add_new(&EG(symbol_table), okey, &copy);
                }
            } ZEND_HASH_FOREACH_END();
            zend_hash_clean(reg);
        }
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
    /* Releases this coroutine's held refs to its Stage-8 object globals. When
     * called from the request-end drain (coroutine context) an I/O __destruct
     * (e.g. the connection closing under HOOK_ALL) runs safely; from on_close
     * (the abnormal-exit backstop) it shares the same out-of-coroutine caveat as
     * the object-delta drain. */
    zend_hash_index_del(&zealphp_coro_indirect_objs,      (zend_ulong)cid);
}

/* Defined below (~1713); forward-declared so snapshot_save()/snapshot_restore()
 * can reuse the CV-cache-safe in-place superglobal write for the #15 reset/clear. */
static void zealphp_set_superglobal(const char *name, size_t name_len, zval *value);

/* Superglobal OWNERSHIP gating (the go()-child steal — zealphp#332 / #2
 * residual): the live superglobals belong to ONE coroutine — the request
 * root that populated them (zealphp_request_input_set /
 * zealphp_superglobals_owner) or whose resume last restored them. A CHILD
 * coroutine spawned mid-request (`go()` + first yield — the async-log /
 * fire-and-forget pattern) must NOT snapshot-and-clear the live state on
 * ITS yield: that strands the request's superglobals under the child's key
 * and the parent — which never yielded, so nothing ever restores for it —
 * continues with EMPTY superglobals (observed: $_SERVER 22→0 keys,
 * REQUEST_METHOD gone → 501 dispatch, $_SESSION wiped → session-write loss).
 * owner==0 keeps the legacy first-yielder-claims behaviour for callers that
 * never declare ownership (raw phpt tests, non-framework use). os_get_cid()
 * is reliable in on_yield/PHP context but NOT in on_resume, so the owner cid
 * is stored alongside each snapshot and re-established on restore. */
static long zealphp_sg_owner_cid = 0;
/* Debug knob (#32): ZEALPHP_SG_DEBUG=1 traces every superglobal
 * save/skip/clear/restore decision (with coroutine keys, cids and the
 * ownership state) to stderr. Invaluable for scheduler-interplay bugs —
 * the #32 logger-restore wipe was pinned with exactly this trace. */
static int zealphp_sg_debug = -1;
static int zealphp_sg_dbg(void) {
    if (zealphp_sg_debug == -1) {
        const char *e = getenv("ZEALPHP_SG_DEBUG");
        zealphp_sg_debug = (e && *e == '1') ? 1 : 0;
    }
    return zealphp_sg_debug;
}
/* snapshot key (coroutine ptr cast) → IS_LONG owner cid, written at save time. */
static HashTable zealphp_coro_sg_owner_cids;

static void zealphp_snapshot_save(long cid)
{
    /* Guard: EG(symbol_table) may be invalid during coroutine teardown.
     * Check that the table has a valid nTableMask (non-zero when initialized). */
    if (!EG(symbol_table).nTableMask) return;

    /* Owner gate: a non-owner coroutine (a go() child) yielding must leave
     * the live superglobals alone — they belong to its parent request. #42:
     * a KNOWN coroutine — ADOPTED child or registered request coro — still
     * CAPTURES its own snapshot (that capture at an adopted child's first
     * yield IS the inheritance: the live view at that moment is the
     * spawning request's state), but only the OWNER may CLEAR the live
     * table + release ownership. Unregistered service/naive children skip
     * entirely, exactly as before. */
    int zp_capture_only = 0;
    if (zealphp_sg_owner_cid > 0 && os_get_cid) {
        long zealphp_cur_cid = os_get_cid();
        if (zealphp_cur_cid > 0 && zealphp_cur_cid != zealphp_sg_owner_cid) {
            if (!zend_hash_index_exists(&zealphp_adopted_coro_cids, (zend_ulong)zealphp_cur_cid)
                && !zend_hash_index_exists(&zealphp_request_coro_cids, (zend_ulong)zealphp_cur_cid)) {
                if (zealphp_sg_dbg()) fprintf(stderr, "[SGDBG] save SKIP key=%ld cur=%ld owner=%ld\n", cid, zealphp_cur_cid, zealphp_sg_owner_cid);
                return;
            }
            zp_capture_only = 1;
            if (zealphp_sg_dbg()) fprintf(stderr, "[SGDBG] save CAPTURE-ONLY key=%ld cur=%ld owner=%ld\n", cid, zealphp_cur_cid, zealphp_sg_owner_cid);
        }
    }
    if (zealphp_sg_dbg()) {
        zval *sgd = zend_hash_str_find(&EG(symbol_table), "_SESSION", sizeof("_SESSION")-1);
        zval *sgda = sgd && Z_ISREF_P(sgd) ? Z_REFVAL_P(sgd) : sgd;
        fprintf(stderr, "[SGDBG] save+clear key=%ld cur=%ld owner=%ld sess_n=%d\n",
            cid, os_get_cid ? os_get_cid() : -99, zealphp_sg_owner_cid,
            (sgda && Z_TYPE_P(sgda) == IS_ARRAY) ? (int)zend_hash_num_elements(Z_ARRVAL_P(sgda)) : -1);
    }

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

    /* #15: AFTER snapshotting this coroutine's superglobals, reset the live EG
     * slots to empty — the same post-save reset $GLOBALS isolation does via
     * reset_to_parent. Without this, a value this coroutine left in the
     * process-shared symbol table (e.g. $_SESSION) stays live while it is
     * suspended, so the NEXT coroutine's snapshot_save captures it as ITS OWN →
     * cross-coroutine session leak. This coroutine's values are re-applied from
     * the snapshot on its resume, so the round-trip is preserved; peers running
     * in between start from a clean slate. Superglobals are per-request, so the
     * clean baseline is "empty" (each request repopulates its own). */
    if (!zp_capture_only) {
        for (const char **n = sg_names; *n; n++) {
            if (zend_hash_str_find(&EG(symbol_table), *n, strlen(*n))) {
                zval empty;
                array_init(&empty);
                zealphp_set_superglobal(*n, strlen(*n), &empty);
                zval_ptr_dtor(&empty);
            }
        }
    }

    /* Record which cid owns this snapshot (restore can't ask os_get_cid —
     * it returns -1 there) — BOTH paths: the adopted child's resume uses
     * this to re-claim the live table for ITS slice. Release the live-state
     * ownership only on the owner path (#42: capture-only must leave the
     * parent's claim intact — the live slots still hold the parent's data). */
    if (os_get_cid) {
        long zealphp_rc = os_get_cid();
        if (zealphp_rc > 0) {
            zval zc;
            ZVAL_LONG(&zc, zealphp_rc);
            zend_hash_index_update(&zealphp_coro_sg_owner_cids, (zend_ulong)cid, &zc);
        }
    }
    if (!zp_capture_only) {
        zealphp_sg_owner_cid = 0;
    }
}

static void zealphp_snapshot_restore(long cid)
{
    zval *snapshot = zend_hash_index_find(&zealphp_coro_snapshots, (zend_ulong)cid);
    if (!snapshot || Z_TYPE_P(snapshot) != IS_ARRAY) {
        if (zealphp_sg_dbg()) fprintf(stderr, "[SGDBG] restore NOSNAP key=%ld owner=%ld\n", cid, zealphp_sg_owner_cid);
        return;
    }
    if (zealphp_sg_dbg()) {
        zval *sv = zend_hash_str_find(Z_ARRVAL_P(snapshot), "_SESSION", sizeof("_SESSION")-1);
        fprintf(stderr, "[SGDBG] restore key=%ld owner_before=%ld snap_sess_n=%d\n",
            cid, zealphp_sg_owner_cid,
            (sv && Z_TYPE_P(sv) == IS_ARRAY) ? (int)zend_hash_num_elements(Z_ARRVAL_P(sv)) : -1);
    }

    /* Restore gate (#32 → reworked for #40): the original single-owner
     * check ("skip when someone else owns") FALSE-SKIPPED a legitimate
     * request resume — with one worker, request A suspends at I/O, request
     * B claims ownership and (window) hasn't released it when A resumes; A's
     * restore was skipped as "owned", so A continued on B's live $_SERVER
     * (cross-request CONTAMINATION) or on the post-save empties (the
     * labs-dashboard PHP_SELF= empty flavor). A single owner variable cannot
     * represent several concurrently-suspended requests — the same #37
     * disease, in this stage's own gate. The correct discriminator is the
     * #37 ptr set: a KNOWN request coroutine (recorded at its save) always
     * restores ITS OWN snapshot; an unclaimed service runner (the #32
     * async-log case — never saved, never recorded) is skipped in framework
     * mode. Legacy mode (no claims anywhere) restores unconditionally. */
    {
        if (zend_hash_num_elements(&zealphp_request_coro_cids) > 0
            && !zend_hash_index_exists(&zealphp_request_coro_ptrs, (zend_ulong)cid)) {
            if (zealphp_sg_dbg()) fprintf(stderr, "[SGDBG] restore SKIP(not-request-ptr) key=%ld owner=%ld\n", cid, zealphp_sg_owner_cid);
            return;
        }
        /* Re-claim live ownership for the resumed request so ITS children's
         * yields keep being save-gated (the #332 child-steal protection). */
        zval *zealphp_oc = zend_hash_index_find(&zealphp_coro_sg_owner_cids, (zend_ulong)cid);
        long zealphp_snap_cid = (zealphp_oc && Z_TYPE_P(zealphp_oc) == IS_LONG) ? Z_LVAL_P(zealphp_oc) : 0;
        if (zealphp_snap_cid > 0) {
            zealphp_sg_owner_cid = zealphp_snap_cid;
        }
    }

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
        } else if (zend_hash_str_find(&EG(symbol_table), *n, strlen(*n))) {
            /* #15 safety net: this superglobal is NOT in our snapshot but a value
             * is live in EG — a peer set it and finished without yielding (so the
             * post-save reset never ran for it). Clear it so we don't inherit the
             * peer's value (the set-and-finish-without-yield leak window). */
            zval empty;
            array_init(&empty);
            zealphp_set_superglobal(*n, strlen(*n), &empty);
            zval_ptr_dtor(&empty);
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
    /* HAZARD-2: if this coroutine is suspended mid-CG-swap, stash the swap and
     * restore the real (EG) tables so peers don't see our stack-local scratch. */
    if (CG(class_table) != EG(class_table)) {
        zend_ulong k = (zend_ulong)(uintptr_t)arg;
        zend_hash_index_update_ptr(&zealphp_coro_cg_swap_fn, k, CG(function_table));
        zend_hash_index_update_ptr(&zealphp_coro_cg_swap_cl, k, CG(class_table));
        CG(function_table) = EG(function_table);
        CG(class_table)    = EG(class_table);
    }
    /* HAZARD-2: stash + clear this coroutine's autoload-recursion set so peers
     * autoloading the same class don't hit the shared recursion guard. */
    if (EG(in_autoload) && zend_hash_num_elements(EG(in_autoload)) > 0) {
        zval al; array_init(&al);
        zend_string *ak;
        ZEND_HASH_FOREACH_STR_KEY(EG(in_autoload), ak) {
            if (ak) { zval one; ZVAL_LONG(&one, 1); zend_hash_add(Z_ARRVAL(al), ak, &one); }
        } ZEND_HASH_FOREACH_END();
        zend_hash_index_update(&zealphp_coro_in_autoload, (zend_ulong)(uintptr_t)arg, &al);
        zend_hash_clean(EG(in_autoload));
    }
    /* #37 — request-state owner gate, computed BEFORE the SG save below
     * releases ownership. A fire-and-forget go() child's first yield (or a
     * service coroutine like the async-log runner) must NOT save the live
     * request state under ITS key and reset/orphan it to baseline: the
     * still-running parent would see $GLOBALS entries become NULL (#37, the
     * labs-dashboard `$g` wipe), define()s vanish, and statics/ini roll back
     * on the child's later resume. Same #31-family steal already gated for
     * the 7 superglobals (0.3.36) and cwd/locale/umask (0.3.39) — this
     * extends the gate to constants/ini/statics/fn-statics/$GLOBALS, at the
     * dispatcher level so each stage's body stays untouched. No owner
     * claimed (raw non-framework use) → gate passes everyone (legacy
     * first-yielder behaviour, all existing phpt semantics preserved). */
    int zp_req_owner_ok = zealphp_process_state_owner_ok();
    /* Record this coroutine as a KNOWN request coroutine (ptr-keyed, for the
     * restore side) when it is strictly claimed — i.e. its cid is in the
     * claim set, not merely passing via the empty-set legacy mode. */
    if (os_get_cid) {
        long zp_cur = os_get_cid();
        if (zp_cur > 0 && (zend_hash_index_exists(&zealphp_request_coro_cids, (zend_ulong)zp_cur)
                           || zend_hash_index_exists(&zealphp_adopted_coro_cids, (zend_ulong)zp_cur))) {
            zval zp1; ZVAL_LONG(&zp1, 1);
            zend_hash_index_update(&zealphp_request_coro_ptrs, (zend_ulong)(uintptr_t)arg, &zp1);
        }
    }
    if (zealphp_sg_dbg()) fprintf(stderr, "[GDBG] yield key=%ld cur=%ld reqset_n=%d save_ok=%d\n",
        (long)(uintptr_t)arg, os_get_cid ? os_get_cid() : -99,
        (int)zend_hash_num_elements(&zealphp_request_coro_cids), zp_req_owner_ok);
    zealphp_snapshot_save((zend_long)(uintptr_t)arg);
    if (zp_req_owner_ok) {
        zealphp_constants_snapshot_save((zend_long)(uintptr_t)arg);
        zealphp_ini_snapshot_save((zend_long)(uintptr_t)arg);
    }
    zealphp_cwd_snapshot_save((zend_long)(uintptr_t)arg);
    zealphp_locale_snapshot_save((zend_long)(uintptr_t)arg);
    zealphp_umask_snapshot_save((zend_long)(uintptr_t)arg);
    zealphp_tz_snapshot_save((zend_long)(uintptr_t)arg);
    zealphp_mbenc_snapshot_save((zend_long)(uintptr_t)arg);
    zealphp_libxml_snapshot_save((zend_long)(uintptr_t)arg);
    if (zp_req_owner_ok) {
        zealphp_statics_snapshot_save((zend_long)(uintptr_t)arg);
        if (zealphp_fn_statics_active) {
            zealphp_fn_statics_snapshot_save((zend_long)(uintptr_t)arg);
        }
    }
    /* Full $GLOBALS snapshot: runs AFTER superglobals save so the
     * snapshot reflects whatever the request handler last wrote. The
     * is_superglobal_key filter inside this call deliberately skips the
     * 7 SG slots — those are owned by zealphp_snapshot_save above. */
    if (zealphp_coro_globals_hooks_active && zp_req_owner_ok) {
        zealphp_globals_snapshot_save((zend_long)(uintptr_t)arg);
        /* Bridge cid→ptr so the PHP-context request-end drain can free this
         * coroutine's (pointer-keyed) delta. os_get_cid() is reliable in on_yield. */
        if (os_get_cid) {
            zend_hash_index_update_ptr(&zealphp_coro_cid_to_ptr, (zend_ulong)os_get_cid(), arg);
        }
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
    /* HAZARD-2: re-apply this coroutine's in-progress CG-swap (stashed on yield)
     * so its compile continues against its own scratch where it left off. */
    {
        zend_ulong k = (zend_ulong)(uintptr_t)arg;
        HashTable *sfn = zend_hash_index_find_ptr(&zealphp_coro_cg_swap_fn, k);
        if (sfn) {
            HashTable *scl = zend_hash_index_find_ptr(&zealphp_coro_cg_swap_cl, k);
            CG(function_table) = sfn;
            if (scl) CG(class_table) = scl;
            zend_hash_index_del(&zealphp_coro_cg_swap_fn, k);
            zend_hash_index_del(&zealphp_coro_cg_swap_cl, k);
        }
    }
    /* HAZARD-2: restore this coroutine's autoload-recursion set. */
    {
        zval *al = zend_hash_index_find(&zealphp_coro_in_autoload, (zend_ulong)(uintptr_t)arg);
        if (al && Z_TYPE_P(al) == IS_ARRAY && EG(in_autoload)) {
            zend_hash_clean(EG(in_autoload));
            zend_string *ak;
            ZEND_HASH_FOREACH_STR_KEY(Z_ARRVAL_P(al), ak) {
                if (ak) { zval one; ZVAL_LONG(&one, 1); zend_hash_add(EG(in_autoload), ak, &one); }
            } ZEND_HASH_FOREACH_END();
            zend_hash_index_del(&zealphp_coro_in_autoload, (zend_ulong)(uintptr_t)arg);
        }
    }
    /* NB: os_get_cid() == -1 here (see the identity-rationale comment on
     * zealphp_on_yield) — we MUST key on arg, never os_get_cid(), in this path. */
    /* #37 restore gate: in framework mode (claim set non-empty) only KNOWN
     * request coroutines (ptr recorded at save time — os_get_cid is -1 here,
     * so cid can't be consulted) may replay request-state snapshots. Gated
     * children never saved (natural no-ops), and a service runner that
     * saved during the pre-claim legacy era keeps its STALE snapshots
     * parked instead of reset_to_parent-ing the live request's $GLOBALS /
     * cwd / statics (the phpt-057 shape-B residual). Legacy mode (no claims
     * anywhere) restores unconditionally — raw/phpt semantics unchanged. */
    int zp_restore_ok = 1;
    if (zend_hash_num_elements(&zealphp_request_coro_cids) > 0
        && !zend_hash_index_exists(&zealphp_request_coro_ptrs, (zend_ulong)(uintptr_t)arg)) {
        zp_restore_ok = 0;
    }
    /* Full $GLOBALS restore runs BEFORE superglobals restore so that
     * the superglobals layer can overwrite the 7 SG slots last and win
     * any race against stale snapshot data. */
    if (zealphp_coro_globals_hooks_active && zp_restore_ok) {
        zealphp_globals_snapshot_restore((zend_long)(uintptr_t)arg);
    }
    zealphp_snapshot_restore((zend_long)(uintptr_t)arg);
    if (zp_restore_ok) {
        zealphp_constants_snapshot_restore((zend_long)(uintptr_t)arg);
        zealphp_ini_snapshot_restore((zend_long)(uintptr_t)arg);
        zealphp_cwd_snapshot_restore((zend_long)(uintptr_t)arg);
        zealphp_locale_snapshot_restore((zend_long)(uintptr_t)arg);
        zealphp_umask_snapshot_restore((zend_long)(uintptr_t)arg);
        zealphp_tz_snapshot_restore((zend_long)(uintptr_t)arg);
        zealphp_mbenc_snapshot_restore((zend_long)(uintptr_t)arg);
        zealphp_libxml_snapshot_restore((zend_long)(uintptr_t)arg);
        zealphp_statics_snapshot_restore((zend_long)(uintptr_t)arg);
        if (zealphp_fn_statics_active) {
            zealphp_fn_statics_snapshot_restore((zend_long)(uintptr_t)arg);
        }
    }
}

static void zealphp_on_close(void *arg)
{
    /* Chain to OpenSwoole's PHPCoroutine::on_close FIRST */
    if (orig_on_close) orig_on_close(arg);
    if (!arg) return;
    /* HAZARD-2: drop any stashed CG-swap for a coroutine torn down mid-compile. */
    zend_hash_index_del(&zealphp_coro_cg_swap_fn, (zend_ulong)(uintptr_t)arg);
    zend_hash_index_del(&zealphp_coro_cg_swap_cl, (zend_ulong)(uintptr_t)arg);
    zend_hash_index_del(&zealphp_coro_in_autoload, (zend_ulong)(uintptr_t)arg);
    /* Release live-state ownership held by the closing coroutine (#32):
     * a request that ends while owning (e.g. finished without a final
     * yield) must not leave a stale owner that gates peers' restores. #40:
     * ALSO release by the closing coroutine's cid — a request that claimed
     * but never yielded has no recorded snapshot owner, so the record-based
     * check alone left its claim dangling forever. */
    {
        zval *zealphp_oc = zend_hash_index_find(&zealphp_coro_sg_owner_cids, (zend_ulong)(uintptr_t)arg);
        if (zealphp_oc && Z_TYPE_P(zealphp_oc) == IS_LONG
            && Z_LVAL_P(zealphp_oc) == zealphp_sg_owner_cid) {
            zealphp_sg_owner_cid = 0;
        }
        if (os_get_cid) {
            long zp_close_cid = os_get_cid();
            if (zp_close_cid > 0 && zp_close_cid == zealphp_sg_owner_cid) {
                zealphp_sg_owner_cid = 0;
            }
        }
    }
    zend_hash_index_del(&zealphp_coro_snapshots, (zend_ulong)(uintptr_t)arg);
    zend_hash_index_del(&zealphp_coro_sg_owner_cids, (zend_ulong)(uintptr_t)arg);
    /* #37: a request coroutine closing without reaching superglobals_clear
     * (fatal, early return) must not stay in the request set forever. */
    if (os_get_cid) {
        long zp_c = os_get_cid();
        if (zp_c > 0) {
            zend_hash_index_del(&zealphp_request_coro_cids, (zend_ulong)zp_c);
            zend_hash_index_del(&zealphp_adopted_coro_cids, (zend_ulong)zp_c);
        }
    }
    zend_hash_index_del(&zealphp_request_coro_ptrs, (zend_ulong)(uintptr_t)arg);
    zealphp_constants_snapshot_delete((zend_long)(uintptr_t)arg);
    /* #9: free the request constants that zealphp_constants_clear() ORPHANED on
     * this coroutine — now safe, the request has ended and its run_time_cache is
     * gone, so no FETCH_CONSTANT can read the freed struct. KEY FIX (2026-06-10):
     * the producer (zealphp_constants_clear) parks the orphans keyed by
     * os_get_cid() — a SMALL coroutine id — but this drain was keyed by the
     * (uintptr_t)arg POINTER. The two key spaces never collide, so the deferred
     * orphans were NEVER found here and leaked (bounded by worker recycle).
     * os_get_cid() is reliable in a coroutine's own close callback (see the
     * identity-rationale comment above zealphp_on_yield), so drain by cid —
     * matching the producer. */
    if (os_get_cid) {
        long zp_cc = os_get_cid();
        if (zp_cc > 0) {
            zval *cdef = zend_hash_index_find(&zealphp_coro_constant_deferred, (zend_ulong)zp_cc);
            if (cdef && Z_TYPE_P(cdef) == IS_ARRAY) {
                zval *cv;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(cdef), cv) {
                    zend_constant *c = (zend_constant *)(uintptr_t)Z_LVAL_P(cv);
                    if (c) {
                        zealphp_free_orphan_constant(c);
                    }
                } ZEND_HASH_FOREACH_END();
            }
            zend_hash_index_del(&zealphp_coro_constant_deferred, (zend_ulong)zp_cc);
        }
    }
    zealphp_ini_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_cwd_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_locale_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_umask_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_tz_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_mbenc_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_libxml_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_statics_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_fn_statics_snapshot_delete((zend_long)(uintptr_t)arg);
    zealphp_globals_snapshot_delete((zend_long)(uintptr_t)arg);
    /* Stage 7: drop this coroutine's force-re-included set. Keyed by os_get_cid()
     * (the handler's key); during a coroutine's own close callback os_get_cid()
     * still returns its cid. Prevents the per-request set from accumulating
     * across the worker's lifetime (cids are monotonic). */
    if (os_get_cid) {
        zend_hash_index_del(&zealphp_coro_reincluded, (zend_ulong)os_get_cid());
        /* Drop the cid→ptr bridge entry (request-end drain usually removed it
         * already; this covers coroutines closed without a request-end drain). */
        zend_hash_index_del(&zealphp_coro_cid_to_ptr, (zend_ulong)os_get_cid());
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
#ifdef ZEALPHP_HAVE_MYSQLND_HEADERS
/* ── mysqlnd vio orig_path allocator-consistency shim (ext#44) ────────
 *
 * Root cause (gdb-verified at field level): for a PERSISTENT-KEYED connect,
 * the generic xport layer strdups stream->orig_path PERSISTENTLY
 * (transports.c: pestrdup(orig_path, persistent_id ? 1 : 0) → malloc), but a
 * coroutine-hooked transport factory (OpenSwoole's tcp_socket/coroutine ops)
 * returns an emalloc'd NON-persistent stream. php_stream_free then frees
 * orig_path with stream->is_persistent==0 → _efree() on a malloc'd pointer →
 * Zend MM metadata catastrophe ("zend_mm_heap corrupted") at mysqlnd
 * connection teardown — the coroutine-legacy WordPress crash. (Stock builds
 * are consistent: xp_socket honours persistent_id, so the stream itself is
 * malloc'd persistent and every pefree pairs up. And under USE_ZEND_ALLOC=0
 * both allocators are malloc → the A/B that pinned this.)
 *
 * Fix: wrap the vio open methods via the EXPORTED method table
 * (mysqlnd_mysqlnd_vio_methods, .data — vio instances copy it BY VALUE at
 * creation, so patching at boot covers every later vio) and re-pair
 * orig_path with the stream's actual allocator right after creation. The
 * free(old) is safe precisely because persistent-keyed ⇒ pestrdup(,1) ⇒
 * malloc'd. Covers every later free path (dtor, explicit close, error).
 * Opt out: ZEALPHP_MYSQLND_SHIM_DISABLE=1. */
static func_mysqlnd_vio__open_stream zealphp_orig_vio_open_tcp = NULL;
static func_mysqlnd_vio__open_stream zealphp_orig_vio_open_pipe = NULL;

static void zealphp_fix_stream_orig_path(php_stream *s, bool persistent_keyed,
                                         const MYSQLND_CSTRING scheme)
{
    if (s && persistent_keyed && !s->is_persistent && s->orig_path) {
        char *zp_fixed = pestrdup(s->orig_path, 0);
        free(s->orig_path);   /* persistent-keyed ⇒ malloc'd (see block comment) */
        s->orig_path = zp_fixed;
        if (zealphp_sg_dbg()) {
            fprintf(stderr, "[MND] orig_path re-paired stream=%p scheme=%.*s\n",
                    (void *)s, (int)scheme.l, scheme.s ? scheme.s : "");
        }
    }
}

static php_stream * zealphp_vio_open_tcp_or_unix_shim(MYSQLND_VIO * const vio,
        const MYSQLND_CSTRING scheme, const bool persistent,
        MYSQLND_STATS * const conn_stats, MYSQLND_ERROR_INFO * const error_info)
{
    php_stream *zp_s = zealphp_orig_vio_open_tcp(vio, scheme, persistent, conn_stats, error_info);
    zealphp_fix_stream_orig_path(zp_s, persistent, scheme);
    return zp_s;
}

static php_stream * zealphp_vio_open_pipe_shim(MYSQLND_VIO * const vio,
        const MYSQLND_CSTRING scheme, const bool persistent,
        MYSQLND_STATS * const conn_stats, MYSQLND_ERROR_INFO * const error_info)
{
    php_stream *zp_s = zealphp_orig_vio_open_pipe(vio, scheme, persistent, conn_stats, error_info);
    zealphp_fix_stream_orig_path(zp_s, persistent, scheme);
    return zp_s;
}

static void zealphp_install_mysqlnd_vio_shim(void)
{
    if (zealphp_orig_vio_open_tcp) {
        return;   /* installed once */
    }
    const char *zp_dis = getenv("ZEALPHP_MYSQLND_SHIM_DISABLE");
    if (zp_dis && *zp_dis == '1') {
        return;
    }
    MYSQLND_CLASS_METHODS_TYPE(mysqlnd_vio) *zp_m =
        (MYSQLND_CLASS_METHODS_TYPE(mysqlnd_vio) *)dlsym(RTLD_DEFAULT, "mysqlnd_mysqlnd_vio_methods");
    if (!zp_m || !zp_m->open_tcp_or_unix || !zp_m->open_pipe) {
        return;   /* mysqlnd absent / unexported — shim stays off */
    }
    zealphp_orig_vio_open_tcp = zp_m->open_tcp_or_unix;
    zp_m->open_tcp_or_unix = zealphp_vio_open_tcp_or_unix_shim;
    zealphp_orig_vio_open_pipe = zp_m->open_pipe;
    zp_m->open_pipe = zealphp_vio_open_pipe_shim;
}
#endif /* ZEALPHP_HAVE_MYSQLND_HEADERS */

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
#ifdef ZEALPHP_HAVE_MYSQLND_HEADERS
    /* ext#44 — the orig_path mismatch only exists under hooked transports,
     * so the vio shim rides the same activation. */
    zealphp_install_mysqlnd_vio_shim();
#endif
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

/* zealphp_request_input_set(array $get,$post,$cookie,$server,$files,$request): void
 * Re-establishes ONLY the 6 request-INPUT superglobals (NOT $_SESSION) via the
 * same dual-write (EG(symbol_table) + PG(http_globals)) as zealphp_set_superglobal,
 * so the auto-global JIT resolves function-scope $_GET correctly. Deliberately
 * excludes $_SESSION so the session manager's `$_SESSION = &$g->session` live
 * reference is never clobbered.
 *
 * HAZARD-3 fix (request-input coroutine leak): the request-input populate at
 * request start writes process-global $GLOBALS BEFORE the handler coroutine's
 * isolation baseline, so a concurrent overlapping request can overwrite it and
 * the handler reads the wrong request's input. ZealPHP calls this in the HANDLER
 * coroutine, right before dispatch (no intervening yield), from the per-coroutine
 * OpenSwoole request — guaranteeing the handler sees its OWN input. */
PHP_FUNCTION(zealphp_request_input_set)
{
    zval *get, *post, *cookie, *server, *files, *request;

    ZEND_PARSE_PARAMETERS_START(6, 6)
        Z_PARAM_ARRAY(get)
        Z_PARAM_ARRAY(post)
        Z_PARAM_ARRAY(cookie)
        Z_PARAM_ARRAY(server)
        Z_PARAM_ARRAY(files)
        Z_PARAM_ARRAY(request)
    ZEND_PARSE_PARAMETERS_END();

    zealphp_set_superglobal("_GET",     sizeof("_GET")-1,     get);
    zealphp_set_superglobal("_POST",    sizeof("_POST")-1,    post);
    zealphp_set_superglobal("_COOKIE",  sizeof("_COOKIE")-1,  cookie);
    zealphp_set_superglobal("_SERVER",  sizeof("_SERVER")-1,  server);
    zealphp_set_superglobal("_FILES",   sizeof("_FILES")-1,   files);
    zealphp_set_superglobal("_REQUEST", sizeof("_REQUEST")-1, request);

    /* The populating coroutine owns the live superglobals from here — its
     * go() children's yields must not snapshot-and-steal them (#332). */
    if (os_get_cid) {
        long zealphp_oc = os_get_cid();
        if (zealphp_oc > 0) {
            zealphp_sg_owner_cid = zealphp_oc;
            zval zp1; ZVAL_LONG(&zp1, 1);
            zend_hash_index_update(&zealphp_request_coro_cids, (zend_ulong)zealphp_oc, &zp1);
        }
        if (zealphp_sg_dbg()) fprintf(stderr, "[SGDBG] claim(input_set) cid=%ld\n", zealphp_oc);
    }
}

/* zealphp_superglobals_owner(): bool
 * Claims live-superglobal OWNERSHIP for the current coroutine (#332 — see
 * the zealphp_sg_owner_cid block). For frameworks that populate the
 * superglobals via plain PHP writes (no zealphp_request_input_set call):
 * after this, only THIS coroutine's yields snapshot-and-clear the live
 * state; a go() child's yield leaves it alone. Returns false outside a
 * coroutine. */
PHP_FUNCTION(zealphp_superglobals_owner)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (os_get_cid) {
        long zealphp_oc = os_get_cid();
        if (zealphp_oc > 0) {
            zealphp_sg_owner_cid = zealphp_oc;
            zval zp1; ZVAL_LONG(&zp1, 1);
            zend_hash_index_update(&zealphp_request_coro_cids, (zend_ulong)zealphp_oc, &zp1);
            RETURN_TRUE;
        }
    }
    RETURN_FALSE;
}

/* zealphp_superglobals_adopt(): bool
 * #42 — registers the CURRENT coroutine as an ADOPTED request child. Call at
 * the top of a `go()` child / App::parallel task spawned inside a request.
 * Effect: the child gets its OWN superglobal snapshot lane — its first yield
 * CAPTURES the live superglobals (the spawning request's state — the parent
 * hasn't yielded between go() and the child's first yield, so the live view
 * is the parent's) WITHOUT clearing them (no #332 steal: the parent keeps
 * the live table and its ownership claim). Every later child resume/yield
 * runs the normal restore/save+clear cycle, so `$_SERVER` et al. survive the
 * child's own yields exactly like they do for a request coroutine. Does NOT
 * register into the process-state owner set — constants/ini/statics/$GLOBALS
 * stages keep treating the child as a child. Returns false outside a
 * coroutine; idempotent. */
PHP_FUNCTION(zealphp_superglobals_adopt)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (os_get_cid) {
        long zealphp_ac = os_get_cid();
        if (zealphp_ac > 0) {
            zval zp1; ZVAL_LONG(&zp1, 1);
            zend_hash_index_update(&zealphp_adopted_coro_cids, (zend_ulong)zealphp_ac, &zp1);
            RETURN_TRUE;
        }
    }
    RETURN_FALSE;
}

/* zealphp_superglobals_clear(): void
 * Resets all superglobals to empty arrays. Called at request end
 * to prevent cross-request leakage in coroutine mode. */
PHP_FUNCTION(zealphp_superglobals_clear)
{
    /* Request end — release ownership so the next holder starts clean
     * (#32), but ONLY when this coroutine is the current owner (#40): a
     * request finishing its teardown while a RESUMED peer holds the live
     * state must not strip the peer's claim (that reopens the child-steal
     * window mid-peer-request). */
    if (os_get_cid) {
        long zp_c = os_get_cid();
        if (zp_c > 0) {
            if (zealphp_sg_owner_cid == zp_c) {
                zealphp_sg_owner_cid = 0;
            }
            zend_hash_index_del(&zealphp_request_coro_cids, (zend_ulong)zp_c);
        }
    } else {
        zealphp_sg_owner_cid = 0;
    }
    ZEND_PARSE_PARAMETERS_NONE();

    /* Request end IS the reliable re-park point for the process-setting
     * stages (cwd/locale/umask/tz/mbenc): we are in the request coroutine's
     * own PHP context, so the live settings are THIS request's — restore the
     * worker baseline so the NEXT fresh request doesn't inherit them (a fresh
     * coroutine has no snapshot to restore, so whatever is live at its start
     * becomes its state). The on_close re-park is owner-gated and the cid was
     * just removed above, so without this call the normal request-end path
     * would never re-park (measured: 33/250 residual tz leaks — every one a
     * fresh request starting right after a tz-changing request ended). */
    zealphp_process_settings_repark();

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
        } else if (zend_hash_str_find(&EG(symbol_table), *n, strlen(*n))) {
            /* #15: removing restore. A superglobal ABSENT from this snapshot but
             * LIVE in EG was populated by a peer coroutine — leaving it would let
             * this coroutine read the peer's value (e.g. a sensitive $_SESSION
             * token/role). Empty it so the restore is isolating, mirroring the
             * scheduler restore's safety net (zealphp_snapshot_restore). Without
             * this the userland save/restore twin leaked while the scheduler path
             * (fixed in 0.3.31) did not. */
            zval empty;
            array_init(&empty);
            zealphp_set_superglobal(*n, strlen(*n), &empty);
            zval_ptr_dtor(&empty);
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
        zend_hash_clean(&zealphp_coro_indirect_objs);
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

/* ── zealphp_globals_baseline_refresh(): bool ──────────────────────────
 * Re-capture the parent $GLOBALS baseline from the CURRENT symbol table.
 *
 * The baseline is snapshotted once, at zealphp_coroutine_globals(true) activation
 * time. Boot-time writes to $GLOBALS that land AFTER activation — e.g. an app
 * bootstrap include (load.php) at worker start — are therefore NOT in the
 * baseline. They survive for the first request coroutine (which still sees the
 * live symbol table) but vanish for every subsequent one the moment a yield
 * resets $GLOBALS to that stale baseline (#26: only 1/N concurrent requests saw
 * the boot global). Calling this once after boot completes folds those writes
 * into the baseline, so every request coroutine sees them.
 *
 * No-op (returns false) when the globals-isolation feature isn't active. */
PHP_FUNCTION(zealphp_globals_baseline_refresh)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (!zealphp_coro_globals_hooks_active) {
        RETURN_FALSE;
    }
    zealphp_globals_parent_clear();
    zealphp_globals_parent_snapshot();
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

    /* The ZEND_BIND_STATIC / ZEND_BIND_INIT_STATIC_OR_JMP opcode hooks are
     * installed in MINIT (before any user code is compiled) — see the comment
     * there. Installing at runtime missed every function compiled before this
     * point. The handler is flag-gated, so it was a no-op until now anyway. */
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

    /* HAZARD-2 fix: NEVER delete user classes/functions from the shared
     * process-global CG tables while silent-redeclare is on (coroutine-legacy).
     *
     * This "fresh process state per request" cleanup is the POOL-mode mechanism:
     * a pool worker handles one request at a time, so deleting per-request
     * classes/functions to let the next request re-declare them is safe there
     * (and silent-redeclare is OFF in pool mode). Under COROUTINE concurrency the
     * same delete is a data race: CG(class_table)/CG(function_table) are
     * process-global and shared across all in-flight coroutines, so one request's
     * request-end clean (e.g. CoSessionManager calling clean(6)) deletes a class
     * (TableSessionHandler, an autoloaded controller, …) that a CONCURRENT
     * coroutine is mid-use → intermittent "Class \"X\" not found" fatal → worker
     * abort (ASAN-confirmed: the class-not-found cascade behind the shutdown
     * bad-free on PHP 8.4/8.5). When silent-redeclare is on it ALSO makes the
     * delete redundant: re-included files re-declare first-wins, so classes stay
     * registered and stable WITHOUT being torn down per request. Strip the
     * class(2)+function(4) bits; the include-cache reset is handled per-coroutine
     * by Stage 7 (zealphp_include_eval_handler). */
    if (zealphp_silent_redeclare_enabled) {
        flags &= ~(zend_long)(2 | 4);
    }

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

/* zealphp_coroutine_globals_request_end(): void
 *
 * Request-end drain for per-coroutine $GLOBALS isolation. The framework calls
 * this from the request coroutine's PHP context (CoSessionManager /
 * SessionManager `finally`, AFTER the handler + response). It releases the LAST
 * reference to every isolated OBJECT global HERE — in coroutine context — so an
 * object's __destruct may yield (e.g. `$wpdb` closing its MySQL socket under
 * HOOK_ALL). Without it the final ref falls to on_close (a C scheduler callback
 * OUTSIDE any coroutine), where an I/O __destruct throws "API must be called in
 * the coroutine" and the connection never closes cleanly.
 *
 * Two references can pin an isolated object: (1) this coroutine's per-yield delta
 * (pointer-keyed — found via the cid→ptr bridge), and (2) the live EG slot. We
 * drop the delta first, then reset EG to the parent baseline; whichever held the
 * object's final ref, that ref is released in THIS coroutine and __destruct runs
 * here. No-op unless per-coroutine $GLOBALS isolation is active. Idempotent:
 * on_close's snapshot_delete then finds an already-freed delta. */
PHP_FUNCTION(zealphp_coroutine_globals_request_end)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (!zealphp_coro_globals_hooks_active) return;
    if (!EG(symbol_table).nTableMask) return;
    if (os_get_cid) {
        zend_ulong cid = (zend_ulong) os_get_cid();
        void *ptr = zend_hash_index_find_ptr(&zealphp_coro_cid_to_ptr, cid);
        if (ptr) {
            /* Free this coroutine's pointer-keyed delta (drops its object refs). */
            zealphp_globals_snapshot_delete((zend_long)(uintptr_t)ptr);
            zend_hash_index_del(&zealphp_coro_cid_to_ptr, cid);
        }
    }
    /* Clear the live EG user globals to the parent baseline — drops the EG slot's
     * ref to any object global, firing __destruct HERE (coroutine context). */
    zealphp_globals_reset_to_parent();
}

/* ── define() interception ───────────────────────────────────────── */

/* Intercept define() to track per-request constants. The real define()
 * runs first; if it succeeds, we record the name so zealphp_constants_clear()
 * can remove it at request end. */
static ZEND_NAMED_FUNCTION(zealphp_define_intercept)
{
    /* Stage 3b — silent-define-redeclare. When silent_redeclare is on
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
    long cid = os_get_cid ? os_get_cid() : -1;

    if (cid < 0) {
        /* Sync / non-coroutine mode: no coroutine close to defer to, and no
         * cross-coroutine run_time_cache sharing — free immediately (unchanged). */
        ZEND_HASH_FOREACH_STR_KEY(&zealphp_request_constants, name) {
            if (name) {
                /* zend_hash_del on EG(zend_constants) removes AND frees the
                 * constant. Case-sensitive constants use the exact name as key. */
                zend_hash_del(EG(zend_constants), name);
            }
        } ZEND_HASH_FOREACH_END();
        zend_hash_clean(&zealphp_request_constants);
        return;
    }

    /* #9: coroutine mode — ORPHAN each request constant (remove from
     * EG(zend_constants) WITHOUT freeing) and park its pointer for deferral, so
     * the actual free happens at coroutine close (zealphp_on_close), after the
     * request's run_time_cache is gone. An immediate free here would dangle a
     * cached FETCH_CONSTANT slot (process-shared under opcache) that still points
     * at the struct → use-after-free. Mirrors zealphp_constants_snapshot_restore. */
    zval *slot = zend_hash_index_find(&zealphp_coro_constant_deferred, (zend_ulong)cid);
    if (!slot) {
        zval arr;
        array_init(&arr);
        slot = zend_hash_index_update(&zealphp_coro_constant_deferred, (zend_ulong)cid, &arr);
    }
    HashTable *deferred = Z_ARRVAL_P(slot);

    /* Suppress the destructor so zend_hash_del REMOVES the bucket without freeing
     * the zend_constant — i.e. orphan it, address intact (same trick as
     * zealphp_constants_snapshot_save). */
    dtor_func_t orig_dtor = EG(zend_constants)->pDestructor;
    EG(zend_constants)->pDestructor = NULL;
    ZEND_HASH_FOREACH_STR_KEY(&zealphp_request_constants, name) {
        if (name) {
            zend_constant *c = zend_hash_find_ptr(EG(zend_constants), name);
            if (c) {
                zend_hash_del(EG(zend_constants), name);   /* orphan (no free) */
                zval p;
                ZVAL_LONG(&p, (zend_long)(uintptr_t)c);
                zend_hash_next_index_insert(deferred, &p);
            }
        }
    } ZEND_HASH_FOREACH_END();
    EG(zend_constants)->pDestructor = orig_dtor;

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

/* ── exit()/die() interception (ext#47) ───────────────────────────── */

/* Throw ZealPHP\HaltException carrying the exit status. Returns true when
 * thrown, false when the class is not loaded and the caller must delegate
 * to the saved OpenSwoole/engine behaviour. The lookup deliberately does
 * NOT autoload (re-entering the autoloader from an exit site is unsafe) —
 * the framework class_exists()-loads HaltException before enabling the
 * hook. The `status` property is only written when the class declares it
 * (an older framework without the property still gets the throw; writing
 * an undeclared property would trip the dynamic-property deprecation). */
static bool zealphp_throw_halt(zend_string *message, zend_long status_long)
{
    zend_string *lc = zend_string_init(ZEND_STRL("zealphp\\haltexception"), 0);
    zend_class_entry *ce = zend_hash_find_ptr(EG(class_table), lc);
    zend_string_release(lc);
    if (!ce) {
        return false;
    }

    zend_object *obj = zend_throw_exception(ce,
        message ? ZSTR_VAL(message) : "zealphp exit", 0);
    if (obj && zend_hash_str_exists(&ce->properties_info, ZEND_STRL("status"))) {
        zval ex;
        ZVAL_OBJ(&ex, obj);
        if (message) {
            zend_update_property_str(ce, Z_OBJ(ex), ZEND_STRL("status"), message);
        } else {
            zend_update_property_long(ce, Z_OBJ(ex), ZEND_STRL("status"), status_long);
        }
    }
    return true;
}

#if PHP_VERSION_ID >= 80400
/* PHP 8.4+: exit()/die() is a real internal function (OpenSwoole already
 * replaced its handler at MINIT to throw ExitException in-coroutine /
 * in-server). We layer ABOVE that saved handler: HaltException when inside
 * a coroutine with the framework loaded, delegate otherwise. */
static ZEND_NAMED_FUNCTION(zealphp_exit_intercept)
{
    if (zealphp_exit_hook_active && os_get_cid && os_get_cid() > 0) {
        zend_string *message = NULL;
        zend_long status = 0;
        ZEND_PARSE_PARAMETERS_START(0, 1)
            Z_PARAM_OPTIONAL
            Z_PARAM_STR_OR_LONG(message, status)
        ZEND_PARSE_PARAMETERS_END();
        if (zealphp_throw_halt(message, status)) {
            return;
        }
    }
    if (zealphp_orig_exit_handler) {
        zealphp_orig_exit_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    }
}
#elif defined(ZEND_EXIT)
/* PHP < 8.4: exit compiles to the ZEND_EXIT opcode. Status extraction
 * mirrors OpenSwoole's coro_exit_handler; throw-then-DISPATCH is the same
 * proven shape (the VM's exception check unwinds before the opcode body). */
static int zealphp_exit_opcode_handler(zend_execute_data *execute_data)
{
    if (zealphp_exit_hook_active && os_get_cid && os_get_cid() > 0) {
        const zend_op *opline = EX(opline);
        zval *exit_status = NULL;
        if (opline->op1_type != IS_UNUSED) {
            if (opline->op1_type == IS_CONST) {
                exit_status = RT_CONSTANT(opline, opline->op1);
            } else {
                exit_status = EX_VAR(opline->op1.var);
            }
            if (exit_status && Z_ISREF_P(exit_status)) {
                exit_status = Z_REFVAL_P(exit_status);
            }
        }
        zend_string *msg = (exit_status && Z_TYPE_P(exit_status) == IS_STRING)
            ? Z_STR_P(exit_status) : NULL;
        zend_long stl = (exit_status && Z_TYPE_P(exit_status) == IS_LONG)
            ? Z_LVAL_P(exit_status) : 0;
        if (zealphp_throw_halt(msg, stl)) {
            return ZEND_USER_OPCODE_DISPATCH;
        }
    }
    if (zealphp_orig_exit_opcode_handler) {
        return zealphp_orig_exit_opcode_handler(execute_data);
    }
    return ZEND_USER_OPCODE_DISPATCH;
}
#endif

/* zealphp_exit_hook(bool): bool — toggle the exit()/die() interceptor.
 * Enable at boot AFTER OpenSwoole is loaded (the saved handler must be
 * OpenSwoole's) and AFTER class_exists(ZealPHP\HaltException::class).
 * On <8.4 disable is flag-gated (the opcode handler stays installed and
 * delegates) — swapping user opcode handlers back mid-run is not safe. */
PHP_FUNCTION(zealphp_exit_hook)
{
    bool enable;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enable)
    ZEND_PARSE_PARAMETERS_END();

#if PHP_VERSION_ID >= 80400
    if (enable && !zealphp_exit_hook_active) {
        zend_string *name = zend_string_init(ZEND_STRL("exit"), 0);
        zend_function *func = zend_hash_find_ptr(CG(function_table), name);
        if (func && func->type == ZEND_INTERNAL_FUNCTION
            && func->internal_function.handler != zealphp_exit_intercept) {
            zealphp_orig_exit_handler = func->internal_function.handler;
            func->internal_function.handler = zealphp_exit_intercept;
            zealphp_exit_hook_active = true;
        }
        zend_string_release(name);
    } else if (!enable && zealphp_exit_hook_active) {
        zend_string *name = zend_string_init(ZEND_STRL("exit"), 0);
        zend_function *func = zend_hash_find_ptr(CG(function_table), name);
        if (func && zealphp_orig_exit_handler
            && func->internal_function.handler == zealphp_exit_intercept) {
            func->internal_function.handler = zealphp_orig_exit_handler;
        }
        zealphp_orig_exit_handler = NULL;
        zealphp_exit_hook_active = false;
        zend_string_release(name);
    }
#elif defined(ZEND_EXIT)
    if (enable && !zealphp_exit_hook_active) {
        if (!os_get_cid) {
            /* dlsym'd in MINIT — NULL means OpenSwoole isn't loaded, so
             * there is no coroutine context to gate on (and no ExitException
             * behaviour to fix). Refuse cleanly. */
            php_error_docref(NULL, E_WARNING,
                "ext-zealphp: exit hook needs OpenSwoole's coroutine API");
            RETURN_FALSE;
        }
        if (!zealphp_exit_opcode_hooked) {
            zealphp_orig_exit_opcode_handler = zend_get_user_opcode_handler(ZEND_EXIT);
            zend_set_user_opcode_handler(ZEND_EXIT, zealphp_exit_opcode_handler);
            zealphp_exit_opcode_hooked = true;
        }
        zealphp_exit_hook_active = true;
    } else if (!enable) {
        zealphp_exit_hook_active = false;
    }
#else
    if (enable) {
        php_error_docref(NULL, E_WARNING,
            "ext-zealphp: exit hook is unsupported on this PHP build");
    }
#endif

    RETURN_BOOL(zealphp_exit_hook_active);
}

/* ── Module lifecycle ────────────────────────────────────────────── */

/* ── Stage 3a: silent-redeclare opcode hooks ───────────────────────────
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
 * We hook opcode 203 (ZEND_BIND_INIT_STATIC_OR_JMP) TOO. The earlier assumption
 * "the first call always reaches 183, so 183 alone is enough" is FALSE on PHP
 * 8.4+: an INITIALIZED static (`static $x = <expr>;`, e.g. `static $s = null;`)
 * compiles to 203, which initializes the live table AND jumps past 183 on the
 * very first call — so 183 never fires and the function was never registered →
 * never isolated. That was the 8.3-passes / 8.4-leaks regression in the
 * function-static trust-bar contract. This same handler is installed for both
 * opcodes; the registration is identical and the chaining is opcode-aware (see
 * the tail of this function). The activation-time seed walk still covers
 * anything bound before the hooks went live.
 *
 * Chain-aware: if another extension (e.g. uopz's uopz_set_static) already
 * installed a BIND_STATIC / BIND_INIT_STATIC_OR_JMP handler, we invoke it rather
 * than clobber it. */
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
    /* Opcode-aware chaining: this one handler is installed for BOTH
     * ZEND_BIND_STATIC (183) and ZEND_BIND_INIT_STATIC_OR_JMP (203); each opcode
     * has its OWN previous handler (another extension may have hooked one but not
     * the other), so chain to the correct one or fall through to DISPATCH. */
#ifdef ZEND_BIND_INIT_STATIC_OR_JMP
    if (EX(opline)->opcode == ZEND_BIND_INIT_STATIC_OR_JMP) {
        if (zealphp_prev_bind_init_static) {
            return zealphp_prev_bind_init_static(execute_data);
        }
        return ZEND_USER_OPCODE_DISPATCH;
    }
#endif
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
 * Combined with Stage 3a (silent function/class redeclare) and Stage 3b
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
    /* #13: a CV/VAR operand can be IS_REFERENCE (`$ref = 'f.php'; require_once $ref;`).
     * Deref before the IS_STRING gate — otherwise the reference fails the gate,
     * the file is dispatched to the standard cached no-op, and Stage-7 never
     * re-executes it (its per-request init code silently runs once). */
    if (inc_filename) {
        ZVAL_DEREF(inc_filename);
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
    /* #11: run the re-include guard in BOTH coroutine and sync (non-OpenSwoole)
     * modes. The eviction below recurses unbounded on a self-/circular
     * require_once if unguarded; the guard used to be gated on os_get_cid, so
     * sync mode (os_get_cid == NULL) got the eviction with NO protection → a
     * file that require_once's itself recursed until the heap exhausted (OOM).
     * Coroutine buckets stay keyed by cid (cleared on coroutine close); sync mode
     * uses a single bucket (key 0). Include isolation otherwise needs the
     * coroutine scheduler, so a sync context is one logical request per process
     * (CLI / test), where key 0 gives correct once-per-request idempotency. */
    {
        zend_ulong reinc_key = os_get_cid ? (zend_ulong) os_get_cid() : 0;
        zval *seen = zend_hash_index_find(&zealphp_coro_reincluded, reinc_key);
        if (!seen) {
            zval z;
            array_init(&z);
            seen = zend_hash_index_update(&zealphp_coro_reincluded, reinc_key, &z);
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

/* ── Stage 3c: compile-time silent-redeclare via CG-table swap ──────────
 *
 * Top-level `function foo() {}` / `class Bar {}` at file scope are bound
 * to CG(function_table) / CG(class_table) at COMPILE time by
 * zend_register_top_func / zend_register_top_class — they never emit a
 * runtime ZEND_DECLARE_* opcode for Stage 3a to intercept.
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
 * our originals — first-wins, identical semantics to Stage 3c but applied
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
     * requests. Stage 3c's CG-table swap below already handles top-level
     * redeclaration correctly on EVERY compile, so the cache was only a
     * re-compile optimization — not worth a UAF. */
    /* Save real table pointers — restore on exit.
     *
     * HAZARD-2 fix: capture the real tables from EG, NOT CG. EG(function_table)/
     * EG(class_table) are the STABLE process-global tables — this Stage-4 design
     * swaps CG only, never EG. CG(class_table), by contrast, may CURRENTLY point at
     * a *different* coroutine's stack-local scratch: OpenSwoole does not per-
     * coroutine save/restore CG, so when another coroutine is suspended mid-compile
     * the global CG is left pointing at its scratch. Capturing `real` from that CG
     * makes both the restore (below) and the first-wins merge-back target a
     * DANGLING pointer once that coroutine's stack is freed → zend_compile_class_decl
     * adds into freed memory (heap-use-after-free) and zend_shutdown later frees it
     * (bad-free) — both ASAN-confirmed on PHP 8.4/8.5. EG is always the true table,
     * so capture+restore+merge through EG. */
    HashTable *real_cg_fn = EG(function_table);
    HashTable *real_cg_cl = EG(class_table);
    HashTable *real_eg_fn = EG(function_table);
    HashTable *real_eg_cl = EG(class_table);

    /* Scratch tables with NULL dtor — we manage entry lifecycle below. */
    HashTable scratch_fn, scratch_cl;
    zend_hash_init(&scratch_fn, 8, NULL, NULL, 0);
    zend_hash_init(&scratch_cl, 8, NULL, NULL, 0);

    /* Stage 3c: swap CG only. EG stays pointing at the real table so
     * internal function/class lookups during compile (Closure for type
     * hints, attribute classes, parent classes for inheritance fixup)
     * still resolve.
     *
     * Stage 3c-v2 attempt — swapping EG(function_table) only, keeping
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

    /* COMPILE-ATOMICITY (critical coroutine-correctness invariant): this CG
     * swap must NOT be crossed by a coroutine switch. HOOK_FILE coroutinizes the
     * source-file read inside zend_compile_file below; if it yields while CG
     * points at our stack-local scratch and a zend_try frame is live, the switch
     * corrupts engine state (SIGSEGV under OPcache, lost-wakeup hang without it —
     * gdb-confirmed, 50-app sweep / phpMyAdmin Symfony-DI bootstrap). The fix
     * lives in the framework: App::run() drops HOOK_FILE whenever silentRedeclare
     * + enableCoroutine are both on, so the compile-time file read runs BLOCKING
     * and this window is atomic. (A per-compile enable/disable_hook toggle from
     * here was tried and is WORSE — mid-request wrapper swaps have side effects.)
     * Network/socket/sleep hooks stay on, so runtime coroutine concurrency is
     * unaffected; only file I/O is synchronous under the compile hook. */

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
    /* EG was never swapped (Stage 3c CG-only design) — keep these
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
            /* Loser: first declaration already in real. Free the dup so we
             * don't leak its op_array body — BUT NEVER free an immutable
             * (opcache SHM) op_array. Under OPcache, a cache hit binds the
             * SHM op_array into CG (our scratch); if it loses the first-wins
             * race, destroy_zend_function() would free shared memory the whole
             * process (and future requests) still execute → use-after-free in
             * execute_ex, a worker SIGSEGV that surfaces a request or two later
             * (50-app sweep: phpMyAdmin period-3 200/500/CRASH, opcache-on only;
             * gone with opcache off). Immutable losers are owned by OPcache —
             * just drop our scratch reference and let it be. */
            zend_function *lf = (zend_function *)ptr;
            if (!(lf->common.fn_flags & ZEND_ACC_IMMUTABLE)
                && !(EG(in_autoload) && zend_hash_num_elements(EG(in_autoload)) > 0)) {
                destroy_zend_function(lf);
            }
        }
    } ZEND_HASH_FOREACH_END();

    ZEND_HASH_FOREACH_STR_KEY_PTR(&scratch_cl, key, ptr) {
        if (key && !zend_hash_exists(real_cg_cl, key)) {
            zend_hash_add_ptr(real_cg_cl, key, ptr);
        } else if (ptr) {
            /* Same immutable-SHM guard as functions above: never destroy an
             * OPcache-owned (immutable) class entry.
             *
             * HAZARD-2 fix (Valgrind-confirmed UAF): ALSO never destroy a loser
             * while we are inside an autoload. zend_lookup_class_ex returns the
             * class THIS compile just produced (our scratch dup), and the caller
             * (`new` -> object_properties_init) is about to use it. If a CONCURRENT
             * coroutine already registered the winner for this key, destroying the
             * dup here frees its inheritance-allocated default_properties_table out
             * from under the live engine -> "Invalid read of size 8 in
             * _object_properties_init" (Valgrind: freed by destroy_zend_class <-
             * compile_file_hook, used by php_*_create_object). ASAN can't see it
             * (Zend-MM efree). Orphan the dup instead -- it then LEAKS until the
             * worker recycles (NOT reclaimed at request-end; #12 +
             * docs/issue-12-oparray-cache-design.md) rather than corrupt. Non-autoload
             * re-includes resolve the class by name to the winner at runtime, so
             * their losers remain safe to destroy — EXCEPT inherited ones (below). */
            zend_class_entry *lce = (zend_class_entry *)ptr;
            /* Stage-7 re-execution crash fix (v0.3.24, 2026-05-30): the "non-autoload
             * losers are safe to destroy" assumption above is FALSE for a loser class
             * WITH A PARENT / interfaces / traits. Stage 3c swaps CG only, never EG, so
             * when this compile linked the loser it resolved parent/interfaces against
             * the LIVE WINNER hierarchy in EG(class_table). The loser's inherited
             * method / property-info / default-property slots therefore reference
             * winner-owned structures; destroy_zend_class() on it frees/decrefs them
             * out from under the live winner -> the winner's default_properties_table
             * is corrupted -> SEGV in _object_properties_init (WRITE to a near-null
             * counted pointer in zend_gc_addref) on the next `new`. THIS is the crash
             * that takes down WordPress and any require_once-bootstrap inherited class
             * under coroutine-legacy (ASAN-pinned PHP 8.4: 300 `class X extends Y`
             * re-executed via Stage 7 crash -> clean with this guard; 300 FLAT classes
             * unaffected either way; 33/33 phpt green). The orphaned INHERITED loser
             * is NOT reclaimed: it leaks ~4-7 KB per re-exec until the worker recycles
             * (#12 -- proven unfixable in-place across 5 ASAN iterations; the loser is
             * always early-bound against the live winner via EG, so any free corrupts
             * the winner. Real fix scoped in docs/issue-12-oparray-cache-design.md).
             * Bounded by max_requests, NOT by request boundary. A FLAT loser (no parent/
             * iface/trait) is self-contained, so it stays safe to destroy. */
            bool zealphp_inherited_loser =
                (lce->parent != NULL) || (lce->parent_name != NULL)
                || lce->num_interfaces > 0 || lce->num_traits > 0;
            if (!(lce->ce_flags & ZEND_ACC_IMMUTABLE)
                && !(EG(in_autoload) && zend_hash_num_elements(EG(in_autoload)) > 0)
                && !zealphp_inherited_loser) {
                zval cl_zv;
                ZVAL_PTR(&cl_zv, lce);
                destroy_zend_class(&cl_zv);
            }
        }
    } ZEND_HASH_FOREACH_END();

    /* Stage 6.2 cache-save REMOVED — see the cache-hit note above: a cached
     * op_array's refcount pointer dangles once the engine frees it, segfaulting
     * the next compile. Stage 3c re-compiles + first-wins-merges every time,
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
         * coroutine mode — exactly the redeclare crash Stage 3a/4 close for
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_cwd_isolation, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, on, _IS_BOOL, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_locale_isolation, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, on, _IS_BOOL, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_timezone_isolation, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, on, _IS_BOOL, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_mbenc_isolation, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, on, _IS_BOOL, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_libxml_isolation, 0, 0, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, on, _IS_BOOL, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_umask_isolation, 0, 0, _IS_BOOL, 0)
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

/* Per-coroutine CWD isolation knob (framework #323). No-arg call returns the
 * current state; bool arg sets it and returns the PREVIOUS state. Enabling
 * captures the CURRENT process cwd as the worker baseline — call it from
 * worker start (after any boot-time chdir), not mid-request. If the baseline
 * can't be read the stage stays off (fail-closed: never chdir blindly). */
PHP_FUNCTION(zealphp_cwd_isolation)
{
    zend_bool on = 1;
    zend_bool on_is_null = 1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(on, on_is_null)
    ZEND_PARSE_PARAMETERS_END();

    bool prev = zealphp_cwd_isolation_active;
    if (!on_is_null) {
        if ((bool)on && !zealphp_cwd_isolation_active) {
            /* The stage rides the shared scheduler wrappers — install them if
             * no other stage has yet (same guard as the superglobal/globals/
             * statics activations). */
            if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
                php_error_docref(NULL, E_WARNING,
                    "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
                    "Per-coroutine CWD isolation requires OpenSwoole.");
                RETURN_BOOL(prev);
            }
            if (!zealphp_coro_wrappers_installed && !zealphp_install_coro_hooks()) {
                RETURN_BOOL(prev);
            }
            if (!VCWD_GETCWD(zealphp_cwd_baseline, sizeof(zealphp_cwd_baseline))) {
                zealphp_cwd_baseline[0] = '\0';
                RETURN_BOOL(prev); /* no baseline → stay off */
            }
        }
        zealphp_cwd_isolation_active = (bool)on;
        if (!(bool)on) {
            zend_hash_clean(&zealphp_coro_cwd_snapshots);
        }
    }
    RETURN_BOOL(prev);
}

/* Per-coroutine setlocale() isolation knob — same get/set + baseline-at-enable
 * semantics as zealphp_cwd_isolation(): enabling captures the CURRENT process
 * locale as the worker baseline (call from boot, after any boot-time
 * setlocale()), so a locale set BEFORE enabling is respected as home. */
PHP_FUNCTION(zealphp_locale_isolation)
{
    zend_bool on = 1;
    zend_bool on_is_null = 1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(on, on_is_null)
    ZEND_PARSE_PARAMETERS_END();

    bool prev = zealphp_locale_isolation_active;
    if (!on_is_null) {
        if ((bool)on && !zealphp_locale_isolation_active) {
            if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
                php_error_docref(NULL, E_WARNING,
                    "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
                    "Per-coroutine locale isolation requires OpenSwoole.");
                RETURN_BOOL(prev);
            }
            if (!zealphp_coro_wrappers_installed && !zealphp_install_coro_hooks()) {
                RETURN_BOOL(prev);
            }
            const char *zealphp_loc_cur = setlocale(LC_ALL, NULL);
            if (!zealphp_loc_cur) {
                RETURN_BOOL(prev); /* no baseline -> stay off (fail-closed) */
            }
            if (zealphp_locale_baseline) free(zealphp_locale_baseline);
            zealphp_locale_baseline = strdup(zealphp_loc_cur);
            if (!zealphp_locale_baseline) {
                RETURN_BOOL(prev);
            }
        }
        zealphp_locale_isolation_active = (bool)on;
        if (!(bool)on) {
            zend_hash_clean(&zealphp_coro_locale_snapshots);
        }
    }
    RETURN_BOOL(prev);
}

/* Per-coroutine umask() isolation knob — baseline captured at enable time
 * (umask(0)+restore: the only read API writes, so a read is two back-to-back
 * syscalls with no yield in between). */
PHP_FUNCTION(zealphp_umask_isolation)
{
    zend_bool on = 1;
    zend_bool on_is_null = 1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(on, on_is_null)
    ZEND_PARSE_PARAMETERS_END();

    bool prev = zealphp_umask_isolation_active;
    if (!on_is_null) {
        if ((bool)on && !zealphp_umask_isolation_active) {
            if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
                php_error_docref(NULL, E_WARNING,
                    "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
                    "Per-coroutine umask isolation requires OpenSwoole.");
                RETURN_BOOL(prev);
            }
            if (!zealphp_coro_wrappers_installed && !zealphp_install_coro_hooks()) {
                RETURN_BOOL(prev);
            }
            mode_t zealphp_um_b = umask(0);
            umask(zealphp_um_b);
            zealphp_umask_baseline = zealphp_um_b;
            zealphp_umask_baseline_set = true;
        }
        zealphp_umask_isolation_active = (bool)on;
        if (!(bool)on) {
            zend_hash_clean(&zealphp_coro_umask_snapshots);
        }
    }
    RETURN_BOOL(prev);
}

/* Per-coroutine date_default_timezone_set() isolation knob — baseline is the
 * timezone at enable time. See the PHP-setting stage block for rationale
 * (measured 179/250 cross-request tz leaks at 49-way concurrency). */
PHP_FUNCTION(zealphp_timezone_isolation)
{
    zend_bool on = 1;
    zend_bool on_is_null = 1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(on, on_is_null)
    ZEND_PARSE_PARAMETERS_END();

    bool prev = (bool)zealphp_tz_isolation_active;
    if (!on_is_null) {
        if ((bool)on && !zealphp_tz_isolation_active) {
            if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
                php_error_docref(NULL, E_WARNING,
                    "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
                    "Per-coroutine timezone isolation requires OpenSwoole.");
                RETURN_BOOL(prev);
            }
            if (!zealphp_coro_wrappers_installed && !zealphp_install_coro_hooks()) {
                RETURN_BOOL(prev);
            }
            char *zealphp_tz_cur = zealphp_call_string_getter("date_default_timezone_get");
            if (!zealphp_tz_cur) {
                RETURN_BOOL(prev); /* no baseline -> stay off (fail-closed) */
            }
            if (zealphp_tz_baseline) free(zealphp_tz_baseline);
            zealphp_tz_baseline = strdup(zealphp_tz_cur);
            efree(zealphp_tz_cur);
            if (!zealphp_tz_baseline) {
                RETURN_BOOL(prev);
            }
        }
        zealphp_tz_isolation_active = (bool)on;
        if (!(bool)on) {
            zend_hash_clean(&zealphp_coro_tz_snapshots);
        }
    }
    RETURN_BOOL(prev);
}

/* Per-coroutine mb_internal_encoding() isolation knob — baseline is the
 * encoding at enable time. Auto-refuses when mbstring isn't loaded (the
 * getter lookup fails → no baseline → stays off). Measured 173/250
 * cross-request encoding leaks at 49-way concurrency. */
PHP_FUNCTION(zealphp_mbenc_isolation)
{
    zend_bool on = 1;
    zend_bool on_is_null = 1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(on, on_is_null)
    ZEND_PARSE_PARAMETERS_END();

    bool prev = (bool)zealphp_mbenc_isolation_active;
    if (!on_is_null) {
        if ((bool)on && !zealphp_mbenc_isolation_active) {
            if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
                php_error_docref(NULL, E_WARNING,
                    "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
                    "Per-coroutine mb-encoding isolation requires OpenSwoole.");
                RETURN_BOOL(prev);
            }
            if (!zealphp_coro_wrappers_installed && !zealphp_install_coro_hooks()) {
                RETURN_BOOL(prev);
            }
            char *zealphp_mb_cur = zealphp_call_string_getter("mb_internal_encoding");
            if (!zealphp_mb_cur) {
                RETURN_BOOL(prev); /* mbstring absent / no baseline -> stay off */
            }
            if (zealphp_mbenc_baseline) free(zealphp_mbenc_baseline);
            zealphp_mbenc_baseline = strdup(zealphp_mb_cur);
            efree(zealphp_mb_cur);
            if (!zealphp_mbenc_baseline) {
                RETURN_BOOL(prev);
            }
        }
        zealphp_mbenc_isolation_active = (bool)on;
        if (!(bool)on) {
            zend_hash_clean(&zealphp_coro_mbenc_snapshots);
        }
    }
    RETURN_BOOL(prev);
}

/* Per-coroutine libxml_use_internal_errors() isolation knob — swaps the
 * LIBXML(error_list) pointer per coroutine (state + collected errors).
 * Resolves the libxml module globals via dlsym; refuses (stays off) when
 * libxml isn't loaded. */
PHP_FUNCTION(zealphp_libxml_isolation)
{
    zend_bool on = 1;
    zend_bool on_is_null = 1;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_BOOL_OR_NULL(on, on_is_null)
    ZEND_PARSE_PARAMETERS_END();

    bool prev = (bool)zealphp_libxml_isolation_active;
    if (!on_is_null) {
        if ((bool)on && !zealphp_libxml_isolation_active) {
            if (!os_set_on_yield || !os_set_on_resume || !os_set_on_close || !os_get_cid) {
                php_error_docref(NULL, E_WARNING,
                    "ext-zealphp: OpenSwoole coroutine scheduler hooks not found. "
                    "Per-coroutine libxml isolation requires OpenSwoole.");
                RETURN_BOOL(prev);
            }
            if (!zealphp_coro_wrappers_installed && !zealphp_install_coro_hooks()) {
                RETURN_BOOL(prev);
            }
            {
                int zp_cur = zealphp_libxml_flag_get();
                if (zp_cur < 0) {
                    RETURN_BOOL(prev); /* libxml absent -> stay off (fail-closed) */
                }
                zealphp_libxml_baseline = zp_cur;
            }
        }
        zealphp_libxml_isolation_active = (bool)on;
        if (!(bool)on) {
            zend_hash_clean(&zealphp_coro_libxml_snapshots);
        }
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
    /* Clear the current request's force-re-included set. Mirror the #11 keying:
     * coroutine bucket via os_get_cid(), sync (non-OpenSwoole) bucket via key 0,
     * so the sync re-include guard is reset at the request boundary too (without
     * this, sync mode would re-include a file at most once per worker lifetime). */
    zend_hash_index_del(&zealphp_coro_reincluded, (zend_ulong)(os_get_cid ? os_get_cid() : 0));
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
    zend_hash_init(&zealphp_coro_sg_owner_cids, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_request_coro_cids, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_request_coro_ptrs, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_adopted_coro_cids, 64, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_constant_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_constant_deferred,  64,  NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_ini_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_cwd_snapshots, 64, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_locale_snapshots, 64, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_tz_snapshots, 64, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_mbenc_snapshots, 64, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_libxml_snapshots, 64, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_umask_snapshots, 64, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_static_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_fn_static_snapshots, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_reincluded, 256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_cg_swap_fn, 64, NULL, NULL, 0);
    zend_hash_init(&zealphp_coro_cg_swap_cl, 64, NULL, NULL, 0);
    zend_hash_init(&zealphp_coro_in_autoload, 64, NULL, ZVAL_PTR_DTOR, 0);
    /* Registry values are borrowed zend_op_array* — we don't own them, no dtor. */
    zend_hash_init(&zealphp_fn_static_registry, 256, NULL, NULL, 0);
    zend_hash_init(&zealphp_coro_globals_deltas,     256, NULL, ZVAL_PTR_DTOR, 0);
    zend_hash_init(&zealphp_coro_globals_tombstones, 256, NULL, ZVAL_PTR_DTOR, 0);
    /* Stage-8 object-global registry: cid → array(key → object). ZVAL_PTR_DTOR
     * releases held object refs when a coroutine's entry is deleted. */
    zend_hash_init(&zealphp_coro_indirect_objs,      256, NULL, ZVAL_PTR_DTOR, 0);
    /* cid→Coroutine* bridge (raw pointers, NULL dtor — the deltas own the zvals). */
    zend_hash_init(&zealphp_coro_cid_to_ptr, 64, NULL, NULL, 0);
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
        /* PHPCoroutine::set_hook_flags(uint32_t) — for compile-atomicity. Both
         * the openswoole and swoole mangled names; NULL if unavailable (then
         * compile-atomic is a no-op and the user must drop HOOK_FILE manually). */
        dlclose(handle);
    }

    /* Stage 3a: register silent-redeclare opcode handlers.
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
     * deferred to Stage 3c — see docs/architecture/state-isolation-reference.md.
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

    /* Stage 5: function-static touched-set hooks. MUST be installed in MINIT,
     * NOT at runtime activation. PHP binds each opline->handler at COMPILE time
     * from zend_user_opcodes[], so a handler installed after user code is already
     * compiled NEVER fires for that code. The fixture's tb_static (and ALL
     * app-level functions compiled before App::run) were silently missed when
     * this was installed at runtime — they never registered, so their
     * function-statics were never isolated (8.3 happened to pass; 8.4 leaked
     * because initialized statics route through opcode 203). Installing here, in
     * MINIT, before any user code compiles, fixes the timing. The handler itself
     * is gated by zealphp_fn_statics_active, so it is a cheap flag-check + chain
     * for apps that don't use Stage 5 — same zero-overhead-when-off contract as
     * the DECLARE/INCLUDE handlers above. Both BIND opcodes (183 and 8.4's 203)
     * are hooked; the handler chains opcode-aware. */
    zealphp_prev_bind_static = zend_get_user_opcode_handler(ZEND_BIND_STATIC);
    zend_set_user_opcode_handler(ZEND_BIND_STATIC,           zealphp_bind_static_handler);
#ifdef ZEND_BIND_INIT_STATIC_OR_JMP
    zealphp_prev_bind_init_static = zend_get_user_opcode_handler(ZEND_BIND_INIT_STATIC_OR_JMP);
    zend_set_user_opcode_handler(ZEND_BIND_INIT_STATIC_OR_JMP, zealphp_bind_static_handler);
#endif
    zealphp_bind_static_installed = true;

    /* Stage 7: smart require_once. Same zero-overhead-when-off pattern as
     * Stage 3a — the handler checks zealphp_include_isolation_enabled and
     * chains/DISPATCHes immediately when disabled. */
    zealphp_prev_include_eval = zend_get_user_opcode_handler(ZEND_INCLUDE_OR_EVAL);
    zend_set_user_opcode_handler(ZEND_INCLUDE_OR_EVAL,        zealphp_include_eval_handler);

    /* Stage 3c: compile-time silent-redeclare via CG-table swap.
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_request_input_set, 0, 6, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, get, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, post, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, cookie, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, server, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, files, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, request, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_owner, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_superglobals_adopt, 0, 0, _IS_BOOL, 0)
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_coroutine_globals_request_end, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_globals_baseline_refresh, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_process_state_snapshot, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_process_state_clean, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_define_hook, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_exit_hook, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, enable, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_protect_classes, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, names, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* ── Per-request run_time_cache reset (coroutine-legacy) ──────────────────────
 *
 * Persisted user functions/methods (kept across requests by silent-redeclare)
 * cache resolved pointers in their op_array run_time_cache: constants
 * (ZEND_FETCH_CONSTANT), called functions (ZEND_INIT_FCALL*), global-var slots
 * (ZEND_BIND_GLOBAL), classes/methods/properties, etc. The per-request isolation
 * + re-declaration layers free or MOVE those targets — defineIsolation frees and
 * recreates constants, coroutineGlobalsIsolation churns $GLOBALS, Stage-7 re-
 * execution re-declares symbols — so a cached pointer dangles on the next request
 * and reads a STALE resolution. Symptoms across cache types: a legacy bootstrap's
 * `define('MB_IN_BYTES', 1024 * KB_IN_BYTES)` throws "Unsupported operand types:
 * string * int" on the 2nd+ request (constant cache) while constant('KB_IN_BYTES')
 * (uncached) is correct; WP-style `global $wpdb` methods SEGV in ZEND_BIND_GLOBAL
 * (global-var cache); ZEND_INIT_FCALL_BY_NAME SEGVs (call cache); etc.
 *
 * zealphp_reset_request_rtcaches() CLEARS (memset 0) each per-request op_array's
 * run_time_cache IN PLACE, so every slot goes cold and RE-RESOLVES against the
 * live tables on next use. The framework invokes it once per request from
 * CoSessionManager/SessionManager.
 *
 * Why memset-in-place rather than nulling the map_ptr: nulling forces a re-init/
 * re-alloc whose timing left EX(run_time_cache) pointing at freed/volatile memory
 * on some entry paths -> SEGV (zend_fetch_ce_from_cache_slot / ZEND_BIND_GLOBAL).
 * Clearing keeps the map_ptr AND the cache memory intact — the CG(arena) block is
 * never freed mid-worker-life — and only zeroes the cached resolutions. A zero
 * slot is the cold path the engine already handles, so this is concurrency-safe
 * (a coroutine reading a slot we just cleared simply re-resolves) and covers
 * functions AND methods uniformly. In non-opcache the run_time_cache map_ptr is a
 * direct pointer (NULL until the first call, then the arena cache), so GET is
 * always safe and never out-of-bounds.
 *
 * Boot/snapshot symbols are skipped purely as an OPTIMISATION (their cached
 * targets are process-stable and never go stale); without a snapshot every user
 * symbol is cleared, which is still safe. */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_reset_request_rtcaches, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(zealphp_reset_request_rtcaches)
{
    ZEND_PARSE_PARAMETERS_NONE();
    zend_long count = 0;
    bool snap = zealphp_state_snapshotted;

    zend_string *fname;
    zend_function *fn;
    ZEND_HASH_FOREACH_STR_KEY_PTR(EG(function_table), fname, fn) {
        if (fname && fn && fn->type == ZEND_USER_FUNCTION
            && (!snap || !zend_hash_exists(&zealphp_snapshot_functions, fname))) {
            void *rtc = ZEND_MAP_PTR_GET(fn->op_array.run_time_cache);
            if (rtc) {
                memset(rtc, 0, fn->op_array.cache_size);
                count++;
            }
        }
    } ZEND_HASH_FOREACH_END();

    zend_string *cname;
    zend_class_entry *ce;
    ZEND_HASH_FOREACH_STR_KEY_PTR(EG(class_table), cname, ce) {
        if (cname && ce && ce->type == ZEND_USER_CLASS
            && (!snap || !zend_hash_exists(&zealphp_snapshot_classes, cname))) {
            zend_function *m;
            ZEND_HASH_FOREACH_PTR(&ce->function_table, m) {
                if (m && m->type == ZEND_USER_FUNCTION) {
                    void *rtc = ZEND_MAP_PTR_GET(m->op_array.run_time_cache);
                    if (rtc) {
                        memset(rtc, 0, m->op_array.cache_size);
                        count++;
                    }
                }
            } ZEND_HASH_FOREACH_END();
        }
    } ZEND_HASH_FOREACH_END();
    RETURN_LONG(count);
}

/* ── zealphp_reset_request_statics(): int ────────────────────────────────
 *
 * Per-request FUNCTION-STATIC reset — the PHP-FPM "fresh process per request"
 * contract for coroutine-legacy. PHP's shutdown_executor() (Zend/zend_execute_API.c)
 * destroys every user function/method's live static_variables table at request
 * shutdown, so `static $x = INIT;` re-initialises to INIT on the NEXT request. In
 * a long-lived OpenSwoole worker that per-request shutdown never runs, so a
 * function-local static keeps its last value ACROSS requests.
 *
 * The canonical failure: WordPress's wp_start_object_cache() has
 * `static $first_init = true;` and sets it false at the end of request 1. On
 * request 2 the stale `false` makes WP take the "already initialised" branch
 * (wp_cache_switch_to_blog) instead of calling wp_cache_init(), so $wp_object_cache
 * is never created -> "Call to a member function switch_to_blog() on null" -> 500,
 * then a worker crash on request 3 building on that half-initialised state. ANY
 * legacy "init-once" guard (`static $done = false;`) has the same failure mode —
 * this is general, not WP-specific.
 *
 * Mirrors shutdown_executor() EXACTLY: zend_array_destroy(live) +
 * ZEND_MAP_PTR_SET(static_variables_ptr, NULL) for each instantiated per-request
 * user function and class method, so the next ZEND_BIND_STATIC re-dups the
 * immutable template (op_array->static_variables) and the initial values are
 * restored. The `live != static_variables` guard never destroys the shared
 * template itself (matches the Stage-5 walk).
 *
 * Crash-safe under coroutine concurrency: a static bound to a still-suspended
 * frame's CV is an IS_REFERENCE shared (refcount >= 2) between the table slot and
 * that CV. zend_array_destroy drops only the table's reference, so the
 * zend_reference (and the live CV) survive; nulling the map_ptr only makes the
 * NEXT call re-dup the template. Boot/snapshot symbols are skipped so per-worker
 * framework statics (e.g. one-time handler registration in src/) persist as
 * intended. Gated by the framework on silent_redeclare (the legacy-persistent
 * marker) and by the ZEALPHP_FN_STATICS_RESET_DISABLE env kill-switch. */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_reset_request_statics, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(zealphp_reset_request_statics)
{
    ZEND_PARSE_PARAMETERS_NONE();

    /* env kill-switch — read once per worker. */
    static int disabled = -1;
    if (disabled < 0) {
        const char *e = getenv("ZEALPHP_FN_STATICS_RESET_DISABLE");
        disabled = (e && *e && *e != '0') ? 1 : 0;
    }
    if (disabled) {
        RETURN_LONG(0);
    }

    zend_long count = 0;
    bool snap = zealphp_state_snapshotted;

    /* Global user functions. */
    zend_string *fname;
    zend_function *fn;
    ZEND_HASH_FOREACH_STR_KEY_PTR(EG(function_table), fname, fn) {
        if (fname && fn && fn->type == ZEND_USER_FUNCTION
            && (!snap || !zend_hash_exists(&zealphp_snapshot_functions, fname))) {
            zend_op_array *opa = &fn->op_array;
            if (!opa->static_variables) continue;
            if (!ZEND_MAP_PTR(opa->static_variables_ptr)) continue;
            HashTable *live = ZEND_MAP_PTR_GET(opa->static_variables_ptr);
            if (live && live != opa->static_variables) {
                zend_array_destroy(live);
                ZEND_MAP_PTR_SET(opa->static_variables_ptr, NULL);
                count++;
            }
        }
    } ZEND_HASH_FOREACH_END();

    /* Methods of user classes. No ZEND_HAS_STATIC_IN_METHODS gate — the per-method
     * static_variables NULL check is a cheap filter and skipping the flag matches
     * the Stage-5 walk, so a re-compiled class can't slip a method static past us. */
    zend_string *cname;
    zend_class_entry *ce;
    ZEND_HASH_FOREACH_STR_KEY_PTR(EG(class_table), cname, ce) {
        if (cname && ce && ce->type == ZEND_USER_CLASS
            && (!snap || !zend_hash_exists(&zealphp_snapshot_classes, cname))) {
            zend_function *m;
            ZEND_HASH_FOREACH_PTR(&ce->function_table, m) {
                if (m && m->type == ZEND_USER_FUNCTION) {
                    zend_op_array *opa = &m->op_array;
                    if (!opa->static_variables) continue;
                    if (!ZEND_MAP_PTR(opa->static_variables_ptr)) continue;
                    HashTable *live = ZEND_MAP_PTR_GET(opa->static_variables_ptr);
                    if (live && live != opa->static_variables) {
                        zend_array_destroy(live);
                        ZEND_MAP_PTR_SET(opa->static_variables_ptr, NULL);
                        count++;
                    }
                }
            } ZEND_HASH_FOREACH_END();
        }
    } ZEND_HASH_FOREACH_END();

    RETURN_LONG(count);
}

/* ── zealphp_reset_request_class_statics(): int ──────────────────────────
 *
 * Per-request CLASS-STATIC-PROPERTY reset — the class-property analog of
 * zealphp_reset_request_statics(). PHP's shutdown_executor() also resets static
 * PROPERTIES per request, via zend_cleanup_internal_class_data() for every user
 * class with static members (Zend/zend_execute_API.c). A long-lived OpenSwoole
 * worker never runs that, so a class static property keeps its value ACROSS
 * requests — and the per-coroutine class-static isolation deliberately leaves
 * OBJECT/resource statics process-shared (scalars/arrays only), so an object
 * static (a DI container, a connection registry) persists across requests.
 *
 * Canonical break: Drupal's static service container + `Database` connection
 * registry (class static properties). Request 1 builds them; on request 2 the
 * mix of "globals/scalars reset but the object container persisted" makes Drupal
 * throw "The specified database connection is not defined" -> 500.
 *
 * zend_cleanup_internal_class_data(ce) is ZEND_API (exported — unlike the static
 * free_zend_constant): it nulls ce->static_members_table's map_ptr and frees the
 * live table (correctly releasing typed-property reference sources); the next
 * static-property access re-initialises from ce->default_static_members_table via
 * zend_class_init_statics() (Zend/zend_object_handlers.c). Object static
 * __destructors run HERE, in the request coroutine's context (an I/O __destruct
 * may yield), same as the object-global drain. Boot/snapshot classes are skipped
 * so framework class statics (App::$routes, the middleware stack, Store/Counter
 * backends) persist for the worker's lifetime.
 *
 * COUPLING: freeing the live table invalidates cached ZEND_FETCH_STATIC_PROP slots
 * in run_time_cache (a later fetch would read a dangling slot -> SEGV), so this
 * MUST be paired with zealphp_reset_request_rtcaches(), which clears those slots.
 * The framework runs both every request under the same silent_redeclare gate, so a
 * cleared cache always re-resolves against the re-init'd table on the next access.
 *
 * Gated by the framework on silent_redeclare and by the
 * ZEALPHP_CLASS_STATICS_RESET_DISABLE env kill-switch. */
/* Reset a class's static properties IN PLACE — dtor each OWN static's live
 * value and re-copy its declared default from default_static_members_table,
 * WITHOUT freeing/reallocating the static_members_table array.
 *
 * Why in-place rather than zend_cleanup_internal_class_data (free + null map_ptr
 * + lazy re-init at a NEW address): a cached ZEND_FETCH_STATIC_PROP slot in some
 * op_array's run_time_cache holds a raw pointer INTO this table. The paired
 * zealphp_reset_request_rtcaches() clears those slots — but it deliberately
 * SKIPS snapshot functions/methods (treating their cached targets as
 * process-stable). A snapshot function that reads a NON-snapshot class's static
 * property has exactly such a slot, and freeing the table leaves it dangling ->
 * the next read is a use-after-free (stale value at best; a wild Z_STRLEN -> a
 * multi-TB alloc / SEGV under heap reuse at worst). Keeping the table at a
 * STABLE address makes every cached slot — snapshot or not — keep pointing at a
 * valid table holding the reset value, so the reset is self-sufficient and no
 * longer coupled to rtcache-clearing completeness.
 *
 * Value semantics mirror zend_class_init_statics (a plain ZVAL_COPY from the
 * template): scalars/arrays/typed-with-default reset to their default; a
 * no-default typed static resets to UNDEF (correct "uninitialised" PHP-FPM
 * parity — read-before-write throws, exactly as a fresh process would). Object
 * statics run their __destruct here in the request coroutine's context (an I/O
 * __destruct may yield), same as before. Inherited (IS_INDIRECT) slots are
 * skipped — they point at the parent's live slot, which the parent's own
 * in-place reset restores; the indirect pointer stays valid because the parent
 * table's address is likewise stable. */
static void zealphp_reset_class_statics_inplace(zend_class_entry *ce)
{
    zval *live = CE_STATIC_MEMBERS(ce);
    if (!live) {
        return;
    }
    int n = ce->default_static_members_count;
    for (int i = 0; i < n; i++) {
        zval *dst = &live[i];
        /* Inherited static: live slot indirects to the parent's slot. Leave it;
         * the parent's own reset owns the value, and the address is stable. */
        if (Z_TYPE_P(dst) == IS_INDIRECT) {
            continue;
        }
        zval *tmpl = &ce->default_static_members_table[i];
        if (Z_TYPE_P(tmpl) == IS_INDIRECT) {
            continue; /* defensive — template marks it inherited */
        }
        zval fresh;
        ZVAL_COPY(&fresh, tmpl);   /* default copy (refcount-correct; UNDEF ok) */
        zval old = *dst;
        ZVAL_COPY_VALUE(dst, &fresh);
        zval_ptr_dtor(&old);       /* release prior value (runs object __destruct) */
    }
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_reset_request_class_statics, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(zealphp_reset_request_class_statics)
{
    ZEND_PARSE_PARAMETERS_NONE();

    /* env kill-switch — read once per worker. */
    static int disabled = -1;
    if (disabled < 0) {
        const char *e = getenv("ZEALPHP_CLASS_STATICS_RESET_DISABLE");
        disabled = (e && *e && *e != '0') ? 1 : 0;
    }
    if (disabled) {
        RETURN_LONG(0);
    }

    zend_long count = 0;
    bool snap = zealphp_state_snapshotted;

    zend_string *cname;
    zend_class_entry *ce;
    ZEND_HASH_FOREACH_STR_KEY_PTR(EG(class_table), cname, ce) {
        if (cname && ce && ce->type == ZEND_USER_CLASS
            && ce->default_static_members_count > 0
            && (!snap || !zend_hash_exists(&zealphp_snapshot_classes, cname))) {
            /* Only touch classes whose live static table is actually instantiated.
             * Reset the values IN PLACE (keeping the table allocation + address)
             * rather than zend_cleanup_internal_class_data's free + lazy re-init,
             * so a cached ZEND_FETCH_STATIC_PROP slot (incl. a snapshot function's,
             * which the rtcache reset skips) can't dangle into freed memory. */
            if (ZEND_MAP_PTR(ce->static_members_table) && CE_STATIC_MEMBERS(ce)) {
                zealphp_reset_class_statics_inplace(ce);
                count++;
            }
        }
    } ZEND_HASH_FOREACH_END();

    RETURN_LONG(count);
}


/* -- Stage 8: true-global-scope request include ----------------------
 * Run a file's top-level code against EG(symbol_table) (real globals)
 * even when called from deep inside a coroutine call stack, so bare
 * file-scope vars ($menu/$submenu/$_wp_submenu_nopriv in WordPress) --
 * and every transitive require_once -- bind to $GLOBALS instead of the
 * caller's frame. Mirrors zend_execute()'s top-frame path but FORCES the
 * global symbol table. See docs/architecture/2026-06-02-stage8-*.
 */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_zealphp_require_global, 0, 1, IS_MIXED, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(zealphp_require_global)
{
    zend_string *path;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(path)
    ZEND_PARSE_PARAMETERS_END();

    if (EG(exception)) { RETURN_THROWS(); }

    zend_file_handle fh;
    zend_stream_init_filename(&fh, ZSTR_VAL(path));

    zend_op_array *op_array = zend_compile_file(&fh, ZEND_REQUIRE);
    zend_destroy_file_handle(&fh);

    if (!op_array) {
        /* #18: a parse error returns a NULL op_array AND sets an exception.
         * RETURN_FALSE while EG(exception) is live returns a value with a
         * pending exception (a latent ZEND_ASSERT in a debug build). Propagate
         * the exception instead of masking it with a false return. */
        if (EG(exception)) { RETURN_THROWS(); }
        RETURN_FALSE;
    }
    if (EG(exception)) {
        destroy_op_array(op_array);
        efree_size(op_array, sizeof(zend_op_array));
        RETURN_THROWS();
    }

    zend_execute_data *call = zend_vm_stack_push_call_frame(
        ZEND_CALL_TOP_CODE | ZEND_CALL_HAS_SYMBOL_TABLE,
        (zend_function *) op_array, 0, NULL);
    call->symbol_table = &EG(symbol_table);   /* FORCE global scope */

    zval result;
    ZVAL_UNDEF(&result);

    zend_init_code_execute_data(call, op_array, &result);

    /* #17: guard the execute against a zend_bailout (E_ERROR / OOM / exit())
     * inside the required file. A bare longjmp would skip the cleanup below and
     * leak the op_array + call frame. On bailout free the op_array we own and
     * re-raise; let the engine's bailout unwind reclaim the VM-stack frame
     * (manually freeing it against the bailout's stack state could double-free). */
    volatile int zealphp_bailed = 0;
    zend_try {
        zend_execute_ex(call);
    } zend_catch {
        zealphp_bailed = 1;
    } zend_end_try();

    if (zealphp_bailed) {
        /* ext#52 hardening: a bailout longjmps past every zend_leave_helper, so
         * NO frame on this coroutine detached its symbol table — any bucket
         * still IS_INDIRECT into this coroutine's VM stack dangles into memory
         * that dies with the coroutine, and the NEXT request's
         * zend_attach_symbol_table would move those recycled bytes into a live
         * frame (over-free on its first overwrite). Scrub: NULL every
         * EG(symbol_table) bucket whose INDIRECT target lies inside one of THIS
         * coroutine's VM-stack pages. Master-frame CVs live on another
         * context's stack and are untouched. The CV values leak — same
         * tradeoff php-src itself accepts for bailout. */
        if (EG(symbol_table).nTableMask) {
            zval *zuaf_b;
            ZEND_HASH_FOREACH_VAL(&EG(symbol_table), zuaf_b) {
                if (Z_TYPE_P(zuaf_b) != IS_INDIRECT) continue;
                const zval *zuaf_t = Z_INDIRECT_P(zuaf_b);
                for (zend_vm_stack zuaf_p = EG(vm_stack); zuaf_p; zuaf_p = zuaf_p->prev) {
                    if (zuaf_t >= (const zval *)zuaf_p && zuaf_t < zuaf_p->end) {
                        ZVAL_NULL(zuaf_b);
                        break;
                    }
                }
            } ZEND_HASH_FOREACH_END();
        }
        destroy_op_array(op_array);
        efree_size(op_array, sizeof(zend_op_array));
        zend_bailout();
    }

    zend_vm_stack_free_call_frame(call);

    destroy_op_array(op_array);
    efree_size(op_array, sizeof(zend_op_array));

    if (EG(exception)) {
        zval_ptr_dtor(&result);
        RETURN_THROWS();
    }
    if (Z_TYPE(result) != IS_UNDEF) {
        RETURN_COPY_VALUE(&result);
    }
    RETURN_NULL();
}

static const zend_function_entry zealphp_functions[] = {
    PHP_FE(zealphp_override,               arginfo_zealphp_override)
    PHP_FE(zealphp_restore,                arginfo_zealphp_restore)
    PHP_FE(zealphp_restore_all,            arginfo_zealphp_restore_all)
    PHP_FE(zealphp_superglobals_set,       arginfo_zealphp_superglobals_set)
    PHP_FE(zealphp_request_input_set,      arginfo_zealphp_request_input_set)
    PHP_FE(zealphp_superglobals_clear,     arginfo_zealphp_superglobals_clear)
    PHP_FE(zealphp_superglobals_owner,     arginfo_zealphp_superglobals_owner)
    PHP_FE(zealphp_superglobals_adopt,     arginfo_zealphp_superglobals_adopt)
    PHP_FE(zealphp_superglobals_save,      arginfo_zealphp_superglobals_save)
    PHP_FE(zealphp_superglobals_restore,   arginfo_zealphp_superglobals_restore)
    PHP_FE(zealphp_coroutine_superglobals, arginfo_zealphp_coroutine_superglobals)
    PHP_FE(zealphp_coroutine_globals,      arginfo_zealphp_coroutine_globals)
    PHP_FE(zealphp_coroutine_statics,      arginfo_zealphp_coroutine_statics)
    PHP_FE(zealphp_constants_clear,        arginfo_zealphp_constants_clear)
    PHP_FE(zealphp_ini_restore,           arginfo_zealphp_ini_restore)
    PHP_FE(zealphp_define_hook,            arginfo_zealphp_define_hook)
    PHP_FE(zealphp_exit_hook,              arginfo_zealphp_exit_hook)
    PHP_FE(zealphp_require_global,         arginfo_zealphp_require_global)
    PHP_FE(zealphp_globals_snapshot,       arginfo_zealphp_globals_snapshot)
    PHP_FE(zealphp_globals_clean,          arginfo_zealphp_globals_clean)
    PHP_FE(zealphp_coroutine_globals_request_end, arginfo_zealphp_coroutine_globals_request_end)
    PHP_FE(zealphp_globals_baseline_refresh, arginfo_zealphp_globals_baseline_refresh)
    PHP_FE(zealphp_process_state_snapshot, arginfo_zealphp_process_state_snapshot)
    PHP_FE(zealphp_process_state_clean,    arginfo_zealphp_process_state_clean)
    PHP_FE(zealphp_protect_classes,        arginfo_zealphp_protect_classes)
    PHP_FE(zealphp_silent_redeclare,       arginfo_zealphp_silent_redeclare)
    PHP_FE(zealphp_include_isolation,     arginfo_zealphp_include_isolation)
    PHP_FE(zealphp_include_isolation_reset, arginfo_zealphp_include_isolation_reset)
    PHP_FE(zealphp_cwd_isolation,         arginfo_zealphp_cwd_isolation)
    PHP_FE(zealphp_locale_isolation,      arginfo_zealphp_locale_isolation)
    PHP_FE(zealphp_timezone_isolation,    arginfo_zealphp_timezone_isolation)
    PHP_FE(zealphp_mbenc_isolation,       arginfo_zealphp_mbenc_isolation)
    PHP_FE(zealphp_libxml_isolation,      arginfo_zealphp_libxml_isolation)
    PHP_FE(zealphp_umask_isolation,       arginfo_zealphp_umask_isolation)
    PHP_FE(zealphp_reset_request_rtcaches, arginfo_zealphp_reset_request_rtcaches)
    PHP_FE(zealphp_reset_request_statics,  arginfo_zealphp_reset_request_statics)
    PHP_FE(zealphp_reset_request_class_statics, arginfo_zealphp_reset_request_class_statics)
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

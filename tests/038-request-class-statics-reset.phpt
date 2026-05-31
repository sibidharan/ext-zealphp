--TEST--
038: per-request class-static-property reset — re-initialises per-request CLASS statics (incl. object/DI-container statics) to their template, skips boot/snapshot classes (coroutine-legacy PHP-FPM "fresh process per request" contract)
--SKIPIF--
<?php if (!extension_loaded('zealphp')) die('skip Required extension missing: zealphp'); ?>
--FILE--
<?php
/* coroutine-legacy class-static-property reset.
 *
 * PHP's shutdown_executor() resets static PROPERTIES per request too (via
 * zend_cleanup_internal_class_data for every user class with static members), so
 * `class C { static $x = INIT; }` re-initialises to INIT next request. A
 * long-lived OpenSwoole worker never runs that, and the per-coroutine class-static
 * isolation deliberately leaves OBJECT statics process-shared, so a static DI
 * container / connection registry persists ACROSS requests. The canonical break is
 * Drupal's static service container: request 2 sees the persisted object container
 * out of sync with the reset globals -> "database connection is not defined" 500.
 *
 * zealphp_reset_request_class_statics() calls zend_cleanup_internal_class_data()
 * (ZEND_API): nulls the live static_members_table + frees it (object statics
 * __destruct here, in coroutine context), so the next static-property access
 * re-inits from the template via zend_class_init_statics(). Boot/snapshot classes
 * (process-stable, per-worker) are skipped. The framework calls it once per request.
 */

// Boot class: top-level declaration bound at COMPILE time -> in the snapshot ->
// per-worker -> its static property must be SKIPPED (keeps counting).
class BootC038 { public static int $hits = 0; }

zealphp_silent_redeclare(true);
zealphp_process_state_snapshot();   // boot baseline (captures BootC038)

// Per-request class declared at RUNTIME via require AFTER the snapshot -> reset
// each request. Models the Drupal/WP init-once guard + object-container static.
$tmp = sys_get_temp_dir() . '/zl038_' . getmypid() . '.php';
file_put_contents($tmp,
    "<?php\n"
    . "class App038 {\n"
    . "    public static bool \$inited = false;\n"
    . "    public static \$container = null;\n"
    . "    public static function boot() {\n"
    . "        if (!self::\$inited) { self::\$container = new stdClass(); self::\$container->ready = true; self::\$inited = true; }\n"
    . "        return (self::\$container && self::\$container->ready) ? 'READY' : 'MISSING';\n"
    . "    }\n"
    . "}\n"
);
require $tmp;

// Warm boot class + per-request class.
BootC038::$hits++; BootC038::$hits++;                 // 2
echo "boot1=", App038::boot(), "\n";                  // READY (inited false->true, container built)
// Simulate end-of-request stale state: guard stays set, container is gone.
App038::$inited = true; App038::$container = null;    // boot() WITHOUT reset would now return MISSING

$n = zealphp_reset_request_class_statics();
echo "reset_ge1=", ($n >= 1 ? "yes" : "no"), "\n";    // App038 reset; BootC038 skipped

// Freeing the static_members_table invalidates cached ZEND_FETCH_STATIC_PROP slots,
// so the framework pairs this with the run_time_cache reset every request (both run
// under the same silent_redeclare gate) to clear those stale slots. Mirror that
// pairing here before re-accessing — otherwise a same-request re-fetch reads a
// dangling slot and SEGVs.
zealphp_reset_request_rtcaches();

// Per-request class statics re-init to template (inited=false, container=null), so
// boot() rebuilds the container -> READY (the discriminator: MISSING without reset).
// Also proves resetting an OBJECT static (the stdClass container) is crash-free.
echo "boot2=", App038::boot(), "\n";                  // READY (fresh)
echo "inited_is_true=", (App038::$inited ? "yes" : "no"), "\n";   // yes (re-set by boot)
// Boot class static is NOT reset (process-stable, per-worker) -> hits persists.
echo "boothits=", BootC038::$hits, "\n";              // 2

$n2 = zealphp_reset_request_class_statics();          // idempotent + crash-free
echo "reset2_ge1=", ($n2 >= 1 ? "yes" : "no"), "\n";
@unlink($tmp);
echo "DONE\n";
?>
--EXPECT--
boot1=READY
reset_ge1=yes
boot2=READY
inited_is_true=yes
boothits=2
reset2_ge1=yes
DONE

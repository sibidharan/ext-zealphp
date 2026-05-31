--TEST--
036: per-request run_time_cache reset — clears per-request FUNCTIONS and METHODS, skips boot/snapshot symbols (coroutine-legacy staleness fix)
--SKIPIF--
<?php if (!extension_loaded('zealphp')) die('skip Required extension missing: zealphp'); ?>
--FILE--
<?php
/* coroutine-legacy run_time_cache staleness fix.
 *
 * Persisted user functions/methods (kept across requests by silent-redeclare)
 * cache resolved pointers in their op_array run_time_cache (constants, called
 * functions, global-var slots, classes/methods/props). Per-request isolation +
 * re-declaration free or move those targets, so a cached pointer dangles on the
 * next request -> stale resolution / SEGV (ZEND_FETCH_CONSTANT "string * int",
 * ZEND_BIND_GLOBAL, ZEND_INIT_FCALL_BY_NAME, ...).
 *
 * zealphp_reset_request_rtcaches() CLEARS (memset 0) each per-request op_array's
 * run_time_cache in place so every slot re-resolves cold on next use — covering
 * global functions AND class methods. Boot/snapshot symbols (process-stable
 * targets) are skipped. The framework calls it once per request.
 */

// Boot symbol: a top-level function is bound at COMPILE time -> in the snapshot
// taken below -> stable -> must be SKIPPED.
function boot_helper() { return strlen('boot'); }
boot_helper();

zealphp_silent_redeclare(true);
zealphp_process_state_snapshot();   // boot baseline (captures boot_helper)

// Per-request symbols declared at RUNTIME via require (the legacy bootstrap
// pattern) AFTER the snapshot -> NOT in the snapshot -> cleared. One global
// function and one class method (the WP-style `global $x` + constant case).
$tmp = sys_get_temp_dir() . '/zl036_' . getmypid() . '.php';
file_put_contents($tmp,
    "<?php\n"
    . "function app_fn() { return PHP_INT_SIZE; }\n"
    . "class App036 { public function calc() { global \$g036; \$g036 = 5; return PHP_INT_SIZE + \$g036; } }\n"
);
require $tmp;
app_fn();
$o = new App036();
$o->calc();   // warm both run_time_caches

$n = zealphp_reset_request_rtcaches();
echo "reset_ge2=", ($n >= 2 ? "yes" : "no"), "\n";   // app_fn + App036::calc; boot_helper skipped

// Everything still callable after the reset (re-resolves cold; no crash / SEGV).
echo "boot_helper=", boot_helper(), "\n";
echo "app_fn=", app_fn(), "\n";
echo "calc=", $o->calc(), "\n";

$n2 = zealphp_reset_request_rtcaches();   // idempotent + crash-free
echo "reset2_ge2=", ($n2 >= 2 ? "yes" : "no"), "\n";
@unlink($tmp);
echo "DONE\n";
?>
--EXPECT--
reset_ge2=yes
boot_helper=4
app_fn=8
calc=13
reset2_ge2=yes
DONE

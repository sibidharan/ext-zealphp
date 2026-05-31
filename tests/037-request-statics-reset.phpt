--TEST--
037: per-request function-static reset — re-initialises per-request FUNCTION and METHOD statics to their template values, skips boot/snapshot symbols (coroutine-legacy PHP-FPM "fresh process per request" contract)
--SKIPIF--
<?php if (!extension_loaded('zealphp')) die('skip Required extension missing: zealphp'); ?>
--FILE--
<?php
/* coroutine-legacy function-static reset.
 *
 * PHP's shutdown_executor() destroys every user function/method's live
 * static_variables table at request shutdown, so `static $x = INIT;`
 * re-initialises to INIT on the next request (fresh-process-per-request). A
 * long-lived OpenSwoole worker never runs that per-request shutdown, so a
 * function-local static keeps its last value ACROSS requests. The canonical
 * break is WordPress's `static $first_init = true;` in wp_start_object_cache():
 * set false at the end of request 1, the stale false makes request 2 skip
 * wp_cache_init() -> $wp_object_cache null -> "switch_to_blog() on null" 500.
 *
 * zealphp_reset_request_statics() mirrors shutdown_executor(): for each
 * per-request user function and class method it does zend_array_destroy(live) +
 * ZEND_MAP_PTR_SET(static_variables_ptr, NULL) so the next call re-dups the
 * immutable template. Boot/snapshot symbols (process-stable, per-worker) are
 * skipped. The framework calls it once per request.
 */

// Boot symbol: a top-level function is bound at COMPILE time -> in the snapshot
// taken below -> per-worker -> must be SKIPPED (keeps counting across "requests").
function boot_seq() { static $n = 0; return ++$n; }

zealphp_silent_redeclare(true);
zealphp_process_state_snapshot();   // boot baseline (captures boot_seq)

// Per-request symbols declared at RUNTIME via require (the legacy bootstrap
// pattern) AFTER the snapshot -> NOT boot -> reset each request. One global
// function and one class method, the WP-style `static $first_init` case.
$tmp = sys_get_temp_dir() . '/zl037_' . getmypid() . '.php';
file_put_contents($tmp,
    "<?php\n"
    . "function app_seq() { static \$n = 0; return ++\$n; }\n"
    . "class App037 { public function seq() { static \$s = 100; return ++\$s; } }\n"
);
require $tmp;

// Warm + mutate the statics.
echo "app1=", app_seq(), "\n";   // 1
echo "app2=", app_seq(), "\n";   // 2
$o = new App037();
echo "m1=", $o->seq(), "\n";     // 101
echo "m2=", $o->seq(), "\n";     // 102
echo "boot1=", boot_seq(), "\n"; // 1
echo "boot2=", boot_seq(), "\n"; // 2

// Reset per-request statics. boot_seq skipped; app_seq + App037::seq counted.
$n = zealphp_reset_request_statics();
echo "reset_ge2=", ($n >= 2 ? "yes" : "no"), "\n";

// Per-request statics re-initialise to their INITIAL template values (as if a
// fresh request) — no crash / SEGV when destroying + re-duping.
echo "app_after=", app_seq(), "\n";   // 1  (template $n=0, then ++)
echo "m_after=", $o->seq(), "\n";     // 101 (template $s=100, then ++)
// Boot static is NOT reset (process-stable, per-worker) -> keeps counting.
echo "boot_after=", boot_seq(), "\n"; // 3

$n2 = zealphp_reset_request_statics();   // idempotent + crash-free
echo "reset2_ge2=", ($n2 >= 2 ? "yes" : "no"), "\n";
@unlink($tmp);
echo "DONE\n";
?>
--EXPECT--
app1=1
app2=2
m1=101
m2=102
boot1=1
boot2=2
reset_ge2=yes
app_after=1
m_after=101
boot_after=3
reset2_ge2=yes
DONE

--TEST--
036: per-request run_time_cache reset — resets per-request (runtime-declared) user functions, SKIPS boot/snapshot symbols (coroutine-legacy constant-staleness fix)
--SKIPIF--
<?php if (!extension_loaded('zealphp')) die('skip Required extension missing: zealphp'); ?>
--FILE--
<?php
/* coroutine-legacy run_time_cache staleness fix.
 *
 * Persisted user functions (kept across requests by silent-redeclare) cache
 * resolved constant/symbol pointers in an op_array run_time_cache that lives in
 * CG(arena) — rewound every request. The persisted map_ptr then dangles into the
 * reused arena slot, so a later request reads a STALE resolution (e.g.
 * `1024 * KB_IN_BYTES` -> "Unsupported operand types: string * int").
 *
 * zealphp_reset_request_rtcaches() nulls the run_time_cache map_ptr of
 * PER-REQUEST functions (declared AFTER the boot snapshot — i.e. via runtime
 * require/include, the legacy bootstrap pattern) so they re-init/re-resolve.
 * It MUST SKIP boot/snapshot functions — those have caches in the stable arena
 * region; resetting them re-allocates into the volatile region and dangles after
 * the next rewind -> SEGV in zend_fetch_ce_from_cache_slot.
 */

// Boot symbol: a top-level function is bound at COMPILE time, so it is in the
// snapshot taken below -> stable -> must be SKIPPED by the reset.
function boot_helper() { return strlen('boot'); }
boot_helper();

zealphp_silent_redeclare(true);
zealphp_process_state_snapshot();   // boot baseline (captures boot_helper)

// Per-request symbols: declared at RUNTIME via require (the legacy bootstrap
// pattern), AFTER the snapshot -> NOT in the snapshot -> volatile -> reset.
$tmp = sys_get_temp_dir() . '/zl036_' . getmypid() . '.php';
file_put_contents($tmp,
    "<?php\n"
    . "function app_one() { return PHP_INT_SIZE; }\n"                       // bareword constant
    . "function app_two() { return DIRECTORY_SEPARATOR === '/' ? 1 : 0; }\n"
);
require $tmp;
app_one();
app_two();   // warm their run_time_caches

$n = zealphp_reset_request_rtcaches();
echo "reset_count=$n\n";            // the 2 runtime-declared fns; boot_helper skipped

// Everything still callable after the reset (re-resolves; no crash / no SEGV).
echo "boot_helper=", boot_helper(), "\n";
echo "app_one=", app_one(), "\n";
echo "app_two=", app_two(), "\n";

$n2 = zealphp_reset_request_rtcaches();   // idempotent + crash-free
echo "reset_count2=$n2\n";
@unlink($tmp);
echo "DONE\n";
?>
--EXPECT--
reset_count=2
boot_helper=4
app_one=8
app_two=1
reset_count2=2
DONE

--TEST--
072: request-BEGIN static refresh SKIPS snapshot (boot/framework) symbols — memoisation statics persist per worker; only per-request statics refresh (the #59-wave coroutine-legacy perf contract)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// Same gate as 065/066: the BIND_STATIC touched-set registry (which BEGIN
// iterates) is armed by the coroutine-statics lane.
if (!extension_loaded('openswoole')) {
    die("skip begin-refresh registry needs the OpenSwoole coroutine runtime");
}
?>
--FILE--
<?php
/* Request-BEGIN function-static refresh (#28 concurrency companion) must apply
 * the SAME snapshot exemption as the request-END reset. Without it, begin
 * re-initialised the FRAMEWORK's own memoisation statics (`static $cache =
 * null;` guards over filesystem probes / env reads) to their null template on
 * EVERY request, forcing full recomputation per request — measured as the
 * dominant coroutine-legacy cost (~4.5x /ping throughput; see
 * docs/issue-59-perf-statics-begin-exemption.md).
 *
 * Contract pinned here:
 *   - a BOOT (snapshot) symbol's static survives begin  -> memo NOT recomputed
 *   - a post-snapshot (per-request) symbol's static is refreshed to template
 *     -> the WP-class `static $first_init` guard un-latches (the #28 fix)
 */

// Boot memo — compiled at top level, captured by the snapshot below. The
// $GLOBALS counter is the wipe-proof observable: if begin wiped the static,
// the next call recomputes and the counter climbs.
function boot_memo() {
    static $cache = null;
    if ($cache === null) {
        $GLOBALS['zl072_computes'] = ($GLOBALS['zl072_computes'] ?? 0) + 1;
        $cache = 'v';
    }
    return $cache;
}

zealphp_silent_redeclare(true);
zealphp_coroutine_statics(true);     // arm the BIND_STATIC touched-set registry
zealphp_process_state_snapshot();    // boot baseline (captures boot_memo)

// Per-request symbol: declared at RUNTIME via require AFTER the snapshot (the
// legacy bootstrap pattern) -> NOT a snapshot symbol -> begin must refresh it.
$tmp = sys_get_temp_dir() . '/zl072_' . getmypid() . '.php';
file_put_contents($tmp,
    "<?php\n"
    . "function app_guard() { static \$done = false; \$was = \$done; \$done = true;"
    . " return \$was ? 'latched' : 'fresh'; }\n"
);
require $tmp;

// Touch both so their statics are live + mutated in the registry.
echo "memo1=", boot_memo(), "\n";                        // v (computes -> 1)
echo "computes1=", $GLOBALS['zl072_computes'], "\n";     // 1
echo "guard1=", app_guard(), "\n";                       // fresh (latches)
echo "guard2=", app_guard(), "\n";                       // latched

// Request BEGIN — refreshes app_guard (per-request), SKIPS boot_memo (snapshot).
$n = zealphp_reset_request_statics_begin();
echo "begin_ge1=", ($n >= 1 ? "yes" : "no"), "\n";

echo "guard_after=", app_guard(), "\n";                  // fresh (template restored)
echo "memo_after=", boot_memo(), "\n";                   // v
echo "computes_after=", $GLOBALS['zl072_computes'], "\n"; // STILL 1 — no recompute

@unlink($tmp);
echo "DONE\n";
?>
--EXPECT--
memo1=v
computes1=1
guard1=fresh
guard2=latched
begin_ge1=yes
guard_after=fresh
memo_after=v
computes_after=1
DONE

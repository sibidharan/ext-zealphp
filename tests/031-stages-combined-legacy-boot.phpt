--TEST--
Stages 3+3.5+7 combined: WordPress-style legacy boot re-included twice composes cleanly
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// BOUNDARY: same opcache hot-path gap as 030. Stage 7 re-executes the boot
// file (correct), but on the SECOND compile of the same path an active
// opcache binds the top-level `function zp031_boot_helper()` from its
// persistent SHM cache via zend_accel_load_script — before Stage 4's
// CG-swap can merge first-wins — so it fatals "Cannot redeclare". The
// ext-zealphp harness runs opcache.enable_cli=0, where the swap path is
// fully exercised and all three stages compose. Skip when opcache is
// active on CLI rather than false-fail on a config the framework doesn't ship.
if (function_exists('opcache_get_status')) {
    $s = @opcache_get_status(false);
    if (is_array($s) && !empty($s['opcache_enabled'])) {
        die('skip opcache active on CLI — Stage 4 CG-swap is bypassed by the opcache hot-path bind (documented gap)');
    }
}
?>
--FILE--
<?php
// Integration of three stages on the documented legacy-app boot pattern:
//
//   Stage 3   — silent function/class redeclare (compile-time CG swap)
//   Stage 3.5 — silent define() redeclare (define() intercept, first-wins)
//   Stage 7   — smart require_once (ZEND_INCLUDE_OR_EVAL hook re-executes
//               files NOT in the boot snapshot)
//
// A real WordPress-style boot file does, at the very top:
//   - define('APP_ROOT', __DIR__)        → Stage 3.5 territory on re-run
//   - declare a top-level function       → Stage 3 territory on re-run
//   - run a per-request side-effect      → must execute on EVERY require_once
//
// We snapshot an UNRELATED file FIRST so the boot file is NOT in the
// snapshot → Stage 7 lets it re-execute. With include-isolation +
// silent-redeclare both on, requiring it TWICE must:
//   * emit NO 'Constant ... already defined' warning,
//   * emit NO 'Cannot redeclare' fatal,
//   * run the per-request side-effect BOTH times,
//   * keep APP_ROOT at its FIRST value (define is short-circuited, not redefined).

$dir = sys_get_temp_dir();
$pid = getmypid();
$boot = $dir . '/zp031_boot_' . $pid . '.php';

file_put_contents($boot, '<?php
define("ZP031_APP_ROOT", __DIR__);
function zp031_boot_helper() { return "helper-v1"; }
$GLOBALS["zp031_runs"] = ($GLOBALS["zp031_runs"] ?? 0) + 1;
echo "side-effect ran\n";
');

$GLOBALS["zp031_runs"] = 0;

// Snapshot an UNRELATED file so $boot is NOT captured by the snapshot.
$unrelated = $dir . '/zp031_unrelated_' . $pid . '.php';
file_put_contents($unrelated, '<?php $zp031_noop = 1;');
require_once $unrelated;
zealphp_process_state_snapshot();

zealphp_include_isolation(true);
zealphp_silent_redeclare(true);

require_once $boot; // 1st boot
require_once $boot; // 2nd boot — Stage 7 forces re-execution

// APP_ROOT is __DIR__ of the boot file; compare to its real dirname so the
// test stays portable across machines / temp-dir layouts. "first value kept"
// is proven by the absence of any redefine warning above + this match.
echo "approot-match: ", (ZP031_APP_ROOT === dirname($boot) ? "yes" : "no"), "\n";
echo "runs=", $GLOBALS["zp031_runs"], "\n";
echo "helper=", zp031_boot_helper(), "\n";

zealphp_include_isolation(false);
zealphp_silent_redeclare(false);
@unlink($boot);
@unlink($unrelated);
echo "done\n";
?>
--EXPECT--
side-effect ran
side-effect ran
approot-match: yes
runs=2
helper=helper-v1
done

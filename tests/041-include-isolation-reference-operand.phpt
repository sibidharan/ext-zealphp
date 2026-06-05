--TEST--
041: Stage 7 include isolation re-executes a require_once whose operand is a REFERENCE (#13)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (function_exists('opcache_get_status')) {
    $s = @opcache_get_status(false);
    if (is_array($s) && !empty($s['opcache_enabled'])) {
        die('skip opcache active on CLI');
    }
}
?>
--FILE--
<?php
// #13: `require_once $ref;` where $ref is a CV holding a REFERENCE. Before the
// fix the operand arrived as IS_REFERENCE, failed the IS_STRING gate in the
// Stage-7 hook, and fell through to the standard cached require_once — so the
// per-request re-execution never happened (the second require_once no-op'd,
// runs=1). With ZVAL_DEREF the reference resolves to its string and Stage 7
// re-executes it each request (runs=2).
$dir = sys_get_temp_dir();
$pid = getmypid();
$boot = $dir . '/zp041_boot_' . $pid . '.php';
file_put_contents($boot, '<?php $GLOBALS["zp041_runs"] = ($GLOBALS["zp041_runs"] ?? 0) + 1;');

$GLOBALS["zp041_runs"] = 0;

$unrelated = $dir . '/zp041_unrelated_' . $pid . '.php';
file_put_contents($unrelated, '<?php $zp041_noop = 1;');
require_once $unrelated;
zealphp_process_state_snapshot();

zealphp_include_isolation(true);

$path = $boot;
$ref  = &$path;          // $ref is a genuine reference to $path

require_once $ref;                   // request 1 — boot runs (via deref'd reference)
zealphp_include_isolation_reset();   // request boundary
require_once $ref;                   // request 2 — Stage 7 re-executes it

echo "runs=", $GLOBALS["zp041_runs"], "\n";

zealphp_include_isolation(false);
@unlink($boot);
@unlink($unrelated);
echo "done\n";
?>
--EXPECT--
runs=2
done

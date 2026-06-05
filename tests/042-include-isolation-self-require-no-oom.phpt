--TEST--
042: Stage 7 include isolation — a self-requiring file terminates instead of recursing to OOM in sync mode (#11)
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
// #11: Stage 7 evicts a per-request file from EG(included_files) to force
// re-execution. A file that require_once's ITSELF would, without a guard, be
// evicted-then-re-included forever → unbounded recursion → OOM. The recursion
// guard used to be gated on os_get_cid, so SYNC mode (no OpenSwoole scheduler →
// os_get_cid == NULL) got the eviction with NO protection. The fix runs the
// guard in sync too (bucket key 0); the self-require now no-ops on the second
// hit and the script terminates cleanly.
$dir = sys_get_temp_dir();
$pid = getmypid();
$self = $dir . '/zp042_self_' . $pid . '.php';
file_put_contents($self, '<?php
$GLOBALS["zp042_runs"] = ($GLOBALS["zp042_runs"] ?? 0) + 1;
require_once "' . $self . '";   // self-require — must NOT recurse forever
');

$GLOBALS["zp042_runs"] = 0;

$unrelated = $dir . '/zp042_unrelated_' . $pid . '.php';
file_put_contents($unrelated, '<?php $zp042_noop = 1;');
require_once $unrelated;
zealphp_process_state_snapshot();

zealphp_include_isolation(true);
require_once $self;       // runs once; the nested self-require is guarded to a no-op
zealphp_include_isolation(false);

echo "runs=", $GLOBALS["zp042_runs"], "\n";
@unlink($self);
@unlink($unrelated);
echo "done\n";
?>
--EXPECT--
runs=1
done

--TEST--
Stage 7: a snapshotted bootstrap file stays cached (not re-executed) under include isolation
--EXTENSIONS--
zealphp
--FILE--
<?php
// Stage 7 complement to 027: a file that IS in the boot snapshot stays
// cached. The opcode hook leaves snapshotted files alone (DISPATCH normally),
// so a require_once after the snapshot is a standard no-op — the boot file's
// side effects run exactly once.
//
// Gate note: the handler is gated on (isolation_enabled && state_snapshotted).
// Snapshot state is a process-global flag that, once set, cannot be reset
// within a single process — so the "no snapshot => no-op" arm of the gate
// can't be exercised here without leaving the snapshot set for the cached
// assertion below. We instead pin the isolation-toggle half of the gate:
// with isolation OFF, a require_once is the standard cached no-op too.

$GLOBALS['boot'] = 0;

$tmp = tempnam(sys_get_temp_dir(), 'zealphp_boot_');
$file = $tmp . '.php';
file_put_contents($file, '<?php $GLOBALS["boot"]++; echo "boot-exec\n";');

// Run the boot file ONCE before snapshotting.
require_once $file;            // executes -> boot == 1
echo "after-first=" . $GLOBALS['boot'] . "\n";

// Snapshot now: the file is already in EG(included_files), so it lands IN the
// snapshot set. Then turn isolation on.
zealphp_process_state_snapshot();
zealphp_include_isolation(true);

// require_once again. Because the file is in the snapshot, the Stage 7 hook
// does NOT evict it -> standard cached no-op -> no re-execution.
require_once $file;
echo "after-snapshot-require=" . $GLOBALS['boot'] . "\n";

// Gate cross-check: isolation OFF also yields the standard cached no-op.
zealphp_include_isolation(false);
require_once $file;
echo "after-isolation-off-require=" . $GLOBALS['boot'] . "\n";

@unlink($file);
@unlink($tmp);
?>
--EXPECT--
boot-exec
after-first=1
after-snapshot-require=1
after-isolation-off-require=1

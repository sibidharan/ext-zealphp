--TEST--
Stage 7: non-snapshot per-request file re-executes on each require_once when include isolation is on
--EXTENSIONS--
zealphp
--FILE--
<?php
// Stage 7 smart require_once. PHP's require_once cache (EG(included_files))
// is process-wide: in a persistent server, a file required on request 1 is a
// no-op on request 2+. Stage 7 hooks ZEND_INCLUDE_OR_EVAL: for a file NOT in
// the boot snapshot, it removes the entry from EG(included_files) so the
// standard handler re-includes it. Per-request logic re-executes; boot stays
// cached.
//
// The opcode handler is gated on (isolation_enabled && state_snapshotted).
// So we snapshot FIRST — BEFORE the temp file exists — so the file is NOT in
// the snapshot, then enable isolation, then require_once twice.

// Snapshot first (temp file does not exist yet -> not in snapshot set).
zealphp_process_state_snapshot();
zealphp_include_isolation(true);

$GLOBALS['c'] = 0;

$tmp = tempnam(sys_get_temp_dir(), 'zealphp_reexec_');
$file = $tmp . '.php';
file_put_contents($file, '<?php $GLOBALS["c"]++; echo "exec\n";');

require_once $file;   // executes once
require_once $file;   // Stage 7 forces re-execution

echo "count=" . $GLOBALS['c'] . "\n";

zealphp_include_isolation(false);
@unlink($file);
@unlink($tmp);
?>
--EXPECT--
exec
exec
count=2

--TEST--
Stage 7: per-request file is idempotent WITHIN a request, re-executes ACROSS requests (reset boundary)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// Stage 7's per-request reincluded tracking is inherently coroutine-scoped: it
// lives entirely on OpenSwoole's coroutine runtime (the per-coroutine reincluded
// set, populated in the include opcode handler and torn down in on_close). With
// no OpenSwoole loaded there is NO coroutine runtime at all — no scheduler, no
// per-coroutine identity — so the once-per-request boundary can't exist. That is
// a test-only condition (a real ZealPHP server always runs on OpenSwoole). Skip
// cleanly so `make test` (php -n, no OpenSwoole) and the ASAN job don't FAIL it.
if (!extension_loaded('openswoole')) {
    die("skip Stage 7 per-request isolation requires the OpenSwoole coroutine runtime");
}
?>
--FILE--
<?php
// Stage 7 smart require_once. PHP's require_once cache (EG(included_files))
// is process-wide: in a persistent server, a file required on request 1 is a
// no-op on request 2+. Stage 7 hooks ZEND_INCLUDE_OR_EVAL: for a file NOT in
// the boot snapshot, it removes the entry from EG(included_files) so the
// standard handler re-includes it on the NEXT request.
//
// CRITICAL: require_once stays idempotent WITHIN one request — re-executing on
// every require_once call (incl. re-entrant/circular) would recurse without
// bound (real failure: phpmyadmin's sodium_compat autoload OOMs the worker).
// Stage 7 force-re-includes each file ONCE per request; a request boundary is
// marked by zealphp_include_isolation_reset() (in a real server each request
// is a fresh coroutine, so this is automatic).

// Snapshot first (temp file does not exist yet -> not in snapshot set).
zealphp_process_state_snapshot();
zealphp_include_isolation(true);

$GLOBALS['c'] = 0;

$tmp = tempnam(sys_get_temp_dir(), 'zealphp_reexec_');
$file = $tmp . '.php';
file_put_contents($file, '<?php $GLOBALS["c"]++; echo "exec\n";');

require_once $file;                    // request 1 -> executes (count -> 1)
require_once $file;                    // SAME request -> idempotent no-op (NOT re-executed)
zealphp_include_isolation_reset();     // request boundary
require_once $file;                    // request 2 -> re-executes (count -> 2)

echo "count=" . $GLOBALS['c'] . "\n";

zealphp_include_isolation(false);
@unlink($file);
@unlink($tmp);
?>
--EXPECT--
exec
exec
count=2

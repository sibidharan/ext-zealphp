--TEST--
068: ext#12 — zealphp_oparray_cache() skips re-exec of pure-declaration files (no orphan-loser leak), but never skips files with side effects
--EXTENSIONS--
zealphp
--FILE--
<?php
// ext#12: under Stage 7, re-executing a require_once'd file re-compiles it, and
// silent-redeclare orphan-leaks the re-compiled inherited class (~KB/re-exec).
// zealphp_oparray_cache(true) makes Stage 7 SKIP re-executing a file whose ENTIRE
// top-level effect is class/function declarations already present as winners —
// so it is never re-compiled → no loser → no leak. A file with ANY side effect
// (top-level code) must STILL re-execute every request (safety).
zealphp_process_state_snapshot();
zealphp_include_isolation(true);
zealphp_silent_redeclare(true);
zealphp_oparray_cache(true);

$dir = sys_get_temp_dir();

// (A) PURE declaration file (inherited class — the leak case).
$pure = tempnam($dir, 'zl68p_') . '.php';
file_put_contents($pure, '<?php class Zp68Base {} class Zp68Child extends Zp68Base { public function who(){ return "child"; } }');

// (B) MIXED file: a declaration PLUS top-level side-effecting code.
$mixed = tempnam($dir, 'zl68m_') . '.php';
file_put_contents($mixed, '<?php class Zp68Helper {} $GLOBALS["zp68_count"]++;');

$GLOBALS['zp68_count'] = 0;

$N = 300;
$base_mem = 0; $class_ok = 0;
for ($i = 0; $i < $N; $i++) {
    zealphp_include_isolation_reset();        // request boundary
    require_once $pure;                        // pure → should be skipped from req 2+
    require_once $mixed;                        // mixed → must re-execute every request
    if ((new Zp68Child())->who() === 'child') { $class_ok++; }
    if ($i === 20) { $base_mem = memory_get_usage(true); }
}
$growth = memory_get_usage(true) - $base_mem;

echo "pure_class_usable=" . ($class_ok === $N ? "YES" : "NO") . "\n";
echo "pure_no_leak=" . ($growth === 0 ? "YES" : "NO($growth)") . "\n";
// mixed file re-executed every request (NOT skipped — has a side effect):
echo "mixed_reexecuted=" . ($GLOBALS['zp68_count'] === $N ? "YES" : "NO({$GLOBALS['zp68_count']}/$N)") . "\n";

zealphp_oparray_cache(false);
zealphp_include_isolation(false);
@unlink($pure); @unlink($mixed);
echo "DONE\n";
?>
--EXPECT--
pure_class_usable=YES
pure_no_leak=YES
mixed_reexecuted=YES
DONE

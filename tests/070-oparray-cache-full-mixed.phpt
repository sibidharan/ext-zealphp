--TEST--
070: ext#12 Option A — zealphp_oparray_cache_full() caches+reuses a MIXED inherited-class file (no orphan leak, no arena growth), while its per-request code still re-executes
--EXTENSIONS--
zealphp
--FILE--
<?php
// A require_once'd file that declares an INHERITED class AND has per-request
// top-level code is the #12 leak case that the pure-declaration skip cannot
// cover. zealphp_oparray_cache_full(true) compiles it ONCE and re-executes a
// shallow copy each request: no re-compile => no inherited-loser CE and no
// CG(arena) growth, yet the per-request code still runs every request.
zealphp_process_state_snapshot();
zealphp_include_isolation(true);
zealphp_silent_redeclare(true);
zealphp_oparray_cache_full(true);

$dir = sys_get_temp_dir();
$parent = tempnam($dir, 'zl70p_') . '.php';
file_put_contents($parent, '<?php class Zp70Base { public $a = 10; }');
$mixed = tempnam($dir, 'zl70m_') . '.php';
file_put_contents($mixed,
    '<?php require_once ' . var_export($parent, true) . ';' .
    ' class Zp70Child extends Zp70Base { public $b = 5; public function go(){ return $this->a + $this->b; } }' .
    ' $GLOBALS["zp70_count"]++;');

$GLOBALS['zp70_count'] = 0;
$N = 400;
$base_mem = 0; $class_ok = 0;
for ($i = 0; $i < $N; $i++) {
    zealphp_include_isolation_reset();
    require_once $mixed;
    if ((new Zp70Child())->go() === 15) { $class_ok++; }
    if ($i === 20) { $base_mem = memory_get_usage(true); }
}
$growth = memory_get_usage(true) - $base_mem;

echo "class_usable=" . ($class_ok === $N ? "YES" : "NO($class_ok/$N)") . "\n";
echo "code_reexecuted=" . ($GLOBALS['zp70_count'] === $N ? "YES" : "NO({$GLOBALS['zp70_count']}/$N)") . "\n";
echo "no_leak=" . ($growth === 0 ? "YES" : "NO($growth)") . "\n";

zealphp_oparray_cache_full(false);
zealphp_include_isolation(false);
@unlink($parent); @unlink($mixed);
echo "DONE\n";
?>
--EXPECT--
class_usable=YES
code_reexecuted=YES
no_leak=YES
DONE

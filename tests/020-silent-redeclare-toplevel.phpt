--TEST--
Stage 4: silent-redeclare survives compile-time top-level redeclarations (CG-swap design)
--EXTENSIONS--
zealphp
--FILE--
<?php
$tmp = tempnam(sys_get_temp_dir(), 'zealphp_toplevel_');
$file = $tmp . '.php';
file_put_contents($file, '<?php
function zealphp_test_toplevel_fn() { return "first"; }
class ZealphpTestToplevelCls { public function w() { return "first-cls"; } }
');

require $file;
echo zealphp_test_toplevel_fn(), "\n";
echo (new ZealphpTestToplevelCls)->w(), "\n";

zealphp_silent_redeclare(true);

// Re-include the SAME file. Stage 4's CG-table swap routes compile-time
// declares into scratch tables that get merged into the real table
// first-wins, so top-level redecls no longer dup-error.
require $file;
echo "second-include ok\n";

echo zealphp_test_toplevel_fn(), "\n";
echo (new ZealphpTestToplevelCls)->w(), "\n";

zealphp_silent_redeclare(false);
@unlink($tmp);
@unlink($file);
?>
--EXPECT--
first
first-cls
second-include ok
first
first-cls

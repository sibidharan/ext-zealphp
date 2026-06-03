--TEST--
zealphp_require_global() runs an included file at TRUE global scope
--EXTENSIONS--
zealphp
--FILE--
<?php
// Stage 8: a bare top-level `$x = ...` in a file run via zealphp_require_global()
// must land in $GLOBALS (real global), unlike a plain `include` nested inside a
// PHP function where it becomes a function-local. This is what lets unmodified
// require_once apps (WordPress's $menu/$submenu/$_wp_submenu_nopriv) work in
// coroutine-legacy mode.

$target = __DIR__ . '/039_target.php';
file_put_contents($target, "<?php\n\$stage8_global = 'GLOBAL_OK';\n\$stage8_arr = [1, 2, 3];\nreturn 42;\n");

function read_via_global() {
    global $stage8_global;
    return $stage8_global;
}

// Baseline: a plain `include` lexically inside a function -> file-scope var is
// a function-local, never reaches the global table.
function baseline_include($f) {
    $r = include $f;
    return [$r, read_via_global()];
}

// Stage 8: zealphp_require_global runs the file at global scope.
function stage8_include($f) {
    $r = zealphp_require_global($f);
    return [$r, read_via_global()];
}

[$r1, $g1] = baseline_include($target);
echo "baseline: return=" . var_export($r1, true) . " global=" . var_export($g1, true) . "\n";

unset($GLOBALS['stage8_global'], $GLOBALS['stage8_arr']);

[$r2, $g2] = stage8_include($target);
echo "stage8: return=" . var_export($r2, true) . " global=" . var_export($g2, true) . "\n";
echo "in GLOBALS: stage8_global=" . (isset($GLOBALS['stage8_global']) ? 'yes' : 'no')
    . " stage8_arr=" . (isset($GLOBALS['stage8_arr']) ? 'yes' : 'no') . "\n";

// A no-explicit-return file returns int 1 (include semantics preserved).
$noret = __DIR__ . '/039_noret.php';
file_put_contents($noret, "<?php\n\$stage8_nr = 'NR';\n");
$r3 = zealphp_require_global($noret);
echo "noret: return=" . var_export($r3, true) . "\n";

@unlink($target);
@unlink($noret);
?>
--EXPECT--
baseline: return=42 global=NULL
stage8: return=42 global='GLOBAL_OK'
in GLOBALS: stage8_global=yes stage8_arr=yes
noret: return=1

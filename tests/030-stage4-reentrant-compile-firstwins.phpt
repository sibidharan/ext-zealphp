--TEST--
Stage 4: re-entrant compile + first-wins VALUE for top-level redeclares (extends 020)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// BOUNDARY: Stage 4's CG-table swap (and Stage 6.2's per-worker op_array
// cache) intercept compile_file, but the opcache HOT PATH
// (zend_accel_load_script) binds top-level decls from a persistent SHM
// cache that our hook never sees. On a same-path re-include, an active
// opcache hits "Cannot redeclare" before Stage 4 can merge first-wins.
// This is the documented residual gap (zealphp.c "the opcache hot path
// remains the gap"; reassessment Stage 5/6 territory). The ext-zealphp
// test harness runs with opcache.enable_cli=0, where the gap is inert and
// the CG-swap path is fully exercised. Skip if opcache is active on CLI so
// we never false-fail on a config the framework doesn't ship.
if (function_exists('opcache_get_status')) {
    $s = @opcache_get_status(false);
    if (is_array($s) && !empty($s['opcache_enabled'])) {
        die('skip opcache active on CLI — Stage 4 CG-swap is bypassed by the opcache hot-path bind (documented gap)');
    }
}
?>
--FILE--
<?php
// Extends 020. Two halves:
//
// (a) FIRST-WINS VALUE — 020 only proved a top-level redeclare doesn't
//     dup-error. Here we go further: the SECOND copy declares a DIFFERENT
//     body for the same top-level symbol. Stage 4's CG-table swap merges
//     scratch into the real table first-wins (and Stage 6.2's per-worker
//     op_array cache returns the first-compiled body on warm re-compile of
//     the same path), so the FIRST body is what keeps executing — both
//     across two distinct files declaring the same name AND across a
//     same-path re-include whose body was rewritten on disk.
//
// (b) RE-ENTRANT / NESTED compile — a file that itself require()s another
//     file declaring top-level symbols, re-included twice. The CG-table
//     swap is stack-local; nested compiles get their own scratch tables and
//     restore in reverse order. Must not dup-error and must not hang.
//     Pins the scratch save/restore ordering under recursion.

$dir = sys_get_temp_dir();
$pid = getmypid();

// ── (a1) Two DIFFERENT files, same fn+class name, different bodies ──
$a = $dir . '/zp030_a_' . $pid . '.php';
$b = $dir . '/zp030_b_' . $pid . '.php';
file_put_contents($a, '<?php
function zp030_diff_fn() { return "BODY-A"; }
class Zp030DiffCls { public function v() { return "CLS-A"; } }
');
file_put_contents($b, '<?php
function zp030_diff_fn() { return "BODY-B"; }
class Zp030DiffCls { public function v() { return "CLS-B"; } }
');

require $a;
echo "a-fn: ", zp030_diff_fn(), "\n";
echo "a-cls: ", (new Zp030DiffCls)->v(), "\n";

zealphp_silent_redeclare(true);
require $b; // different file, different bodies, same names — first wins
echo "b-fn: ", zp030_diff_fn(), "\n";
echo "b-cls: ", (new Zp030DiffCls)->v(), "\n";
zealphp_silent_redeclare(false);

// ── (a2) SAME path re-included, body rewritten on disk between includes ──
$c = $dir . '/zp030_c_' . $pid . '.php';
file_put_contents($c, '<?php function zp030_same_fn() { return "FIRST"; }');
require $c;
echo "same1: ", zp030_same_fn(), "\n";

zealphp_silent_redeclare(true);
file_put_contents($c, '<?php function zp030_same_fn() { return "SECOND"; }');
require $c; // same path, rewritten body — first-wins still holds
echo "same2: ", zp030_same_fn(), "\n";
zealphp_silent_redeclare(false);

// ── (b) RE-ENTRANT / NESTED compile, twice ──
$inner = $dir . '/zp030_inner_' . $pid . '.php';
$outer = $dir . '/zp030_outer_' . $pid . '.php';
file_put_contents($inner, '<?php
function zp030_inner_fn() { return "inner-first"; }
class Zp030InnerCls { public function v() { return "inner-cls-first"; } }
');
$innerEsc = var_export($inner, true);
file_put_contents($outer, '<?php
require ' . $innerEsc . ';
function zp030_outer_fn() { return "outer-first"; }
class Zp030OuterCls { public function v() { return "outer-cls-first"; } }
');

zealphp_silent_redeclare(true);
require $outer; // nested compile: outer's compile-exec require()s inner
echo "nest1\n";
echo zp030_inner_fn(), " ", (new Zp030InnerCls)->v(), "\n";
echo zp030_outer_fn(), " ", (new Zp030OuterCls)->v(), "\n";

require $outer; // re-entrant: nested require of inner again, must not dup/hang
echo "nest2\n";
echo zp030_inner_fn(), " ", (new Zp030InnerCls)->v(), "\n";
echo zp030_outer_fn(), " ", (new Zp030OuterCls)->v(), "\n";
zealphp_silent_redeclare(false);

@unlink($a);
@unlink($b);
@unlink($c);
@unlink($inner);
@unlink($outer);
echo "done\n";
?>
--EXPECT--
a-fn: BODY-A
a-cls: CLS-A
b-fn: BODY-A
b-cls: CLS-A
same1: FIRST
same2: FIRST
nest1
inner-first inner-cls-first
outer-first outer-cls-first
nest2
inner-first inner-cls-first
outer-first outer-cls-first
done

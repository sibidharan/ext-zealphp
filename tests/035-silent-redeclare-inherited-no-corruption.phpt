--TEST--
v0.3.24: re-compiled INHERITED class (silent-redeclare first-wins) must not corrupt the live class — no SEGV on `new`
--EXTENSIONS--
zealphp
--FILE--
<?php
/* Regression for the Stage-4 first-wins merge crash.
 *
 * A re-compiled "loser" class WITH A PARENT was inheritance-linked against the
 * LIVE winner hierarchy (Stage 4 swaps CG only, never EG), so
 * destroy_zend_class() on the loser freed winner-owned inheritance structures
 * -> the winner's default_properties_table was corrupted -> SEGV in
 * _object_properties_init (zend_gc_addref on a near-null counted pointer) on the
 * next `new`. THIS is the crash that took WordPress (and any
 * require_once-bootstrap inherited class) down under coroutine-legacy, where
 * Stage 7 re-executes the declarations every request.
 *
 * Reproduced here WITHOUT OpenSwoole: plain `include` always re-compiles, and
 * silent-redeclare makes the second compile's class a first-wins loser — the
 * exact merge path Stage 7 hits. A FLAT class (Z035Base) is a self-contained
 * loser and stays safe to destroy; the INHERITED loser (Z035Child) must be
 * orphaned, not destroyed.
 */
zealphp_silent_redeclare(true);

$f = __DIR__ . '/035_inherited.inc';
file_put_contents(
    $f,
    "<?php\n"
    . "class Z035Base { public \$a = 10; public function who() { return 'base'; } }\n"
    . "class Z035Child extends Z035Base { public \$b = 20; public function who() { return 'child'; } }\n"
);

include $f;   // first compile — winners registered
include $f;   // re-compile — Z035Child is a first-wins LOSER (inherited -> orphaned, not destroyed)
include $f;   // and again

$o = new Z035Child();                 // pre-fix: SEGV here (corrupted default_properties_table)
echo $o->a + $o->b, "\n";             // inherited default (10) + own default (20)
echo get_class($o), "\n";
echo get_parent_class($o), "\n";
echo $o->who(), "\n";                 // overridden method resolves correctly

@unlink($f);
?>
--EXPECT--
30
Z035Child
Z035Base
child

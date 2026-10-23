--TEST--
088: the cwd stage's chdir on a coroutine switch drops the relative-path stat cache, so is_file('x') never answers for another coroutine's directory
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip needs the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// PHP's chdir() drops the relative stat cache; the cwd stage switched
// directories with a bare VCWD_CHDIR, so the last relative stat survived the
// switch and answered for the wrong directory.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp088_yield(int $ms): void {
    $c = new Channel(1);
    Timer::after($ms, static fn() => $c->push(1));
    $c->pop(2.0);
}
$base = sys_get_temp_dir() . '/zp088_base_' . getmypid();
$dirA = sys_get_temp_dir() . '/zp088_a_' . getmypid();
@mkdir($base); @mkdir($dirA);
file_put_contents("$dirA/only-in-a.txt", 'A');
$cwd = getcwd();
chdir($base);
zealphp_cwd_isolation(true);
$out = [];
Co::run(function () use (&$out, $dirA) {
    $d = new Channel(2);
    go(function () use (&$out, $dirA, $d) {
        zealphp_superglobals_owner();
        chdir($dirA);
        $out['a_sees'] = is_file('only-in-a.txt');
        zp088_yield(20);
        $out['a_sees_after'] = is_file('only-in-a.txt');
        $d->push(1);
    });
    go(function () use (&$out, $d) {
        zealphp_superglobals_owner();
        zp088_yield(5);                       // runs while A is suspended in $dirA
        $out['b_sees'] = is_file('only-in-a.txt');
        $out['b_size'] = @filesize('only-in-a.txt');
        $d->push(1);
    });
    $d->pop(5.0); $d->pop(5.0);
});
zealphp_cwd_isolation(false);
chdir($cwd);
@unlink("$dirA/only-in-a.txt"); @rmdir($dirA); @rmdir($base);
var_dump($out['a_sees'], $out['b_sees'], $out['b_size'], $out['a_sees_after']);
?>
--EXPECT--
bool(true)
bool(false)
bool(false)
bool(true)

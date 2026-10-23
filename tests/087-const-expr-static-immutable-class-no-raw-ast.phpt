--TEST--
087: a constant-expression static default of an opcache-immutable class is never written into the live slot as a raw AST (per-request reset and yield re-park)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime');
if (!function_exists('opcache_get_status')) die('skip needs opcache (immutable classes)');
?>
--INI--
opcache.enable=1
opcache.enable_cli=1
opcache.file_cache_only=0
opcache.jit=disable
--FILE--
<?php
// Under opcache a class from a cached file is IMMUTABLE: its
// default_static_members_table is shared and keeps a constant-expression
// default as an unresolved IS_CONSTANT_AST (the engine resolves it only into
// the live table). The #54 in-place resolve skips immutable classes, so the
// per-request reset AND the yield re-park copied the raw AST into the live
// slot, which the engine never resolves again: the static read as "unknown
// type" (and string conversion of it is undefined behaviour).
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp087_yield(int $ms): void {
    $c = new Channel(1);
    Timer::after($ms, static fn() => $c->push(1));
    $c->pop(2.0);
}
$dir = sys_get_temp_dir() . '/zp087_' . getmypid();
@mkdir($dir);
file_put_contents("$dir/Zp087S.php", "<?php\nenum Zp087S { case On; case Off; }\n");
file_put_contents("$dir/Zp087.php", "<?php\nclass Zp087 { public static \$s = Zp087S::On; public static \$m = [Zp087S::On]; }\n");
spl_autoload_register(function ($c) use ($dir) { $f = "$dir/$c.php"; if (is_file($f)) require $f; });

zealphp_coroutine_globals(true);
$out = [];
Co::run(function () use (&$out) {
    $d = new Channel(2);
    go(function () use (&$out, $d) {                     // request A
        zealphp_superglobals_owner();
        $out['immutable'] = (new ReflectionClass('Zp087'))->isInternal() ? 'internal' : 'user';
        $out['first'] = get_debug_type(Zp087::$s);
        zealphp_reset_request_class_statics();           // the framework's per-request reset
        $out['after_reset'] = get_debug_type(Zp087::$s);
        Zp087::$s = Zp087S::Off;                         // request A's own value
        zp087_yield(20);                                 // save re-parks the slot for peers
        $out['a_after_yield'] = Zp087::$s === Zp087S::Off ? 'own' : get_debug_type(Zp087::$s);
        $d->push(1);
    });
    go(function () use (&$out, $d) {                     // request B, while A is suspended
        zealphp_superglobals_owner();
        zp087_yield(5);
        $out['peer_s'] = get_debug_type(Zp087::$s);
        $out['peer_m'] = is_array(Zp087::$m) ? 'array' : get_debug_type(Zp087::$m);
        $d->push(1);
    });
    $d->pop(5.0); $d->pop(5.0);
});
foreach (['first', 'after_reset', 'a_after_yield', 'peer_s', 'peer_m'] as $k) {
    echo $k, ': ', $out[$k] ?? 'UNSET', "\n";
}
@unlink("$dir/Zp087S.php"); @unlink("$dir/Zp087.php"); @rmdir($dir);
?>
--EXPECT--
first: Zp087S
after_reset: Zp087S
a_after_yield: own
peer_s: Zp087S
peer_m: array

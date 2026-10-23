--TEST--
086: request constants retired at coroutine close are reused per name, so a worker's retained memory is bounded by distinct names, not by requests served
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
// Bug 2 retires (never frees) every request constant at coroutine close, so a
// stale run_time_cache slot can never read freed memory. Each request
// allocates fresh structs for the same names, and every one was kept, value
// included, for the worker's lifetime: memory grew with every request that
// called define() (a remote, unauthenticated growth path under
// defineIsolation). A retired struct is now handed back to the next define()
// of the same name, so the retained set stops growing.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp086_read() { return defined('ZP086_C0') ? ZP086_C0 : '(undef)'; }

zealphp_coroutine_superglobals(true);
zealphp_define_hook(true);
$request = function (int $n) {
    go(function () use ($n) {
        zealphp_superglobals_owner();
        for ($i = 0; $i < 50; $i++) {
            define("ZP086_C$i", str_repeat('x', 200) . $n);
        }
        $c = new Channel(1);
        Timer::after(1, static fn() => $c->push(1));
        $c->pop(1.0);
        $GLOBALS['zp086_last'] = substr(zp086_read(), 200);
        zealphp_constants_clear();
        zealphp_superglobals_clear();
    });
};
for ($r = 0; $r < 20; $r++) { Co::run(fn() => $request($r)); }   // warm-up
$before = memory_get_usage();
for ($r = 20; $r < 220; $r++) { Co::run(fn() => $request($r)); }
$growth = memory_get_usage() - $before;
echo "last request read its own value: ", $GLOBALS['zp086_last'] === '219' ? 'yes' : "NO ({$GLOBALS['zp086_last']})", "\n";
echo "constant gone after clear: ", defined('ZP086_C0') ? 'NO' : 'yes', "\n";
echo "growth bounded: ", $growth < 256 * 1024 ? 'yes' : "NO ($growth bytes)", "\n";
zealphp_define_hook(false);
?>
--EXPECT--
last request read its own value: yes
constant gone after clear: yes
growth bounded: yes

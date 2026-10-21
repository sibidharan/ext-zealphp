--TEST--
075: a request constant read by a boot/snapshot function stays memory-safe across requests — it is retired at worker recycle, not freed at coroutine close (bug 2)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip requires the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// zealphp_reset_request_rtcaches() skips boot/snapshot functions and closures,
// so a FETCH_CONSTANT cache slot in such an op_array survives a request and
// keeps resolving to r1's constant (the #59-wave perf contract — snapshot
// symbols keep process-stable cached targets). On main that struct is freed at
// coroutine close, so r2's cached read is a use-after-free (valgrind: "Invalid
// read" in the FETCH_CONSTANT handler). The fix RETIRES request constants to
// worker recycle instead of freeing them at close, so the retained read stays
// memory-safe. This is a regression guard: a plain run prints the same either
// way (the UAF does not crash without a sanitizer); the ext's valgrind/ASAN CI
// is what catches it — clean after the fix, "Invalid read" before.
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

// Boot-time function: a literal FETCH_CONSTANT with a process-wide op_array cache.
function zp075_read() { return defined('ZP075_C') ? ZP075_C : '(undef)'; }

Coroutine::run(function () {
    zealphp_coroutine_superglobals(true);
    zealphp_define_hook(true);
    zealphp_process_state_snapshot();

    $yield = function (int $ms = 2): void {
        $ch = new Channel(1);
        Timer::after($ms, fn () => $ch->push(1));
        $ch->pop(2.0);
    };
    $done = new Channel(1);

    go(function () use ($yield, $done) {
        define('ZP075_C', 'one');
        echo 'r1: ', zp075_read(), "\n";
        zealphp_reset_request_rtcaches();
        zealphp_constants_clear();
        $yield(2);
        $done->push(1);
    });
    $done->pop();
    $yield(6);

    go(function () use ($yield) {
        define('ZP075_C', 'two');
        echo 'r2: ', zp075_read(), "\n";
        zealphp_reset_request_rtcaches();
        zealphp_constants_clear();
        $yield(2);
    });
    $yield(6);

    zealphp_define_hook(false);
    echo "DONE\n";
});
echo "exit-clean\n";
?>
--EXPECT--
r1: one
r2: one
DONE
exit-clean

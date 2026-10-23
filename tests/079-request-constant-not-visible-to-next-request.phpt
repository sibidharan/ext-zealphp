--TEST--
079: a request constant read from boot-time code (a function and a closure) is not visible to a later request that never defined it
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
// defineIsolation: a constant defined by one request must not leak to the next.
// Boot-time functions and closures (route handlers) cache constant lookups in
// their run_time_cache, which the per-request rtcache reset skips; the next
// request used to read the previous request's value through that slot.
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp079_read() { return defined('ZP079_TENANT') ? ZP079_TENANT : '(undef)'; }
$zp079_closure = function () { return defined('ZP079_TENANT') ? ZP079_TENANT : '(undef)'; };

Coroutine::run(function () use ($zp079_closure) {
    zealphp_coroutine_superglobals(true);
    zealphp_define_hook(true);
    zealphp_process_state_snapshot();

    $yield = function (int $ms = 2): void {
        $ch = new Channel(1);
        Timer::after($ms, fn () => $ch->push(1));
        $ch->pop(2.0);
    };
    $request = function (?string $tenant) use ($yield, $zp079_closure) {
        $done = new Channel(1);
        go(function () use ($tenant, $yield, $zp079_closure, $done) {
            if ($tenant !== null) {
                define('ZP079_TENANT', $tenant);
            }
            echo ($tenant ?? 'anon'), ': fn=', zp079_read(), ' closure=', $zp079_closure(), "\n";
            zealphp_reset_request_rtcaches();
            zealphp_constants_clear();
            $yield(2);
            $done->push(1);
        });
        $done->pop();
        $yield(6);
    };

    $request('alice');
    $request(null);      // never defined it: must not see alice's
    $request('bob');
    $request(null);

    zealphp_define_hook(false);
    echo "DONE\n";
});
?>
--EXPECT--
alice: fn=alice closure=alice
anon: fn=(undef) closure=(undef)
bob: fn=bob closure=bob
anon: fn=(undef) closure=(undef)
DONE

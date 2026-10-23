--TEST--
090: a boot-time function whose parameter default is a request constant re-evaluates it per request (RECV_INIT cache slot is cleared with the constant)
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
// Test 079's sweep nulls FETCH_CONSTANT / DEFINED cache slots. A parameter
// default that names a constant is evaluated by ZEND_RECV_INIT, which caches
// a non-refcounted result in its own slot forever: `function f($t = TENANT)`
// kept returning the first request's tenant.
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp090_tenant($t = ZP090_TENANT) { return $t; }

Coroutine::run(function () {
    zealphp_coroutine_superglobals(true);
    zealphp_define_hook(true);
    zealphp_process_state_snapshot();
    $yield = function (int $ms = 2): void {
        $ch = new Channel(1);
        Timer::after($ms, fn () => $ch->push(1));
        $ch->pop(2.0);
    };
    $request = function (int $tenant) use ($yield) {
        $done = new Channel(1);
        go(function () use ($tenant, $yield, $done) {
            define('ZP090_TENANT', $tenant);
            echo "request $tenant: default=", zp090_tenant(), "\n";
            zealphp_reset_request_rtcaches();
            zealphp_constants_clear();
            $yield(2);
            $done->push(1);
        });
        $done->pop();
        $yield(6);
    };
    $request(1);
    $request(2);
    $request(3);
    zealphp_define_hook(false);
});
?>
--EXPECT--
request 1: default=1
request 2: default=2
request 3: default=3

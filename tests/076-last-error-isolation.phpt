--TEST--
076: per-coroutine error_get_last() isolation — a request never reads a peer's (or a finished request's) last error
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip per-coroutine last-error isolation needs the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// PG(last_error_*) is process-global and OpenSwoole never runs request
// shutdown between requests, so A's warning (path/key/user input) was
// readable by B — while both were in flight, and after A had finished.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp076_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}

zealphp_last_error_isolation(true);

$seen = [];
$done = new Channel(2);

Co::run(function () use (&$seen, $done) {
    go(function () use (&$seen, $done) {
        zealphp_superglobals_owner();
        @trigger_error('ERR-OF-alice', E_USER_NOTICE);
        zp076_yield(20);                      // bob raises his own error meanwhile
        $seen['alice_after_yield'] = error_get_last()['message'] ?? null;
        $done->push(1);
    });
    go(function () use (&$seen, $done) {
        zealphp_superglobals_owner();
        $seen['bob_on_arrival'] = error_get_last()['message'] ?? null;
        @trigger_error('ERR-OF-bob', E_USER_NOTICE);
        zp076_yield(5);
        $seen['bob_after_yield'] = error_get_last()['message'] ?? null;
        $done->push(1);
    });
    $done->pop(5.0);
    $done->pop(5.0);
});
// A later request: in the server a new request coroutine is started by the
// reactor only after the previous ones yielded (re-parked) or closed. Model
// that with a fresh scheduler run once both peers have closed. (A coroutine
// resumed inline by a request's Channel::push gets the pusher's state parked
// first — test 080. The framework does NOT clear the last error per request
// itself; the closing request's re-park in on_close does — test 082.)
Co::run(function () use (&$seen) {
    go(function () use (&$seen) {
        zealphp_superglobals_owner();
        $seen['next_request'] = error_get_last()['message'] ?? null;
    });
});

foreach (['bob_on_arrival', 'alice_after_yield', 'bob_after_yield', 'next_request'] as $k) {
    echo $k, ": ", var_export(array_key_exists($k, $seen) ? $seen[$k] : 'MISSING', true), "\n";
}

zealphp_last_error_isolation(false);
?>
--EXPECT--
bob_on_arrival: NULL
alice_after_yield: 'ERR-OF-alice'
bob_after_yield: 'ERR-OF-bob'
next_request: NULL

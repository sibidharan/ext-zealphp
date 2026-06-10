--TEST--
057: #37 — unclaimed coroutines must not steal request $GLOBALS (go() child yield + service-runner resume)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
?>
--FILE--
<?php
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

Coroutine::run(function () {
    zealphp_coroutine_globals(true);

    // PHP 8.4-safe yield (Co::sleep(float) deprecation): Timer + Channel.
    $yield = function (int $ms = 2) {
        $ch = new Channel(1);
        Timer::after($ms, fn () => $ch->push(1));
        $ch->pop(2.0);
    };

    $fail = 0;

    // SHAPE B precondition: a long-lived SERVICE coroutine (the async-log
    // runner pattern) parked on a channel, started BEFORE any request.
    $svc = new Channel(8);
    go(function () use ($svc) {
        while (true) {
            if ($svc->pop(5.0) === 'stop') return;
        }
    });

    // ── Request root (claims, like the framework does per request) ──
    $done = new Channel(1);
    go(function () use ($yield, $svc, $done, &$fail) {
        zealphp_superglobals_owner();

        $g = new stdClass();
        $g->server = ['REQUEST_URI' => '/test'];
        $GLOBALS['g37'] = $g;

        // SHAPE A: fire-and-forget child — its first yield must NOT save
        // the live table under its own key and reset it to baseline.
        go(function () use ($yield) { $yield(); });
        if (!isset($GLOBALS['g37']) || $GLOBALS['g37'] !== $g) {
            echo "FAIL shape-A: global stolen by go() child yield\n"; $fail++;
        }

        // SHAPE B: resume the service runner MID-REQUEST (channel push).
        // Its restore must not reset_to_parent over our live state.
        $svc->push('log-line');
        if (!isset($GLOBALS['g37']) || $GLOBALS['g37'] !== $g) {
            echo "FAIL shape-B: global wiped by service-runner resume\n"; $fail++;
        }

        // Round-trip our OWN yield — delta save/restore still works.
        $yield();
        if (!isset($GLOBALS['g37']) || !($GLOBALS['g37'] instanceof stdClass)
            || ($GLOBALS['g37']->server['REQUEST_URI'] ?? '') !== '/test') {
            echo "FAIL own-yield: delta not restored\n"; $fail++;
        }

        // Framework lifecycle: the per-request drain CoSessionManager runs
        // in its finally — releases this request's deltas + resets the live
        // table to baseline for the next request.
        zealphp_coroutine_globals_request_end();
        $done->push(1);
    });
    $done->pop(10.0);

    // ── Peer request root: must NOT see the first root's global ──
    $done2 = new Channel(1);
    go(function () use ($done2, &$fail) {
        zealphp_superglobals_owner();
        if (isset($GLOBALS['g37'])) {
            echo "FAIL peer: saw another request's global\n"; $fail++;
        }
        zealphp_coroutine_globals_request_end();
        $done2->push(1);
    });
    $done2->pop(10.0);

    $svc->push('stop');
    zealphp_coroutine_globals(false);
    echo $fail === 0 ? "OK\n" : "FAILURES: $fail\n";
});
?>
--EXPECT--
OK

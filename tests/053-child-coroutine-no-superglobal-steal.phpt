--TEST--
053: a go() child's yield must NOT steal the owner's live superglobals (zealphp#332 / #2 residual)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip per-coroutine superglobal isolation needs the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// The async-log / fire-and-forget pattern: a request handler spawns a child
// coroutine (`go()` + first yield). Pre-fix, the CHILD's on_yield snapshotted
// the PARENT's live superglobals under the CHILD's key and #15-cleared them —
// the parent (which never yielded, so nothing ever restores for it) continued
// with EMPTY superglobals: REQUEST_METHOD gone → 501 dispatch (zealphp#332),
// $_SESSION wiped → session-write loss (#2 residual). With ownership gating,
// only the OWNER's yields snapshot+clear; peer isolation is preserved by the
// owner's own yield. Results are collected and printed at the end so the
// output is deterministic regardless of scheduler interleaving.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

// Co::sleep() is float-or-int depending on the OpenSwoole build (PHP 8.4
// deprecates the implicit float→int conversion), so yields use the same
// Timer+Channel pattern as test 048 — portable across builds.
function zp053_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}

zealphp_coroutine_superglobals(true);

$results = [];
$done = new Channel(2);

Co::run(function () use (&$results, $done) {
    go(function () use (&$results, $done) { // the request root (owner)
        zealphp_superglobals_owner();
        $_SERVER  = ['REQUEST_METHOD' => 'GET', 'REQUEST_URI' => '/probe'];
        $_SESSION = ['n' => 1];

        go(function () { zp053_yield(5); }); // child: yields immediately

        // THE FIX: the child's yield must not have wiped the owner's state.
        $results['after-child'] =
            json_encode($_SERVER) . ' sess=' . json_encode($_SESSION);

        zp053_yield(20); // the OWNER's own yield: snapshot + clear (peer isolation)
        $results['after-own-yield'] =
            json_encode($_SERVER) . ' sess=' . json_encode($_SESSION);
        $done->push(1);
    });
    go(function () use (&$results, $done) { // a peer running while the owner is suspended
        zp053_yield(10);
        // must NOT see the owner's values
        $results['peer-mid'] = json_encode($_SERVER ?? []);
        $done->push(1);
    });
    $done->pop(5.0);
    $done->pop(5.0);
});

foreach (['after-child', 'peer-mid', 'after-own-yield'] as $k) {
    echo $k, ': ', $results[$k] ?? 'MISSING', "\n";
}

zealphp_coroutine_superglobals(false);
?>
--EXPECT--
after-child: {"REQUEST_METHOD":"GET","REQUEST_URI":"\/probe"} sess={"n":1}
peer-mid: []
after-own-yield: {"REQUEST_METHOD":"GET","REQUEST_URI":"\/probe"} sess={"n":1}

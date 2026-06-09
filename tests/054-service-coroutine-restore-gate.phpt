--TEST--
054: a service coroutine's stale (empty) snapshot must never clobber a live owner's superglobals (#32)
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
// #32 — the async-log-runner wipe: a LONG-LIVED service coroutine that
// accumulated an (empty) snapshot while nobody owned the live superglobals
// could RESTORE that snapshot mid-request — writing empty arrays over the
// owner's live $_SERVER/$_SESSION (observed as the coroutine-legacy
// session-write loss on PHP 8.4: write_close persisted [] over the file).
// The restore side is now ownership-gated: a snapshot only lands in the
// live slots when nobody owns them or its recorded owner IS the holder.
//
// Stress shape: service coroutines cycle yields (like the log runner's
// channel-pop loop) while sequential "requests" claim ownership, populate,
// yield mid-flight, and verify integrity at their own write_close point.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp054_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}

zealphp_coroutine_superglobals(true);

$corruptions = new OpenSwoole\Atomic(0);
$requests    = 30;
$doneReq     = new Channel(1);
$stopSvc     = new OpenSwoole\Atomic(0);
$svcDone     = new Channel(2);

Co::run(function () use ($corruptions, $requests, $doneReq, $stopSvc, $svcDone) {
    // Two service coroutines cycling like log runners. Their first yields
    // happen while owner==0 → they accumulate EMPTY snapshots, exactly the
    // #32 precondition.
    for ($s = 0; $s < 2; $s++) {
        go(function () use ($stopSvc, $svcDone) {
            while (!$stopSvc->get()) { zp054_yield(2); }
            $svcDone->push(1);
        });
    }

    go(function () use ($corruptions, $requests, $doneReq) {
        for ($r = 1; $r <= $requests; $r++) {
            zealphp_superglobals_owner();                       // request start
            $_SERVER  = ['REQUEST_METHOD' => 'GET', 'R' => $r];
            $_SESSION = ['n' => $r];
            zp054_yield(1);                                     // mid-request I/O yield
            go(function () { zp054_yield(1); });                // fire-and-forget child
            zp054_yield(3);                                     // more I/O — services interleave
            // write_close point: the live state must still be OURS.
            if (($_SESSION['n'] ?? null) !== $r
                || ($_SERVER['R'] ?? null) !== $r
                || ($_SERVER['REQUEST_METHOD'] ?? null) !== 'GET') {
                $corruptions->add(1);
            }
            zealphp_superglobals_clear();                       // request end
        }
        $doneReq->push(1);
    });

    $doneReq->pop(20.0);
    $stopSvc->set(1);
    $svcDone->pop(5.0);
    $svcDone->pop(5.0);
});

echo "corruptions: ", $corruptions->get(), " / ", $requests, "\n";
zealphp_coroutine_superglobals(false);
?>
--EXPECT--
corruptions: 0 / 30

--TEST--
067: ext#16 — same-name request constant must not read a peer's value after resume
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
?>
--FILE--
<?php
// ext#16: two coroutines define() the SAME constant name with different values.
// The buggy snapshot_restore() left a finished/peer coroutine's constant
// installed in EG(zend_constants) and DEFERRED this coroutine's own constant
// instead of re-installing it — so the resuming coroutine read the PEER's value
// (deterministic "X:peer-value"). The fix retains each coroutine's constants and
// re-installs (overwrite, no free) on every resume, so a coroutine always reads
// its OWN define()'d value.
//
// No owner is claimed -> the request-state gate passes every coroutine (legacy
// mode), so the constant snapshot save/restore runs for both A and B.
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

Coroutine::run(function () {
    // Install the per-coroutine yield/resume/close hooks (the constant snapshot
    // save/restore live there) + the define() tracker.
    zealphp_coroutine_superglobals(true);
    zealphp_define_hook(true);

    // Channel-based yield: parks the coroutine (on_yield -> constant save) then
    // resumes it (on_resume -> constant restore). Timer::after takes int ms.
    $yield = function (int $ms = 2): void {
        $ch = new Channel(1);
        Timer::after($ms, fn () => $ch->push(1));
        $ch->pop(2.0);
    };

    $releaseA = new Channel(1);
    $bDone    = new Channel(1);
    $result   = new Channel(2);

    // A: define its value, park until released, then read its OWN constant.
    go(function () use ($releaseA, $result) {
        define('ZP067_X', 'A-value');   // EG[X] = A
        $releaseA->pop();               // YIELD -> save(A) orphans X out of EG
        $result->push('A:' . constant('ZP067_X'));   // after restore(A)
    });

    // B: define a DIFFERENT value, yield+resume, read, then finish.
    go(function () use ($yield, $bDone, $result) {
        define('ZP067_X', 'B-value');   // X absent (A orphaned it) -> EG[X] = B
        $yield(2);                      // save(B) orphans; resume -> restore(B) re-installs B
        $result->push('B:' . constant('ZP067_X'));
        $bDone->push(1);                // B body ends -> on_close(B)
    });

    // Let B fully finish (and on_close run) BEFORE releasing A, so A's resume
    // hits the "name already in EG" restore branch (old code: A reads B's value).
    $bDone->pop();
    $yield(4);                          // let on_close(B) settle
    $releaseA->push(1);

    $r = [$result->pop(2.0), $result->pop(2.0)];
    sort($r);
    echo implode("\n", $r), "\n";
    zealphp_define_hook(false);
    echo "DONE\n";
});
?>
--EXPECT--
A:A-value
B:B-value
DONE

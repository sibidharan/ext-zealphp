--TEST--
074: define() isolation WITHOUT $GLOBALS isolation — a constant re-installed on an occupied-name resume and then cleared is freed exactly once at coroutine close
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
// Same interleaving as 067 (A resumes while B's same-name constant is still in
// EG -> the occupied restore path keeps A's struct in its snapshot "ptrs"), then
// A calls zealphp_constants_clear(), which moves that struct to the deferred
// list and must drop the now-stale snapshot via the cid->ptr bridge. The bridge
// used to be recorded only when per-coroutine $GLOBALS isolation was active, so
// with define isolation alone both snapshot_delete and the deferred drain freed
// it at close. Under the ASAN / valgrind CI this test is the guard: the
// expected output plus a clean sanitizer run means one free.
//
// $GLOBALS isolation is deliberately NOT enabled here.
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

Coroutine::run(function () {
    zealphp_coroutine_superglobals(true);
    zealphp_define_hook(true);

    $yield = function (int $ms = 2): void {
        $ch = new Channel(1);
        Timer::after($ms, fn () => $ch->push(1));
        $ch->pop(2.0);
    };

    $releaseA = new Channel(1);
    $bDone    = new Channel(1);
    $aDone    = new Channel(1);
    $result   = new Channel(2);

    go(function () use ($releaseA, $result, $aDone) {
        define('ZP074_X', 'A-value');
        $releaseA->pop();                       // yield; resumes on the occupied path
        $result->push('A:' . constant('ZP074_X'));
        zealphp_constants_clear();              // request end for A
        $result->push('A-cleared:' . var_export(defined('ZP074_X'), true));
        $aDone->push(1);
    });                                         // on_close(A): must free A's constant once

    go(function () use ($yield, $bDone) {
        define('ZP074_X', 'B-value');
        $yield(2);
        $bDone->push(1);                        // finishes without clearing -> B stays in EG
    });

    $bDone->pop();
    $yield(4);
    $releaseA->push(1);
    echo $result->pop(2.0), "\n";
    echo $result->pop(2.0), "\n";
    $aDone->pop(2.0);
    $yield(4);                                  // let on_close(A) run
    zealphp_define_hook(false);
    echo "DONE\n";
});
echo "exit-clean\n";
?>
--EXPECT--
A:A-value
A-cleared:false
DONE
exit-clean

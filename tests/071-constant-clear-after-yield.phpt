--TEST--
071: ext#59 — a request constant is removed from EG by clear() even after a yield (lane survives on_yield/on_resume)
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
// ext#59 regression. In coroutine-legacy, a request constant defined by a
// coroutine must be removed from EG(zend_constants) by the request-end
// zealphp_constants_clear() EVEN WHEN the coroutine yielded (I/O) between the
// define() and the clear(). If it isn't, the constant stays installed in the
// process-wide EG and every LATER request that guards with
// `if (!defined(X)) define(X, ...)` adopts the stale value — the 42%->97%
// cross-request leak.
//
// The bug: the per-coroutine "lane" of tracked names was refilled on resume
// using os_get_cid(), which is NOT reliable in on_resume (it is only reliable
// in on_yield / in-coroutine execution). So the real coroutine's lane stayed
// empty after the yield and clear() found nothing to remove. The fix keeps the
// lane owned by define()/save()/clear() (all reliable-cid contexts) and never
// refills it on resume (restore is keyed by the coroutine pointer instead).
//
// A second coroutine then confirms the cross-coroutine effect: after A defines,
// yields, and clears, B must see the constant ABSENT (define its own), not
// adopt A's value.
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

    $aResult = new Channel(1);
    $aCleared = new Channel(1);
    $bResult = new Channel(1);

    // A: define -> YIELD -> read own -> request-end clear -> assert gone.
    go(function () use ($yield, $aResult, $aCleared) {
        define('ZP071_X', 'A-value');
        $yield(2);                          // save+restore the lane across a yield
        $own = constant('ZP071_X');         // must still be our own value
        zealphp_constants_clear();          // must find the lane and remove from EG
        $afterClear = defined('ZP071_X');   // #59: stayed true (bug) / false (fixed)
        $aResult->push($own . '|' . ($afterClear ? 'STILL-DEFINED' : 'gone'));
        $aCleared->push(1);
    });

    // B: starts only AFTER A has defined+yielded+cleared. Must see it absent.
    $aCleared->pop();
    go(function () use ($bResult) {
        $seen = defined('ZP071_X') ? constant('ZP071_X') : 'absent';
        $bResult->push($seen);
    });

    echo "A: ", $aResult->pop(), "\n";
    echo "B sees: ", $bResult->pop(), "\n";
    zealphp_define_hook(false);
});
echo "done\n";
?>
--EXPECT--
A: A-value|gone
B sees: absent
done

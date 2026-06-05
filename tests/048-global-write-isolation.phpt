--TEST--
048: a `global $x` scalar write is isolated per coroutine — no peer leak (#10/#14)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip per-coroutine $GLOBALS isolation needs the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// #10/#14: a top-level `$baseline = "PARENT"` is materialised into EG(symbol_table)
// as an IS_INDIRECT bucket (a CV pointer). The per-coroutine $GLOBALS isolation
// walked these buckets WITHOUT IS_INDIRECT awareness — reset_to_parent copied the
// indirection wrapper instead of the value it points at, so a `global $baseline; $baseline = W`
// write was never reset → every concurrent reader saw the last writer's value.
// The fix derefs IS_INDIRECT to the real slot in all four globals walks.
//
// EVEN coroutines overwrite the global; ODD coroutines never touch it and must see
// the parent baseline ("PARENT"), never a peer's "W" — deterministically 0 leaks.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

$baseline = "PARENT";
zealphp_coroutine_globals(true);

$N = 20;
$leaks = new OpenSwoole\Atomic(0);

Co::run(function () use ($N, $leaks) {
    $wg = new Channel($N);
    for ($i = 0; $i < $N; $i++) {
        go(function () use ($i, $leaks, $wg) {
            if ($i % 2 === 0) {
                global $baseline;
                $baseline = "W$i";
            }
            $ch = new Channel(1);
            Timer::after(5, fn() => $ch->push(1));
            $ch->pop(2.0);
            if ($i % 2 === 1) {
                global $baseline;
                if (($baseline ?? "") !== "" && $baseline !== "PARENT") {
                    $leaks->add(1);
                }
            }
            $wg->push(1);
        });
    }
    for ($i = 0; $i < $N; $i++) $wg->pop(5.0);
});

echo "global-write leaks: ", $leaks->get(), "\n";
?>
--EXPECT--
global-write leaks: 0

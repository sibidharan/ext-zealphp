--TEST--
046: per-coroutine superglobal isolation — a coroutine never inherits a peer's $_SESSION (#15)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip cross-coroutine superglobal isolation needs the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// #15: the per-coroutine superglobal snapshot was non-removing — snapshot_save
// left a coroutine's $_SESSION live in the process-shared EG(symbol_table) while
// it was suspended, so the NEXT coroutine's snapshot_save captured it as its own
// → cross-coroutine session hijack. The fix resets the superglobals to empty
// after snapshotting (peers start clean), with a restore-side clear safety net.
//
// EVEN coroutines start their own session; ODD coroutines NEVER touch $_SESSION.
// After interleaving, an ODD coroutine must see an EMPTY session, and an EVEN one
// must still see ITS OWN — deterministically zero leaks under the fix.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

zealphp_coroutine_superglobals(true);

$N = 20;
$leaks = new OpenSwoole\Atomic(0);

Co::run(function () use ($N, $leaks) {
    $wg = new Channel($N);
    for ($i = 0; $i < $N; $i++) {
        go(function () use ($i, $leaks, $wg) {
            if ($i % 2 === 0) {
                $_SESSION = ['owner' => "co$i"];
            }
            $ch = new Channel(1);
            Timer::after(5, fn() => $ch->push(1));
            $ch->pop(2.0);
            if ($i % 2 === 0) {
                if (($_SESSION['owner'] ?? null) !== "co$i") $leaks->add(1);
            } else {
                if (!empty($_SESSION)) $leaks->add(1);
            }
            $wg->push(1);
        });
    }
    for ($i = 0; $i < $N; $i++) $wg->pop(5.0);
});

echo "session leaks: ", $leaks->get(), "\n";
?>
--EXPECT--
session leaks: 0

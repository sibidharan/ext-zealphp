--TEST--
047: per-coroutine $_GET survives a yield and stays isolated (#15 round-trip guard)
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
// #15 round-trip guard: the post-save reset must NOT break the proven contract
// that a coroutine's OWN superglobal survives its own yield (snapshot on yield,
// restore on resume). Each coroutine sets a distinct $_GET, yields, and must see
// EXACTLY its own value back — proving the reset+restore preserves the round-trip
// AND isolates concurrent coroutines.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

zealphp_coroutine_superglobals(true);

$N = 16;
$ok = new OpenSwoole\Atomic(0);

Co::run(function () use ($N, $ok) {
    $wg = new Channel($N);
    for ($i = 0; $i < $N; $i++) {
        go(function () use ($i, $ok, $wg) {
            $_GET = ['id' => "req$i"];
            $ch = new Channel(1);
            Timer::after(5, fn() => $ch->push(1));
            $ch->pop(2.0);                       // yield — peers run + interleave
            if (($_GET['id'] ?? null) === "req$i") $ok->add(1);   // our own value back
            $wg->push(1);
        });
    }
    for ($i = 0; $i < $N; $i++) $wg->pop(5.0);
});

echo "roundtrip ok: ", $ok->get(), "/", $N, "\n";
?>
--EXPECT--
roundtrip ok: 16/16

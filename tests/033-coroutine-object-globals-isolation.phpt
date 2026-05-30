--TEST--
0.3.23: object-valued $GLOBALS isolate per coroutine + destruct in coroutine context (request-end drain)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
// Object-global isolation rides the OpenSwoole on_yield/on_resume/on_close
// scheduler hooks + a real request-end drain — it needs the coroutine runtime.
// (The no-op-when-inactive path is covered by 032 without OpenSwoole.)
if (!extension_loaded('openswoole')) {
    die("skip object-global isolation needs the OpenSwoole coroutine runtime");
}
?>
--FILE--
<?php
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

zealphp_coroutine_globals(true);   // activate per-coroutine $GLOBALS isolation

// Object whose __destruct does I/O (yields) — the $wpdb-closing-MySQL shape.
// It is ONLY safe to destruct in coroutine context; the request-end drain
// guarantees that, so this must never emit "API must be called in the coroutine".
class GObj {
    public string $v;
    public function __construct(string $x) { $this->v = $x; }
    public function __destruct() {
        $ch = new Channel(1);
        Timer::after(1, fn() => $ch->push(1));
        $ch->pop(0.5);   // YIELD inside __destruct
    }
}

$N = 20;
$leaks = new OpenSwoole\Atomic(0);

Co::run(function () use ($N, $leaks) {
    $wg = new Channel($N);
    for ($i = 0; $i < $N; $i++) {
        go(function () use ($i, $leaks, $wg) {
            $x = "co$i";
            global $g;
            $g = new GObj($x);                 // object-valued global
            $ch = new Channel(1);
            Timer::after(5, fn() => $ch->push(1));
            $ch->pop(2.0);                     // yield — interleave with peers
            if (($g->v ?? null) !== $x) {      // must still be THIS coroutine's
                $leaks->add(1);
            }
            zealphp_coroutine_globals_request_end(); // destruct $g IN coroutine
            $wg->push(1);
        });
    }
    for ($i = 0; $i < $N; $i++) $wg->pop(5.0);
});

echo "object-global leaks: ", $leaks->get(), "\n";
echo "ok\n";
?>
--EXPECT--
object-global leaks: 0
ok

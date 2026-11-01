--TEST--
055: `global $a, $b;` isolates EVERY name of the statement per coroutine (ZEND_BIND_GLOBAL is a repeatable opcode; the write hook must record the whole run, not only the first name)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
?>
--FILE--
<?php
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;

Coroutine::run(function () {
    zealphp_coroutine_globals(true);

    function set055(string $id): void {
        global $first055, $second055, $third055;
        $first055 = $id;
        $second055 = ['v' => $id];
        $third055 = (object) ['v' => $id];
    }
    function read055(): string {
        global $first055, $second055, $third055;
        Coroutine::usleep(2000);
        return $first055 . '/' . $second055['v'] . '/' . $third055->v;
    }

    $done = new Channel(2);
    foreach (['A', 'B'] as $id) {
        go(function () use ($id, $done) {
            set055($id);
            Coroutine::usleep(1000);
            $done->push($id . ':' . read055());
        });
    }
    $r = [$done->pop(3.0), $done->pop(3.0)];
    sort($r);
    echo implode("\n", $r), "\n";
});
?>
--EXPECT--
A:A/A/A
B:B/B/B

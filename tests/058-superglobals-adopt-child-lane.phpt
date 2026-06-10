--TEST--
058: zealphp_superglobals_adopt() — a go() child inherits the request's superglobals and keeps its OWN lane across its yields; the parent is never stolen from (#42)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
if (!function_exists('zealphp_superglobals_adopt')) die('skip no adopt');
?>
--FILE--
<?php
/* Shape: a "request" coroutine claims ownership (the framework's request
 * preamble), populates $_SERVER, spawns a child that adopts, both sides
 * yield twice, every read must see its own data. A second "request"
 * interleaves with different data to prove the lanes don't cross. */
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function tb_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn () => $ch->push(1));
    $ch->pop(3.0);
}

zealphp_coroutine_superglobals(true);

co::run(function () {
    $report = [];
    $joined = new Channel(2);

    $mkreq = function (string $tag, int $delayMs) use (&$report, $joined) {
        go(function () use ($tag, $delayMs, &$report, $joined) {
            zealphp_superglobals_owner();
            $_SERVER = ['REQ' => $tag, 'HTTP_HOST' => "$tag.example"];
            $_GET = ['x' => $tag];

            $childOut = new Channel(1);
            go(function () use ($tag, $delayMs, $childOut) {
                zealphp_superglobals_adopt();
                $pre = ($_SERVER['REQ'] ?? 'NIL') . '/' . ($_GET['x'] ?? 'NIL');
                tb_yield($delayMs);                  // child yield #1
                $mid = ($_SERVER['REQ'] ?? 'NIL') . '/' . ($_GET['x'] ?? 'NIL');
                tb_yield($delayMs);                  // child yield #2
                $post = ($_SERVER['REQ'] ?? 'NIL') . '/' . ($_GET['x'] ?? 'NIL');
                $childOut->push("child pre=$pre mid=$mid post=$post");
            });

            tb_yield($delayMs);                      // parent yield (interleave)
            $pmid = $_SERVER['REQ'] ?? 'NIL';
            $cline = $childOut->pop(5.0);
            tb_yield($delayMs);                      // parent yield after join
            $ppost = $_SERVER['REQ'] ?? 'NIL';
            $report[$tag] = "$tag: parent_mid=$pmid parent_post=$ppost | $cline";
            zealphp_superglobals_clear();
            $joined->push($tag);
        });
    };

    $mkreq('A', 20);
    $mkreq('B', 15);    // interleaves with A
    $joined->pop(8.0);
    $joined->pop(8.0);

    ksort($report);
    foreach ($report as $line) echo $line, "\n";
});
?>
--EXPECT--
A: parent_mid=A parent_post=A | child pre=A/A mid=A/A post=A/A
B: parent_mid=B parent_post=B | child pre=B/B mid=B/B post=B/B

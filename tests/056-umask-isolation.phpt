--TEST--
056: per-coroutine umask() isolation — one coroutine's umask never leaks to peers
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip per-coroutine umask isolation needs the OpenSwoole coroutine runtime');
}
?>
--FILE--
<?php
// umask() is process-global file-mode state: one request's umask(0077) used
// to change every peer's file creation mid-request. Same contract as the
// CWD/locale stages.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp056_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}

umask(0022); // baseline
zealphp_umask_isolation(true);

$results = [];
$done = new Channel(2);

Co::run(function () use (&$results, $done) {
    go(function () use (&$results, $done) {
        umask(0077);
        for ($i = 0; $i < 3; $i++) {
            zp056_yield(5);
            if (umask(0077) !== 0077) { $results['wrong'] = ($results['wrong'] ?? 0) + 1; }
        }
        $done->push(1);
    });
    go(function () use (&$results, $done) {
        for ($i = 0; $i < 4; $i++) {
            $cur = umask(0022);
            if ($cur !== 0022) { $results['leaks'] = ($results['leaks'] ?? 0) + 1; }
            zp056_yield(4);
        }
        $done->push(1);
    });
    $done->pop(5.0);
    $done->pop(5.0);
});

echo "changer wrong-umask resumes: ", $results['wrong'] ?? 0, "\n";
echo "bystander leaks: ", $results['leaks'] ?? 0, "\n";
$final = umask(0022);
echo "process re-parked: ", ($final === 0022 ? "yes" : sprintf("NO: %o", $final)), "\n";

zealphp_umask_isolation(false);
?>
--EXPECT--
changer wrong-umask resumes: 0
bystander leaks: 0
process re-parked: yes

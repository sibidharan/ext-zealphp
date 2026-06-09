--TEST--
055: per-coroutine setlocale() isolation — one coroutine's locale never leaks to peers
--EXTENSIONS--
zealphp
--SKIPIF--
<?php
if (!extension_loaded('openswoole')) {
    die('skip per-coroutine locale isolation needs the OpenSwoole coroutine runtime');
}
if (setlocale(LC_ALL, 'C.UTF-8') === false && setlocale(LC_ALL, 'en_US.UTF-8') === false) {
    die('skip no alternate locale available on this system');
}
setlocale(LC_ALL, 'C');
?>
--FILE--
<?php
// setlocale() is process-global: one request's locale change used to leak
// into every concurrently-running peer (number formatting, strtolower, …).
// With zealphp_locale_isolation(true): each coroutine that calls setlocale()
// gets ITS OWN locale back after every resume; peers and new coroutines see
// the baseline captured at enable time; the process is re-parked after all
// coroutines end.
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp055_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn() => $ch->push(1));
    $ch->pop(2.0);
}

setlocale(LC_ALL, 'C');
$alt = setlocale(LC_ALL, 'C.UTF-8') !== false ? 'C.UTF-8' : 'en_US.UTF-8';
setlocale(LC_ALL, 'C'); // baseline
zealphp_locale_isolation(true);

$results = [];
$done = new Channel(2);

Co::run(function () use (&$results, $done, $alt) {
    go(function () use (&$results, $done, $alt) {
        zealphp_superglobals_owner(); // the request-root claim the framework makes
        setlocale(LC_ALL, $alt);
        // #31-family pin: a fire-and-forget child's yield must not steal the
        // owner's locale (pre-0.3.39 it re-parked the parent to the baseline).
        go(function () { zp055_yield(2); });
        if (setlocale(LC_ALL, '0') !== $alt) { $results['wrong'] = ($results['wrong'] ?? 0) + 1; }
        for ($i = 0; $i < 3; $i++) {
            zp055_yield(5);
            if (setlocale(LC_ALL, '0') !== $alt) { $results['wrong'] = ($results['wrong'] ?? 0) + 1; }
        }
        $results['changer-final'] = setlocale(LC_ALL, '0');
        $done->push(1);
    });
    go(function () use (&$results, $done) {
        for ($i = 0; $i < 4; $i++) {
            if (setlocale(LC_ALL, '0') !== 'C') { $results['leaks'] = ($results['leaks'] ?? 0) + 1; }
            zp055_yield(4);
        }
        $done->push(1);
    });
    $done->pop(5.0);
    $done->pop(5.0);
});

echo "changer wrong-locale resumes: ", $results['wrong'] ?? 0, "\n";
echo "bystander leaks: ", $results['leaks'] ?? 0, "\n";
echo "changer kept its locale: ", ($results['changer-final'] === $alt ? "yes" : "NO: " . $results['changer-final']), "\n";
echo "process re-parked: ", (setlocale(LC_ALL, '0') === 'C' ? "yes" : "NO: " . setlocale(LC_ALL, '0')), "\n";

zealphp_locale_isolation(false);
?>
--EXPECT--
changer wrong-locale resumes: 0
bystander leaks: 0
changer kept its locale: yes
process re-parked: yes

--TEST--
059: per-coroutine timezone / mb-encoding / libxml-flag isolation — two interleaved requests each keep their own setting; service-coroutine close never re-parks a running request
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
if (!function_exists('zealphp_timezone_isolation')) die('skip no tz stage');
if (!function_exists('mb_internal_encoding')) die('skip mbstring absent');
if (!function_exists('libxml_use_internal_errors')) die('skip libxml absent');
?>
--FILE--
<?php
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

function zp59_yield(int $ms): void {
    $ch = new Channel(1);
    Timer::after($ms, static fn () => $ch->push(1));
    $ch->pop(3.0);
}

date_default_timezone_set('UTC');
mb_internal_encoding('UTF-8');
zealphp_coroutine_superglobals(true);
zealphp_timezone_isolation(true);
zealphp_mbenc_isolation(true);
zealphp_libxml_isolation(true);

co::run(function () {
    $report = [];
    $joined = new Channel(2);

    $mkreq = function (string $tag, string $tz, string $enc, bool $lib, int $ms) use (&$report, $joined) {
        go(function () use ($tag, $tz, $enc, $lib, $ms, &$report, $joined) {
            zealphp_superglobals_owner();
            date_default_timezone_set($tz);
            mb_internal_encoding($enc);
            libxml_use_internal_errors($lib);
            zp59_yield($ms);                       // yield #1 — interleave
            $mid = date_default_timezone_get() . '/' . mb_internal_encoding()
                 . '/' . var_export(libxml_use_internal_errors($lib), true);
            zp59_yield($ms);                       // yield #2
            $post = date_default_timezone_get() . '/' . mb_internal_encoding()
                  . '/' . var_export(libxml_use_internal_errors($lib), true);
            $report[$tag] = "$tag: mid=$mid post=$post";
            zealphp_superglobals_clear();
            $joined->push($tag);
        });
    };

    $mkreq('A', 'Asia/Kolkata', 'ISO-8859-1', true, 20);
    $mkreq('B', 'Europe/Berlin', 'ASCII', false, 15);
    $joined->pop(8.0);
    $joined->pop(8.0);

    ksort($report);
    foreach ($report as $line) echo $line, "\n";

    // The baselines must be re-parked after both requests ended.
    echo 'baseline: ', date_default_timezone_get(), '/', mb_internal_encoding(),
         '/', var_export(libxml_use_internal_errors(false), true), "\n";
});
?>
--EXPECT--
A: mid=Asia/Kolkata/ISO-8859-1/true post=Asia/Kolkata/ISO-8859-1/true
B: mid=Europe/Berlin/ASCII/false post=Europe/Berlin/ASCII/false
baseline: UTC/UTF-8/false

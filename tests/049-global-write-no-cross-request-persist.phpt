--TEST--
049: a `global $x` write resets to the parent baseline at request end — no cross-request persist (#10/#14)
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
// #10/#14 cross-request half: request 1 overwrites a materialised-CV global, then
// request_end() resets EG to the parent baseline. A sequential request 2 must see
// the baseline ("PARENT"), not request 1's write — proving the IS_INDIRECT-aware
// reset clears the write at the request boundary (not just on concurrent yields).
use OpenSwoole\Coroutine as Co;
use OpenSwoole\Coroutine\Channel;

$baseline = "PARENT";
zealphp_coroutine_globals(true);

Co::run(function () {
    $done = new Channel(1);
    go(function () use ($done) {
        global $baseline;
        $baseline = "REQ1WRITE";
        zealphp_coroutine_globals_request_end();   // framework's per-request reset
        $done->push(1);
    });
    $done->pop();                                   // request 1 fully done
    go(function () {
        global $baseline;
        echo "req2 sees: ", var_export($baseline ?? "UNDEF", true), "\n";
    });
});
?>
--EXPECT--
req2 sees: 'PARENT'

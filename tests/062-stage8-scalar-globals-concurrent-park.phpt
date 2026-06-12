--TEST--
062: ext#52 — concurrent Stage-8 requests must not corrupt each other's request-frame globals (ALL types parked per yield, not just objects)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
?>
--FILE--
<?php
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

// ext#52: the engine's zend_attach/detach_symbol_table protocol MOVES values
// between frame CVs with no refcount change, assuming a single flow of
// control. Two concurrent zealphp_require_global() requests sharing
// EG(symbol_table) hijack each other's INDIRECT buckets; the loser's next
// write to its stale CV then over-frees a payload it no longer owns (DokuWiki
// $config_group, ASAN-pinned). v0.3.47 parked IS_OBJECT request-frame globals
// only; this pins the 0.3.49 generalization: strings, arrays, ints, and
// `global`-bound REF slots all park per yield and restore per resume.

Coroutine::run(function () {
    zealphp_coroutine_globals(true);

    // Plain functions (process-global, immune to the $GLOBALS isolation the
    // test itself enables): a deprecation-safe yield + the REF leg's
    // function-level `global` bind against a request-frame CV.
    function yield_062(int $ms = 2): void {
        $ch = new Channel(1);
        Timer::after($ms, fn () => $ch->push(1));
        $ch->pop(2.0);
    }
    function refwrite_062() {
        global $p_str;
        $p_str .= '+ref';
    }

    // Two files, SAME top-level var names (the attack surface), different values.
    $mk = function (string $id, int $n) {
        $f = __DIR__ . "/062_req_$id.php";
        file_put_contents($f, "<?php\n"
            . "\$p_str = 'str-$id';\n"
            . "\$p_arr = ['k' => 'v-$id'];\n"
            . "\$p_int = $n;\n"
            . "yield_062();\n"                           // yield #1: peer attaches over our names
            . "refwrite_062();\n"                        // REF bind on the request-frame CV
            . "yield_062();\n"                           // yield #2: REF-wrapped slot must park too
            . "return \$p_str . '|' . \$p_arr['k'] . '|' . \$p_int;\n");
        return $f;
    };
    $fa = $mk('A', 7);
    $fb = $mk('B', 9);

    $done = new Channel(2);
    go(function () use ($fa, $done) {
        zealphp_superglobals_owner();
        $done->push('A=' . zealphp_require_global($fa));
    });
    go(function () use ($fb, $done) {
        zealphp_superglobals_owner();
        $done->push('B=' . zealphp_require_global($fb));
    });

    $r = [$done->pop(5.0), $done->pop(5.0)];
    sort($r);
    echo implode("\n", $r), "\n";

    @unlink($fa);
    @unlink($fb);
    echo "DONE\n";
});
?>
--EXPECT--
A=str-A+ref|v-A|7
B=str-B+ref|v-B|9
DONE

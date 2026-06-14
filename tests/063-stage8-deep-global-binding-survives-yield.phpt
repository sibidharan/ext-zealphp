--TEST--
063: $wpdb-null fix — a deep-frame `global $obj`/`global $arr` binding sees the live object/array global across yields under concurrent Stage-8 (not an orphaned-ref NULL)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
?>
--FILE--
<?php
// Regression pin for the $wpdb-null race (snapshot_restore Phase R). Under
// coroutine-legacy two concurrent Stage-8 requests share EG(symbol_table); the
// engine's attach/detach re-attach (assuming a single flow of control) can hand
// the bucket a DIFFERENT canonical zend_reference than a DEEP frame bound via
// `global $K` earlier — leaving that frame an orphaned ref reading NULL while
// $GLOBALS[$K] still reads the live value. WordPress hit this as
// "$wpdb->prepare() on null" (object global) and "array_pop(): null given"
// ($wp_filter array global). Phase R converges stale deep-frame `global`
// bindings onto the live canonical on every resume. This mirrors test 062's
// concurrent-Stage-8 harness but for the deep-frame OBJECT + ARRAY global read.
use OpenSwoole\Coroutine;
use OpenSwoole\Coroutine\Channel;
use OpenSwoole\Timer;

Coroutine::run(function () {
    zealphp_coroutine_globals(true);

    class Conn063 {
        public function __construct(public string $tag) {}
        public function q(): string { return $this->tag; }
    }

    function yield063(int $ms = 2): void {
        $ch = new Channel(1);
        Timer::after($ms, fn () => $ch->push(1));
        $ch->pop(2.0);
    }

    // require_wp_db() analog — sets an OBJECT and an ARRAY global in a function
    // via `global` (exactly how WP sets $wpdb / $wp_filter).
    function setup063(string $id): void {
        global $conn063, $filters063;
        $conn063 = new Conn063($id);
        $filters063 = ['hook' => $id];
    }

    // get_posts() analog — a DEEP function binds the globals via `global`, yields
    // (peer interleaves and the shared-table re-attach can orphan our refs), then
    // reads. The values MUST be this request's own live object/array, never NULL.
    function deep063(): string {
        global $conn063, $filters063;
        yield063();
        $o = is_object($conn063) ? $conn063->q() : 'NULL';
        $a = is_array($filters063) ? $filters063['hook'] : 'NULL';
        return "$o/$a";
    }

    $mk = function (string $id) {
        $f = __DIR__ . "/063_req_$id.php";
        file_put_contents($f, "<?php\n"
            . "setup063('$id');\n"
            . "yield063();\n"
            . "return deep063();\n");
        return $f;
    };
    $fa = $mk('A');
    $fb = $mk('B');

    $done = new Channel(2);
    go(function () use ($fa, $done) {
        zealphp_superglobals_owner();
        $done->push('A:' . zealphp_require_global($fa));
    });
    go(function () use ($fb, $done) {
        zealphp_superglobals_owner();
        $done->push('B:' . zealphp_require_global($fb));
    });
    $r = [$done->pop(3.0), $done->pop(3.0)];
    sort($r);
    echo implode("\n", $r), "\n";
    @unlink($fa);
    @unlink($fb);
});
?>
--EXPECT--
A:A/A
B:B/B

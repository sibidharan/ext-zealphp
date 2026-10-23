--TEST--
095: by default a go() child shares its request's $GLOBALS and class statics across both coroutines' yields, and no peer request sees them (F12, sharing mode)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
// The child used to share only its first slice: after its first yield it ran
// on the baseline (its request's globals gone) and its writes were never
// parked, so the request never saw them.
use OpenSwoole\Coroutine as Co;
zealphp_coroutine_globals(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        eval('class Zp095Post { public static $v = "tmpl"; }');
        $GLOBALS['zp095_r'] = 'root1';
        Zp095Post::$v = 'root1';
        go(function () {
            echo 'child 1: r=', $GLOBALS['zp095_r'] ?? '-', ' post=', Zp095Post::$v, "\n";
            $GLOBALS['zp095_c'] = 'child1';
            Co::usleep(10000);
            echo 'child 2: r=', $GLOBALS['zp095_r'] ?? '-', ' c=', $GLOBALS['zp095_c'] ?? '-', ' post=', Zp095Post::$v, "\n";
            $GLOBALS['zp095_c'] = 'child2';
            Zp095Post::$v = 'child2';
            Co::usleep(20000);
            echo 'child 3: r=', $GLOBALS['zp095_r'] ?? '-', ' c=', $GLOBALS['zp095_c'] ?? '-', "\n";
        });
        echo 'request after go: c=', $GLOBALS['zp095_c'] ?? '-', "\n";
        $GLOBALS['zp095_r'] = 'root2';
        Co::usleep(20000);
        echo 'request 2: r=', $GLOBALS['zp095_r'] ?? '-', ' c=', $GLOBALS['zp095_c'] ?? '-', ' post=', Zp095Post::$v, "\n";
        $GLOBALS['zp095_r'] = 'root3';
        Co::usleep(30000);
        zealphp_coroutine_globals_request_end();
    });
    Co::usleep(5000);
    go(function () {
        zealphp_superglobals_owner();
        echo 'peer: r=', $GLOBALS['zp095_r'] ?? '-', ' c=', $GLOBALS['zp095_c'] ?? '-', "\n";
        Co::usleep(1000);
        echo 'peer after yield: r=', $GLOBALS['zp095_r'] ?? '-', ' c=', $GLOBALS['zp095_c'] ?? '-', "\n";
        zealphp_coroutine_globals_request_end();
    });
});
zealphp_coroutine_globals(false);
echo "done\n";
?>
--EXPECT--
child 1: r=root1 post=root1
request after go: c=child1
peer: r=- c=-
peer after yield: r=- c=-
child 2: r=root2 c=child1 post=root1
request 2: r=root2 c=child2 post=child2
child 3: r=root3 c=child2
done

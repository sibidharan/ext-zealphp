--TEST--
096: zealphp_child_coroutine_isolation(true) gives each go() child its own $GLOBALS / class-static lane seeded from its creator's view; neither side sees the other's later writes (F12, opt-in)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
use OpenSwoole\Coroutine as Co;
var_dump(zealphp_child_coroutine_isolation(true));   // previous setting
zealphp_coroutine_globals(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        eval('class Zp096Post { public static $v = "tmpl"; }');
        $GLOBALS['zp096_r'] = 'root1';
        Zp096Post::$v = 'root1';
        go(function () {
            echo 'child 1: r=', $GLOBALS['zp096_r'] ?? '-', ' post=', Zp096Post::$v, "\n";
            $GLOBALS['zp096_c'] = 'child1';
            Zp096Post::$v = 'child1';
            Co::usleep(10000);
            echo 'child 2: r=', $GLOBALS['zp096_r'] ?? '-', ' c=', $GLOBALS['zp096_c'] ?? '-', ' post=', Zp096Post::$v, "\n";
            $GLOBALS['zp096_c'] = 'child2';
            Co::usleep(20000);
            echo 'child 3: r=', $GLOBALS['zp096_r'] ?? '-', ' c=', $GLOBALS['zp096_c'] ?? '-', "\n";
        });
        echo 'request after go: c=', $GLOBALS['zp096_c'] ?? '-', ' post=', Zp096Post::$v, "\n";
        $GLOBALS['zp096_r'] = 'root2';
        Co::usleep(20000);
        echo 'request 2: r=', $GLOBALS['zp096_r'] ?? '-', ' c=', $GLOBALS['zp096_c'] ?? '-', ' post=', Zp096Post::$v, "\n";
        Co::usleep(30000);
        echo 'request 3: c=', $GLOBALS['zp096_c'] ?? '-', "\n";
        zealphp_coroutine_globals_request_end();
    });
});
zealphp_coroutine_globals(false);
var_dump(zealphp_child_coroutine_isolation(false));
echo "done\n";
?>
--EXPECT--
bool(false)
child 1: r=root1 post=root1
request after go: c=- post=root1
child 2: r=root1 c=child1 post=child1
request 2: r=root2 c=- post=root1
child 3: r=root1 c=child2
request 3: c=-
bool(true)
done

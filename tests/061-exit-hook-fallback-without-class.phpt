--TEST--
061: zealphp_exit_hook() — with ZealPHP\HaltException NOT loaded, exit() falls through to OpenSwoole's ExitException (no autoload from the exit site) (ext#47)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
if (!function_exists('zealphp_exit_hook')) die('skip no exit hook');
?>
--FILE--
<?php
var_dump(zealphp_exit_hook(true));

co::run(function () {
    try {
        exit("legacy");
    } catch (\OpenSwoole\ExitException $e) {
        echo "openswoole-exit status=", var_export($e->getStatus(), true), "\n";
    }
});
echo "done\n";
?>
--EXPECT--
bool(true)
openswoole-exit status='legacy'
done

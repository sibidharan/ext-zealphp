--TEST--
061: zealphp_exit_hook() — with ZealPHP\HaltException NOT loaded, exit() falls through to OpenSwoole's ExitException (no autoload from the exit site) (ext#47)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
if (!function_exists('zealphp_exit_hook')) die('skip no exit hook');
/* Delegation hands exit() to whatever THIS OpenSwoole build does. Newer
 * builds intercept exit-in-coroutine (throw ExitException); older ones
 * real-exit — also correct delegation, but unassertable in one EXPECT.
 * Probe in a subprocess (an in-SKIPIF probe would real-exit the SKIPIF). */
$deps = '';
foreach (['sockets', 'curl'] as $dep) {           /* openswoole link deps (socket_ce/curl_ce) */
    if (extension_loaded($dep)) {
        $deps .= ' -d extension=' . $dep . '.so';
    }
}
$probe = shell_exec(
    escapeshellarg(PHP_BINARY)
    . ' -n -d extension_dir=' . escapeshellarg((string)ini_get('extension_dir'))
    . $deps
    . ' -d extension=openswoole.so -r '
    . escapeshellarg('co::run(function () { try { exit(7); } catch (\Throwable $e) { echo get_class($e); } });')
    . ' 2>/dev/null'
);
if (!is_string($probe) || strpos($probe, 'ExitException') === false) {
    die('skip this OpenSwoole build does not intercept exit() in coroutines'
        . ' (delegation = real exit; that leg is covered by test 060\'s tail)');
}
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

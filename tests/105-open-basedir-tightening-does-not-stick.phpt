--TEST--
105: a request that tightens open_basedir keeps it for itself, but its peers and the next request get the worker's value back (B11)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--INI--
open_basedir=
--FILE--
<?php
// open_basedir can only be narrowed at runtime stage, so re-parking the
// worker's (wider) value on yield, and zealphp_ini_restore() at request end,
// both failed silently: one request's tightening applied to every request.
use OpenSwoole\Coroutine as Co;
$narrow = realpath(sys_get_temp_dir());
zealphp_coroutine_superglobals(true);
Co::run(function () use ($narrow) {
    go(function () use ($narrow) {
        zealphp_superglobals_owner();
        ini_set('open_basedir', $narrow);
        Co::usleep(5000);
        echo 'A after yield: ', ini_get('open_basedir') === $narrow ? 'narrowed' : var_export(ini_get('open_basedir'), true), "\n";
        zealphp_ini_restore();
        zealphp_superglobals_clear();
    });
    go(function () {
        zealphp_superglobals_owner();
        echo 'peer: ', var_export(ini_get('open_basedir'), true), ' etc readable: ', @is_readable('/etc/hostname') ? 'yes' : 'no', "\n";
        zealphp_superglobals_clear();
    });
});
echo 'after the request: ', var_export(ini_get('open_basedir'), true), "\n";
?>
--EXPECT--
peer: '' etc readable: yes
A after yield: narrowed
after the request: ''

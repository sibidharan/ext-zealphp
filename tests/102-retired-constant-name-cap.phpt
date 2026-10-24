--TEST--
102: the retired request-constant pool stops growing at 4096 distinct names per worker; past it new names are left process-wide and the cap is logged once (B9)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--INI--
log_errors=1
error_log=
--FILE--
<?php
// Each distinct name a request define()s and clears is retired for the
// worker's life (a stale run_time_cache slot may still point at it). With
// input-derived names (define('LANG_' . $_GET['l'])) that grew without bound.
use OpenSwoole\Coroutine as Co;
zealphp_coroutine_superglobals(true);
zealphp_define_hook(true);
Co::run(function () {
    for ($i = 0; $i < 4200; $i++) {
        go(function () use ($i) {
            define("ZP102_N$i", $i);
            zealphp_constants_clear();
        });
    }
});
$isolated = 0; $kept = 0;
for ($i = 0; $i < 4200; $i++) { defined("ZP102_N$i") ? $kept++ : $isolated++; }
echo "isolated: $isolated\n";
echo "left process-wide: $kept\n";
Co::run(function () {               // a name already in the pool still isolates
    go(function () { define('ZP102_N7', 'again'); zealphp_constants_clear(); });
});
echo 'pooled name isolated: ', defined('ZP102_N7') ? 'no' : 'yes', "\n";
zealphp_define_hook(false);
?>
--EXPECTF--
%Aext-zealphp: request-constant pool reached its cap of 4096 distinct names in this worker; constants with new names are no longer request-isolated (define() names derived from request input?)
isolated: 4096
left process-wide: 104
pooled name isolated: yes

--TEST--
060: zealphp_exit_hook() — exit()/die() in a coroutine throws ZealPHP\HaltException (invisible to catch(\Exception)), status carried; real exit outside coroutines (ext#47)
--SKIPIF--
<?php
if (!extension_loaded('zealphp')) die('skip zealphp not loaded');
if (!extension_loaded('openswoole')) die('skip openswoole not loaded');
if (!function_exists('zealphp_exit_hook')) die('skip no exit hook');
?>
--FILE--
<?php
namespace ZealPHP {
    /* Stub of the framework class (src/HaltException.php): extends \Error BY
     * DESIGN so catch(\Exception) cannot swallow a halt. */
    class HaltException extends \Error
    {
        public mixed $status = null;
    }
}
namespace {

var_dump(zealphp_exit_hook(true));

co::run(function () {
    /* 1) The bug being fixed: the Apache-migration idiom
     *    try { header(...); exit; } catch (\Exception) used to catch
     *    OpenSwoole\ExitException and turn a normal exit into an error. */
    try {
        try {
            exit("redirected");
        } catch (\Exception $e) {
            echo "SWALLOWED-BY-EXCEPTION\n";
        }
        echo "FELL-THROUGH\n";
    } catch (\ZealPHP\HaltException $h) {
        echo "halt1: ", get_class($h), " status=", var_export($h->status, true), "\n";
    }

    /* 2) integer status */
    try {
        exit(3);
    } catch (\ZealPHP\HaltException $h) {
        echo "halt2: status=", var_export($h->status, true), "\n";
    }

    /* 3) die() routes identically */
    try {
        die("gone");
    } catch (\ZealPHP\HaltException $h) {
        echo "halt3: status=", var_export($h->status, true), "\n";
    }

    /* 4) bare exit; */
    try {
        exit;
    } catch (\ZealPHP\HaltException $h) {
        echo "halt4: status=", var_export($h->status, true), "\n";
    }
});

/* 5) outside any coroutine the hook DELEGATES — on plain CLI exit() must
 * really terminate (the worker-survival contract only applies in-coroutine). */
echo "pre-real-exit\n";
exit(0);
echo "NOT-REACHED\n";

}
?>
--EXPECT--
bool(true)
halt1: ZealPHP\HaltException status='redirected'
halt2: status=3
halt3: status='gone'
halt4: status=0
pre-real-exit

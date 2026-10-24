--TEST--
094: a $_SESSION object whose last reference is the superglobal snapshot is not destructed in on_close (B6)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--FILE--
<?php
// The snapshot taken at a yield held $_SESSION['svc']; the request then
// replaced $_SESSION and ended without yielding, so on_close freed the last
// reference: __destruct ran outside any coroutine (and its I/O yielded out of
// the close callback — a SIGSEGV on 0.4.0).
use OpenSwoole\Coroutine as Co;
class Zp094Service {
    public function __destruct() {
        $in = Co::getCid() > 0 ? 'coroutine' : 'callback';
        Co::usleep(500);
        echo "service closed in: {$in}\n";
    }
}
zealphp_coroutine_superglobals(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        $_SESSION = ['svc' => new Zp094Service()];
        Co::usleep(2000);
        echo 'resumed with svc: ', isset($_SESSION['svc']) ? 'yes' : 'no', "\n";
        $_SESSION = [];
        echo "request 1 done\n";
    });
    Co::usleep(5000);
    go(function () {             // the next request: the release runs here
        zealphp_superglobals_owner();
        for ($i = 0; $i < 3; $i++) { Co::usleep(500); }
        zealphp_superglobals_clear();
        echo "request 2 done\n";
    });
});
echo "survived\n";
?>
--EXPECT--
resumed with svc: yes
request 1 done
service closed in: coroutine
request 2 done
survived

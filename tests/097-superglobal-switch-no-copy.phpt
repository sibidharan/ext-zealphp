--TEST--
097: a coroutine switch neither copies a large $_POST / $_REQUEST nor grows memory per yield (B3)
--EXTENSIONS--
zealphp
--SKIPIF--
<?php if (!extension_loaded('openswoole')) die('skip needs the OpenSwoole coroutine runtime'); ?>
--INI--
memory_limit=256M
--FILE--
<?php
// The superglobal snapshot used to deep-copy every superglobal on yield and
// again on resume (50k fields: ~2 x 2.5 MB per switch, with memory_limit
// lifted). Snapshot and live slot now share the arrays copy-on-write.
use OpenSwoole\Coroutine as Co;
zealphp_coroutine_superglobals(true);
Co::run(function () {
    go(function () {
        zealphp_superglobals_owner();
        $post = [];
        for ($i = 0; $i < 50000; $i++) { $post["f$i"] = "v$i"; }
        zealphp_superglobals_set([], $post, [], ['REQUEST_URI' => '/'], [], $post, []);
        unset($post);
        Co::usleep(100);                       // settle: first snapshot exists
        $base = memory_get_usage();
        $peak0 = memory_get_peak_usage();
        for ($k = 0; $k < 30; $k++) { Co::usleep(100); }
        $grow = memory_get_usage() - $base;
        $peak = memory_get_peak_usage() - $peak0;
        echo 'fields: ', count($_POST), ' ', count($_REQUEST), "\n";
        echo 'no growth across 30 yields: ', $grow < 256 * 1024 ? 'yes' : "no ($grow)", "\n";
        echo 'no copy at the switch: ', $peak < 256 * 1024 ? 'yes' : "no (+$peak peak)", "\n";
        $_POST['f1'] = 'changed';              // copy-on-write still isolates writes
        Co::usleep(100);
        echo 'own write survives: ', $_POST['f1'], ' / request copy untouched: ', $_REQUEST['f1'], "\n";
    });
});
?>
--EXPECT--
fields: 50000 50000
no growth across 30 yields: yes
no copy at the switch: yes
own write survives: changed / request copy untouched: v1

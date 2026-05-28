--TEST--
Regression: non-empty $_REQUEST + $_SESSION through zealphp_superglobals_set() must not segfault (PG(http_globals)[6] OOB fix)
--EXTENSIONS--
zealphp
--FILE--
<?php
// Regression for the fixed PG(http_globals)[6] out-of-bounds crash.
// zealphp_track_vars_index() used to return 6 (TRACK_VARS_REQUEST) for
// "_REQUEST", but PG(http_globals) is a 6-slot array (indices 0-5). Writing
// to / dtor-ing slot 6 read past the end of the struct. The crash site was
// the "existing" branch in zealphp_set_superglobal: the SECOND set call hit
// the zval_ptr_dtor(&PG(http_globals)[6]) path on the OOB slot.
//
// The fix returns -1 for _REQUEST (and _SESSION/_ENV) — symbol-table only.
// This test merely COMPLETING without SIGSEGV, plus correct reads, is the
// guarantee. Fully CLI-runnable (no OpenSwoole needed).

// First set — NON-EMPTY _REQUEST (6th arg) and _SESSION (7th arg).
zealphp_superglobals_set(
    ['g' => '1'],
    ['p' => '2'],
    ['c' => '3'],
    ['METHOD' => 'POST'],
    [],
    ['k' => 'req-first'],
    ['k' => 'sess-first']
);

echo $_REQUEST['k'] . "\n";
echo $_SESSION['k'] . "\n";

// Second set with a DIFFERENT non-empty _REQUEST/_SESSION. This drives the
// "existing" dtor branch in zealphp_set_superglobal that previously hit the
// OOB slot 6 dtor — the actual crash site.
zealphp_superglobals_set(
    ['g' => '10'],
    ['p' => '20'],
    ['c' => '30'],
    ['METHOD' => 'GET'],
    [],
    ['k' => 'req-second'],
    ['k' => 'sess-second']
);

echo $_REQUEST['k'] . "\n";
echo $_SESSION['k'] . "\n";

// Clear, then confirm _REQUEST is emptied.
zealphp_superglobals_clear();
echo empty($_REQUEST) ? "request-cleared" : "request-NOT-cleared";
echo "\n";
echo "done\n";
?>
--EXPECT--
req-first
sess-first
req-second
sess-second
request-cleared
done

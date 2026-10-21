--TEST--
073: zealphp_superglobals_set() writes THROUGH a reference-bound superglobal slot — a held alias sees the new request's array, never the previous one
--EXTENSIONS--
zealphp
--FILE--
<?php
// A framework accessor that hands out `&$GLOBALS['_GET']` (a $g->get-style
// alias, bound via &__get) turns the symbol-table slot into an IS_REFERENCE.
// zealphp_superglobals_set() must update the value the reference points at
// (as zealphp_snapshot_restore already does), not replace the reference
// wrapper — otherwise the alias keeps the PREVIOUS request's array.
zealphp_superglobals_set(['req' => 'one'], [], [], [], [], [], []);
$alias = &$GLOBALS['_GET'];
var_dump($alias['req']);

// request 2 in the same worker
zealphp_superglobals_set(['req' => 'two'], [], [], [], [], [], []);
var_dump($_GET['req']);
var_dump($alias['req']);           // must be "two", not the previous request's "one"

// writes through the alias stay visible via $_GET (still one slot)
$alias['extra'] = 'x';
var_dump($_GET['extra']);

zealphp_superglobals_clear();
var_dump(empty($alias), empty($_GET));
?>
--EXPECT--
string(3) "one"
string(3) "two"
string(3) "two"
string(1) "x"
bool(true)
bool(true)

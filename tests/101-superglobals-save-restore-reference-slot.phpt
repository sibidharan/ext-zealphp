--TEST--
101: zealphp_superglobals_save()/restore() on a superglobal slot bound by reference stores the array, never a reference inside the reference (B8)
--EXTENSIONS--
zealphp
--FILE--
<?php
// save() copied the slot's IS_REFERENCE itself; restore() then wrote that
// reference INTO the referenced value - a reference to a reference ("unknown
// type" on read, then a crash).
$alias = &$GLOBALS['_GET'];          // the $g->get / &__get shape
$_GET = ['a' => 1];
$snap = zealphp_superglobals_save();
$_GET = ['b' => 2];
zealphp_superglobals_restore($snap);
echo gettype($_GET), "\n";
var_dump($alias);
$_GET['c'] = 3;
echo count($alias), "\n";
?>
--EXPECT--
array
array(1) {
  ["a"]=>
  int(1)
}
2

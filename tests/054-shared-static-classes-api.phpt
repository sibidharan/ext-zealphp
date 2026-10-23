--TEST--
zealphp_shared_static_classes(): registers lower-cased prefixes, replaces the list, returns the count
--SKIPIF--
<?php if (!extension_loaded('zealphp')) die('skip zealphp not loaded'); ?>
--FILE--
<?php
var_dump(zealphp_shared_static_classes(['ZealPHP\\', '\\Vendor\\Shared\\', '']));
var_dump(zealphp_shared_static_classes([]));
var_dump(zealphp_shared_static_classes(['App\\Framework\\']));
// The profiler API is always callable; counters are zero unless ZEALPHP_SWITCH_PROF=1.
$p = zealphp_switch_profile();
var_dump(isset($p['park_total'], $p['unpark_total'], $p['class_statics_save'], $p['globals_save']));
var_dump($p['enabled']);
var_dump($p['park_total']['calls']);
echo "done\n";
?>
--EXPECT--
int(2)
int(0)
int(1)
bool(true)
bool(false)
int(0)
done

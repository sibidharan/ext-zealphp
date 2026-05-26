--TEST--
PARANOID: $_SESSION writes before and after yield are isolated
--EXTENSIONS--
zealphp
--FILE--
<?php
// A writes to session, yields, B writes to session, A resumes and writes more
// Each coroutine's writes must be independent

// A starts
zealphp_superglobals_set([], [], [], [], [], [],
    []);
$_SESSION = ['items' => []];
$_SESSION['items'][] = 'A_item_1';

$snapA = zealphp_superglobals_save();

// B starts (A yielded)
zealphp_superglobals_set([], [], [], [], [], [],
    []);
$_SESSION = ['items' => []];
$_SESSION['items'][] = 'B_item_1';
$_SESSION['items'][] = 'B_item_2';

echo "B items: " . implode(',', $_SESSION['items']) . "\n";
$snapB = zealphp_superglobals_save();

// A resumes — must see only A's items
zealphp_superglobals_restore($snapA);
$_SESSION['items'][] = 'A_item_2';
echo "A items: " . implode(',', $_SESSION['items']) . "\n";
echo "A has B items: " . (in_array('B_item_1', $_SESSION['items']) ? "LEAKED!" : "no") . "\n";

$snapA = zealphp_superglobals_save();

// B resumes — must see only B's items
zealphp_superglobals_restore($snapB);
echo "B items: " . implode(',', $_SESSION['items']) . "\n";
echo "B has A items: " . (in_array('A_item_2', $_SESSION['items']) ? "LEAKED!" : "no") . "\n";

zealphp_superglobals_clear();
echo "PASS\n";
?>
--EXPECT--
B items: B_item_1,B_item_2
A items: A_item_1,A_item_2
A has B items: no
B items: B_item_1,B_item_2
B has A items: no
PASS

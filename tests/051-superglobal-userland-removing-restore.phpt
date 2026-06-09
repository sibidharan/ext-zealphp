--TEST--
051: zealphp_superglobals_restore() is removing — a snapshot-absent superglobal a peer populated is cleared, not leaked (#15 userland twin)
--EXTENSIONS--
zealphp
--FILE--
<?php
// The scheduler save/restore path (on_yield/on_resume) was made removing in
// 0.3.31, but the userland twin zealphp_superglobals_restore() still only WROTE
// snapshot-present slots — never EMPTYING a slot a peer populated that is ABSENT
// from the snapshot. So a coroutine that snapshotted with NO $_SESSION, then
// restored after a peer set a sensitive $_SESSION, read the peer's value (#15).

// Coroutine A: has $_GET[a]=A, NO $_SESSION at snapshot time.
zealphp_superglobals_set(['a' => 'A'], [], [], [], [], [], []);
unset($_SESSION);
$snapA = zealphp_superglobals_save();
var_dump(array_key_exists('_SESSION', $snapA)); // false — snapshot has no _SESSION

// Peer B runs in between and populates a sensitive $_SESSION.
$_SESSION = ['token' => 'sk_secretB', 'role' => 'admin'];

// A resumes via the userland restore. The snapshot-absent $_SESSION must be
// EMPTIED, not left as B's value.
zealphp_superglobals_restore($snapA);

echo 'token: ', ($_SESSION['token'] ?? '<empty>'), "\n";
echo 'role: ',  ($_SESSION['role']  ?? '<empty>'), "\n";
echo 'get a: ', ($_GET['a'] ?? '?'), "\n"; // A's own value still restored

zealphp_superglobals_clear();
?>
--EXPECT--
bool(false)
token: <empty>
role: <empty>
get a: A

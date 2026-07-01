<?php
// ext#59 CONTROL — same yield, no define(); probes putenv/ini_set/$GLOBALS/$_SERVER.
// Emits the same {me, const} shape so burst.py classifies both probes identically.
$me = $_GET['who'] ?? 'anon';
putenv("REQ_ENV=$me");
ini_set('user_agent', $me);
$GLOBALS['req_g'] = $me;
$_SERVER['REQ_MARK'] = $me;
usleep(3000);
$vals = [
    'env' => getenv('REQ_ENV'),
    'ini' => ini_get('user_agent'),
    'g'   => $GLOBALS['req_g'] ?? 'UNDEF',
    'srv' => $_SERVER['REQ_MARK'] ?? 'UNDEF',
];
$bad = array_diff($vals, [$me]);
$const = $bad === [] ? $me : (implode('|', array_unique($bad)) ?: 'UNDEF');
echo json_encode(['me' => $me, 'const' => $const, 'detail' => $bad === [] ? null : $vals]);

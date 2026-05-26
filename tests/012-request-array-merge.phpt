--TEST--
zealphp_superglobals: $_REQUEST reflects merged GET+POST+COOKIE
--EXTENSIONS--
zealphp
--FILE--
<?php
zealphp_superglobals_set(
    ['source' => 'get', 'shared' => 'from_get'],
    ['source' => 'post', 'body' => 'data'],
    ['source' => 'cookie'],
    [],
    [],
    ['source' => 'get', 'shared' => 'from_get', 'body' => 'data']
);

echo "REQUEST source: " . $_REQUEST['source'] . "\n";
echo "REQUEST shared: " . $_REQUEST['shared'] . "\n";
echo "REQUEST body: " . $_REQUEST['body'] . "\n";

zealphp_superglobals_clear();
echo "Cleared REQUEST: " . (empty($_REQUEST) ? "yes" : "no") . "\n";
?>
--EXPECT--
REQUEST source: get
REQUEST shared: from_get
REQUEST body: data
Cleared REQUEST: yes

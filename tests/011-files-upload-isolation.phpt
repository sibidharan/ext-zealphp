--TEST--
zealphp_superglobals: $_FILES isolation between requests
--EXTENSIONS--
zealphp
--FILE--
<?php
// Request with file upload
zealphp_superglobals_set(
    [],
    [],
    [],
    [],
    ['avatar' => ['name' => 'photo.jpg', 'size' => 12345, 'tmp_name' => '/tmp/php123']],
    [],
    []
);
echo "Has file: " . (isset($_FILES['avatar']) ? "yes" : "no") . "\n";
echo "File name: " . $_FILES['avatar']['name'] . "\n";

zealphp_superglobals_clear();

// Next request — no upload
zealphp_superglobals_set([], [], [], [], [], [], []);
echo "Leaked file: " . (isset($_FILES['avatar']) ? "LEAKED!" : "no") . "\n";
echo "FILES empty: " . (empty($_FILES) ? "yes" : "no") . "\n";

zealphp_superglobals_clear();
?>
--EXPECT--
Has file: yes
File name: photo.jpg
Leaked file: no
FILES empty: yes

--TEST--
nghttp2 extension loads and classes are available
--SKIPIF--
<?php
if (!extension_loaded('nghttp2')) {
    die('skip nghttp2 extension is not loaded');
}
?>
--FILE--
<?php
var_dump(extension_loaded('nghttp2'));
var_dump(class_exists('Nghttp2\\Hpack'));
var_dump(class_exists('Nghttp2\\Client'));
var_dump(class_exists('Nghttp2\\Exception\\HpackException'));
var_dump(class_exists('Nghttp2\\Exception\\ClientException'));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)

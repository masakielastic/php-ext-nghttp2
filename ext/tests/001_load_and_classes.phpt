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
var_dump(class_exists('Nghttp2\\Server'));
var_dump(class_exists('Nghttp2\\Session'));
var_dump(class_exists('Nghttp2\\Exception\\HpackException'));
var_dump(class_exists('Nghttp2\\Exception\\ClientException'));
var_dump(class_exists('Nghttp2\\Exception\\ServerException'));
var_dump(class_exists('Nghttp2\\Exception\\SessionException'));
var_dump(method_exists('Nghttp2\\Server', 'setResponse'));
var_dump(method_exists('Nghttp2\\Session', 'client'));
var_dump(method_exists('Nghttp2\\Session', 'popOutbound'));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)

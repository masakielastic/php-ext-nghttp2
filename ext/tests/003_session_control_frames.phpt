--TEST--
Nghttp2\Session submits control frames
--SKIPIF--
<?php
if (!extension_loaded('nghttp2')) {
    die('skip nghttp2 extension is not loaded');
}
?>
--FILE--
<?php
function pump(object $from, object $to): bool {
    $outbound = $from->popOutbound();
    if ($outbound === '') {
        return false;
    }

    $to->receive($outbound);
    return true;
}

function drain(object $left, object $right, int $maxRounds = 10): void {
    for ($i = 0; $i < $maxRounds; $i++) {
        $moved = false;
        $moved = pump($left, $right) || $moved;
        $moved = pump($right, $left) || $moved;

        if (!$moved) {
            break;
        }
    }
}

function newPair(): array {
    $client = Nghttp2\Session::client();
    $server = Nghttp2\Session::server();
    drain($client, $server);
    $client->popEvents();
    $server->popEvents();
    return [$client, $server];
}

[$client, $server] = newPair();
$client->submitSettings([
    ['id' => 3, 'value' => 50],
]);
drain($client, $server);
$serverEvents = $server->popEvents();
$clientEvents = $client->popEvents();
var_dump($serverEvents[0]['type']);
var_dump($serverEvents[0]['ack']);
var_dump($serverEvents[0]['settings'][0]['id']);
var_dump($serverEvents[0]['settings'][0]['value']);
var_dump($clientEvents[0]['type']);
var_dump($clientEvents[0]['ack']);

[$client, $server] = newPair();
$client->submitPing("12345678");
drain($client, $server);
$serverEvents = $server->popEvents();
$clientEvents = $client->popEvents();
var_dump($serverEvents[0]['type']);
var_dump($serverEvents[0]['ack']);
var_dump($serverEvents[0]['opaqueData']);
var_dump($clientEvents[0]['type']);
var_dump($clientEvents[0]['ack']);
var_dump($clientEvents[0]['opaqueData']);

[$client, $server] = newPair();
$streamId = $client->submitRequest([
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':scheme', 'value' => 'https'],
    ['name' => ':authority', 'value' => 'example.com'],
    ['name' => ':path', 'value' => '/rst'],
], true);
drain($client, $server);
$server->popEvents();
$server->submitRstStream($streamId, 8);
drain($server, $client);
$clientEvents = $client->popEvents();
var_dump($clientEvents[0]['type']);
var_dump($clientEvents[0]['streamId'] === $streamId);
var_dump($clientEvents[0]['errorCode']);
var_dump($clientEvents[1]['type']);
var_dump($clientEvents[1]['streamId'] === $streamId);
var_dump($clientEvents[1]['errorCode']);

[$client, $server] = newPair();
$server->submitGoaway(0, "bye");
drain($server, $client);
$clientEvents = $client->popEvents();
var_dump($clientEvents[0]['type']);
var_dump($clientEvents[0]['errorCode']);
var_dump($clientEvents[0]['debugData']);
?>
--EXPECT--
string(8) "settings"
bool(false)
int(3)
int(50)
string(8) "settings"
bool(true)
string(4) "ping"
bool(false)
string(8) "12345678"
string(4) "ping"
bool(true)
string(8) "12345678"
string(10) "rst_stream"
bool(true)
int(8)
string(12) "stream_close"
bool(true)
int(8)
string(6) "goaway"
int(0)
string(3) "bye"

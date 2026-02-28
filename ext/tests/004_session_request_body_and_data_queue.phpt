--TEST--
Nghttp2\Session supports request bodies and queued submitData calls
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

function drain(object $left, object $right, int $maxRounds = 20): void {
    for ($i = 0; $i < $maxRounds; $i++) {
        $moved = false;
        $moved = pump($left, $right) || $moved;
        $moved = pump($right, $left) || $moved;

        if (!$moved) {
            break;
        }
    }
}

$client = Nghttp2\Session::client();
$server = Nghttp2\Session::server();

drain($client, $server);
$client->popEvents();
$server->popEvents();

$streamId = $client->submitRequest([
    ['name' => ':method', 'value' => 'POST'],
    ['name' => ':scheme', 'value' => 'https'],
    ['name' => ':authority', 'value' => 'example.com'],
    ['name' => ':path', 'value' => '/upload'],
], false);

drain($client, $server);
$serverEvents = $server->popEvents();
var_dump(count($serverEvents));
var_dump($serverEvents[0]['type']);
var_dump($serverEvents[0]['endStream']);

$client->submitData($streamId, 'ab', false);
$client->submitData($streamId, 'cd', true);

drain($client, $server);

$serverEvents = $server->popEvents();
var_dump(count($serverEvents));
var_dump($serverEvents[0]['type']);
var_dump($serverEvents[0]['data']);
var_dump($serverEvents[1]['type']);
var_dump($serverEvents[1]['data']);

$server->submitResponse($streamId, [
    ['name' => ':status', 'value' => '200'],
    ['name' => 'content-type', 'value' => 'text/plain'],
], false);
$server->submitData($streamId, 'one', false);
$server->submitData($streamId, 'two', true);

drain($server, $client);

$clientEvents = $client->popEvents();
var_dump(count($clientEvents));
var_dump($clientEvents[0]['type']);
var_dump($clientEvents[1]['type']);
var_dump($clientEvents[1]['data']);
var_dump($clientEvents[2]['type']);
var_dump($clientEvents[2]['data']);
var_dump($clientEvents[3]['type']);
var_dump($clientEvents[3]['streamId'] === $streamId);
?>
--EXPECT--
int(1)
string(7) "headers"
bool(false)
int(2)
string(4) "data"
string(2) "ab"
string(4) "data"
string(2) "cd"
int(4)
string(7) "headers"
string(4) "data"
string(3) "one"
string(4) "data"
string(3) "two"
string(12) "stream_close"
bool(true)

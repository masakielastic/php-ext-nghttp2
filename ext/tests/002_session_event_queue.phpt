--TEST--
Nghttp2\Session emits headers, data, and stream_close events
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

$client = Nghttp2\Session::client();
$server = Nghttp2\Session::server();

drain($client, $server);
$client->popEvents();
$server->popEvents();

$streamId = $client->submitRequest([
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':scheme', 'value' => 'https'],
    ['name' => ':authority', 'value' => 'example.com'],
    ['name' => ':path', 'value' => '/hello'],
], true);

drain($client, $server);

$serverEvents = $server->popEvents();
var_dump(count($serverEvents));
var_dump($serverEvents[0]['type']);
var_dump($serverEvents[0]['category']);
var_dump($serverEvents[0]['streamId'] === $streamId);
var_dump($serverEvents[0]['endStream']);
var_dump($serverEvents[0]['headers'][0]['name']);
var_dump($serverEvents[0]['headers'][3]['value']);

$server->submitResponse($streamId, [
    ['name' => ':status', 'value' => '200'],
    ['name' => 'content-type', 'value' => 'text/plain'],
], false);
$server->submitData($streamId, "hello", true);

drain($server, $client);

$clientEvents = $client->popEvents();
var_dump(count($clientEvents));
var_dump($clientEvents[0]['type']);
var_dump($clientEvents[0]['category']);
var_dump($clientEvents[0]['headers'][0]['value']);
var_dump($clientEvents[1]['type']);
var_dump($clientEvents[1]['data']);
var_dump($clientEvents[2]['type']);
var_dump($clientEvents[2]['streamId'] === $streamId);
var_dump($clientEvents[2]['errorCode']);
?>
--EXPECT--
int(1)
string(7) "headers"
string(7) "request"
bool(true)
bool(true)
string(7) ":method"
string(6) "/hello"
int(3)
string(7) "headers"
string(8) "response"
string(3) "200"
string(4) "data"
string(5) "hello"
string(12) "stream_close"
bool(true)
int(0)

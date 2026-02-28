# php-nghttp2

A native PHP extension powered by **nghttp2** providing
low-level HTTP/2 functionality.

This extension aims to expose a minimal, explicit, and
stateful interface to HTTP/2 primitives including:

- HPACK (RFC 7541)
- Low-level HTTP/2 session API for userland event loops
- Blocking HTTP/2 client
- Blocking HTTP/2 server

The design focuses on low-level experimentation,
blocking APIs, and educational use.

See [CHANGELOG.md](/home/masakielastic/php-ext-nghttp2/CHANGELOG.md) for recent changes.

---

## Features (Current)

- Stateful HPACK encoder/decoder
- Low-level HTTP/2 session API
- Blocking HTTP/2 client over TLS (ALPN `h2`)
- Blocking HTTP/2 server over TLS (ALPN `h2`)
- Configurable server response (`setResponse`)
- Explicit error handling with nghttp2 error codes

---

## Requirements

- PHP 8.1+
- libnghttp2 (development headers required)
- OpenSSL (development headers required)
- pkg-config (recommended)

On Debian/Ubuntu:

```bash
sudo apt install libnghttp2-dev libssl-dev pkg-config
```

---

## Directory Structure

```
.
├── composer.json
├── README.md
└── ext/
    ├── config.m4
    ├── nghttp2.c
    ├── hpack.c
    ├── client.c
    ├── server.c
    ├── exception.c
    └── php_nghttp2.h
```

All extension sources reside in ext/.

---

## Installation (PIE)

Register repository:

```
pie repository:add vcs https://github.com/masakielastic/php-nghttp2
```

Install:

```
pie install php-nghttp2
```

Verify:

```
php -m | grep nghttp2
```

---

## Namespace

```php
namespace Nghttp2;
```

---

## HPACK API

Class: Nghttp2\Hpack

```
final class Hpack
{
    public function __construct(int $maxDynamicTableSize = 4096);

    public function setMaxDynamicTableSize(int $size): void;
    public function getMaxDynamicTableSize(): int;

    public function getDynamicTableSize(): int;
    public function clearDynamicTable(): void;

    public function encode(array $headers): string;
    public function decode(string $headerBlock): array;
}
```

---

## API Layers

- `Nghttp2\Hpack`: stateful HPACK encoder/decoder
- `Nghttp2\Session`: transport-agnostic HTTP/2 session API for userland event loops
- `Nghttp2\Client`: blocking HTTPS client helper
- `Nghttp2\Server`: blocking HTTPS server helper

`Nghttp2\Session` does not open sockets, perform TLS handshakes, or run an event loop.
Those responsibilities stay in userland PHP.

---

## Session API

Class: `Nghttp2\Session`

```php
final class Session
{
    public static function client(array $options = []): self;
    public static function server(array $options = []): self;

    public function receive(string $bytes): int;
    public function popOutbound(): string;
    public function popEvents(): array;

    public function wantsRead(): bool;
    public function wantsWrite(): bool;

    public function submitRequest(array $headers, bool $endStream = true): int;
    public function submitResponse(int $streamId, array $headers, bool $endStream = false): void;
    public function submitHeaders(int $streamId, array $headers, bool $endStream = false): void;
    public function submitData(int $streamId, string $data, bool $endStream = false): void;

    public function submitSettings(array $settings): void;
    public function submitPing(string $opaqueData = ""): void;
    public function submitRstStream(int $streamId, int $errorCode): void;
    public function submitGoaway(int $errorCode = 0, string $debugData = ""): void;

    public function close(): void;
}
```

### Basic Flow

`Nghttp2\Session` follows a simple loop:

1. Feed inbound bytes using `receive()`
2. Read outbound bytes using `popOutbound()`
3. Consume parsed protocol events using `popEvents()`

This makes it suitable for custom socket code, TLS streams, and PHP userland event loops.

### Events

`popEvents()` returns arrays such as:

```php
['type' => 'headers', 'streamId' => 1, 'category' => 'request', 'endStream' => false, 'headers' => [...]]
['type' => 'data', 'streamId' => 1, 'data' => "chunk"]
['type' => 'stream_close', 'streamId' => 1, 'errorCode' => 0]
['type' => 'settings', 'ack' => false, 'settings' => [['id' => 3, 'value' => 100]]]
['type' => 'ping', 'ack' => true, 'opaqueData' => "12345678"]
['type' => 'goaway', 'lastStreamId' => 1, 'errorCode' => 0, 'debugData' => ""]
['type' => 'rst_stream', 'streamId' => 1, 'errorCode' => 8]
```

### Session Client Example

```php
$context = stream_context_create([
    'ssl' => [
        'crypto_method' => STREAM_CRYPTO_METHOD_TLSv1_2_CLIENT | STREAM_CRYPTO_METHOD_TLSv1_3_CLIENT,
        'verify_peer' => true,
        'verify_peer_name' => true,
        'alpn_protocols' => "h2",
        'SNI_enabled' => true,
    ],
]);

$socket = stream_socket_client(
    'tls://example.com:443',
    $errno,
    $errstr,
    10,
    STREAM_CLIENT_CONNECT,
    $context
);

if ($socket === false) {
    throw new RuntimeException("connect failed: {$errstr}", $errno);
}

stream_set_blocking($socket, false);

$session = Nghttp2\Session::client();
$streamId = $session->submitRequest([
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':scheme', 'value' => 'https'],
    ['name' => ':authority', 'value' => 'example.com'],
    ['name' => ':path', 'value' => '/'],
], true);

$body = '';
$closed = false;

while (!$closed) {
    $out = $session->popOutbound();
    if ($out !== '') {
        fwrite($socket, $out);
    }

    $read = [$socket];
    $write = $session->wantsWrite() ? [$socket] : [];
    $except = null;

    stream_select($read, $write, $except, 5);

    if ($read) {
        $chunk = fread($socket, 65535);
        if ($chunk !== '' && $chunk !== false) {
            $session->receive($chunk);
        }
    }

    foreach ($session->popEvents() as $event) {
        if (($event['streamId'] ?? null) !== $streamId) {
            continue;
        }

        if ($event['type'] === 'data') {
            $body .= $event['data'];
        } elseif ($event['type'] === 'stream_close') {
            $closed = true;
        }
    }
}
```

### Session Server Example

```php
$context = stream_context_create([
    'ssl' => [
        'local_cert' => __DIR__ . '/server.crt',
        'local_pk' => __DIR__ . '/server.key',
        'allow_self_signed' => true,
        'verify_peer' => false,
        'alpn_protocols' => "h2",
    ],
]);

$server = stream_socket_server(
    'tls://127.0.0.1:8443',
    $errno,
    $errstr,
    STREAM_SERVER_BIND | STREAM_SERVER_LISTEN,
    $context
);

$socket = stream_socket_accept($server);
stream_set_blocking($socket, false);

$session = Nghttp2\Session::server();
$running = true;

while ($running) {
    $out = $session->popOutbound();
    if ($out !== '') {
        fwrite($socket, $out);
    }

    $read = [$socket];
    $write = $session->wantsWrite() ? [$socket] : [];
    $except = null;

    stream_select($read, $write, $except, 5);

    if ($read) {
        $chunk = fread($socket, 65535);
        if ($chunk !== '' && $chunk !== false) {
            $session->receive($chunk);
        }
    }

    foreach ($session->popEvents() as $event) {
        if ($event['type'] !== 'headers' || $event['category'] !== 'request') {
            continue;
        }

        $streamId = $event['streamId'];
        $body = "hello from Session\n";

        $session->submitResponse($streamId, [
            ['name' => ':status', 'value' => '200'],
            ['name' => 'content-type', 'value' => 'text/plain'],
            ['name' => 'content-length', 'value' => (string) strlen($body)],
        ], false);

        $session->submitData($streamId, $body, true);
        $running = false;
    }
}
```

---

## HTTP/2 Client API

Class: `Nghttp2\Client`

```php
final class Client
{
    public function __construct(string $host, int $port = 443);

    /**
     * @return array{
     *   status:int|null,
     *   headers:array<int, array{name:string, value:string}>,
     *   body:string
     * }
     */
    public function request(string $path, array $headers = []): array;

    public function close(): void;
}
```

---

## HTTP/2 Server API

Class: `Nghttp2\Server`

```php
final class Server
{
    public function __construct(
        string $certFile,
        string $keyFile,
        string $ip = "127.0.0.1",
        int $port = 8443
    );

    public function setResponse(int $status, array $headers, string $body): void;
    public function serveOnce(): void;
    public function serve(): void;
    public function close(): void;
}
```

`setResponse()` accepts headers in either of the following formats:

```php
[
    "content-type" => "application/json",
    ["name" => "x-example", "value" => "1"],
]
```

`content-length` is recalculated internally.

---

## Header Format

Encoding requires list format:

```php
[
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':path', 'value' => '/'],
    ['name' => 'host', 'value' => 'example.com'],
]
```

Decoding returns the same list format.

Headers are treated as raw binary strings.

---

## Example

```php
use Nghttp2\Hpack;

$hpack = new Hpack();

$headers = [
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':path', 'value' => '/'],
    ['name' => 'host', 'value' => 'example.com'],
];

$block = $hpack->encode($headers);

$decoded = $hpack->decode($block);

var_dump($decoded);
```

---

## Error Handling

All nghttp2-related failures throw:

```php
Nghttp2\Exception\HpackException
Nghttp2\Exception\SessionException
Nghttp2\Exception\ClientException
Nghttp2\Exception\ServerException
```

Error codes can be retrieved:

```php
$e->getNghttp2ErrorCode();
```

---

## Client Example

```php
$client = new Nghttp2\Client('httpbin.org', 443);

try {
    $res = $client->request('/get?foo=bar', [
        'accept' => 'application/json',
    ]);

    var_dump($res['status']);
    echo $res['body'], PHP_EOL;
} finally {
    $client->close();
}
```

---

## Server Example

```php
$server = new Nghttp2\Server(__DIR__ . '/server.crt', __DIR__ . '/server.key');
$server->setResponse(
    200,
    ['content-type' => 'application/json'],
    '{"ok":true}'
);

$server->serve();
```

self-signed certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key -out server.crt -days 365 \
  -subj "/CN=127.0.0.1"
```

test:

```bash
curl -k --http2 https://127.0.0.1:8443/
```

---

## Design Principles

 * Minimal abstraction
 * Explicit state control
 * No implicit I/O
 * Suitable for RFC experimentation
 * Designed for low-level HTTP/2 work

---

## Non-Goals
 * High-level HTTP client abstraction
 * PSR-7 integration
 * Built-in event loop abstraction
 * HTTP/3 (QPACK)

---

## License

MIT (or your chosen license)

---

## Status

Experimental.
Intended for educational and low-level HTTP/2 experimentation.

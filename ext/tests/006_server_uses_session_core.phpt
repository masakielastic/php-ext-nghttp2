--TEST--
Nghttp2\Server serves responses through the shared Session core
--SKIPIF--
<?php
if (!extension_loaded('nghttp2')) {
    die('skip nghttp2 extension is not loaded');
}
if (!extension_loaded('openssl')) {
    die('skip openssl extension is not loaded');
}
if (!extension_loaded('pcntl')) {
    die('skip pcntl extension is not loaded');
}

$certFile = '/tmp/php-ext-nghttp2-server-test.crt';
$keyFile = '/tmp/php-ext-nghttp2-server-test.key';

if (!is_file($certFile) || !is_file($keyFile)) {
    $openssl = trim((string) shell_exec('command -v openssl 2>/dev/null'));
    if ($openssl === '') {
        die('skip openssl command not available');
    }

    $cmd = sprintf(
        '%s req -x509 -newkey rsa:2048 -nodes -keyout %s -out %s -days 1 -subj %s -addext %s 2>/dev/null',
        escapeshellarg($openssl),
        escapeshellarg($keyFile),
        escapeshellarg($certFile),
        escapeshellarg('/CN=localhost'),
        escapeshellarg('subjectAltName=DNS:localhost')
    );

    exec($cmd, $output, $code);
    if ($code !== 0 || !is_file($certFile) || !is_file($keyFile)) {
        die('skip failed to generate test certificate');
    }
}
?>
--ENV--
SSL_CERT_FILE=/tmp/php-ext-nghttp2-server-test.crt
SSL_CERT_DIR=/etc/ssl/certs
--FILE--
<?php
$certFile = '/tmp/php-ext-nghttp2-server-test.crt';
$keyFile = '/tmp/php-ext-nghttp2-server-test.key';
$port = 19000 + (getmypid() % 10000);
$pid = pcntl_fork();

if ($pid === -1) {
    throw new RuntimeException('fork failed');
}

if ($pid === 0) {
    $server = new Nghttp2\Server($certFile, $keyFile, '127.0.0.1', $port);
    $server->setResponse(
        201,
        [
            'content-type' => 'text/plain',
            'x-server' => 'session-core',
        ],
        "server-session-core\n"
    );
    $server->serveOnce();
    $server->close();
    exit(0);
}

usleep(200000);

$client = new Nghttp2\Client('localhost', $port);
$response = $client->request('/server-test');
$client->close();

pcntl_waitpid($pid, $status);

$headerMap = [];
foreach ($response['headers'] as $header) {
    $headerMap[$header['name']] = $header['value'];
}

var_dump($response['status']);
var_dump($headerMap['content-type']);
var_dump($headerMap['x-server']);
var_dump($response['body']);
var_dump(pcntl_wifexited($status));
var_dump(pcntl_wexitstatus($status));
?>
--EXPECT--
int(201)
string(10) "text/plain"
string(12) "session-core"
string(20) "server-session-core
"
bool(true)
int(0)

--TEST--
Nghttp2\Client works against a local Session-based server
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

$certFile = '/tmp/php-ext-nghttp2-client-test.crt';
$keyFile = '/tmp/php-ext-nghttp2-client-test.key';

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
SSL_CERT_FILE=/tmp/php-ext-nghttp2-client-test.crt
SSL_CERT_DIR=/etc/ssl/certs
--FILE--
<?php
$certFile = '/tmp/php-ext-nghttp2-client-test.crt';
$keyFile = '/tmp/php-ext-nghttp2-client-test.key';
$port = 18000 + (getmypid() % 10000);
$pid = pcntl_fork();

if ($pid === -1) {
    throw new RuntimeException('fork failed');
}

if ($pid === 0) {
    $context = stream_context_create([
        'ssl' => [
            'local_cert' => $certFile,
            'local_pk' => $keyFile,
            'allow_self_signed' => true,
            'verify_peer' => false,
            'alpn_protocols' => "h2",
        ],
    ]);

    $server = stream_socket_server(
        "tls://127.0.0.1:{$port}",
        $errno,
        $errstr,
        STREAM_SERVER_BIND | STREAM_SERVER_LISTEN,
        $context
    );

    if ($server === false) {
        exit(2);
    }

    $socket = @stream_socket_accept($server, 5);
    if ($socket === false) {
        fclose($server);
        exit(3);
    }

    stream_set_blocking($socket, true);
    stream_set_timeout($socket, 5);

    $session = Nghttp2\Session::server();
    $responded = false;
    $closed = false;

    while (!$closed) {
        $out = $session->popOutbound();
        if ($out !== '') {
            fwrite($socket, $out);
        }

        $chunk = fread($socket, 65535);
        if ($chunk === '' || $chunk === false) {
            if (feof($socket)) {
                break;
            }
            $meta = stream_get_meta_data($socket);
            if (!empty($meta['timed_out'])) {
                continue;
            }
            break;
        }

        $session->receive($chunk);

        foreach ($session->popEvents() as $event) {
            if ($event['type'] === 'headers' && $event['category'] === 'request' && !$responded) {
                $path = '';
                $xTest = '';

                foreach ($event['headers'] as $header) {
                    if ($header['name'] === ':path') {
                        $path = $header['value'];
                    } elseif ($header['name'] === 'x-test') {
                        $xTest = $header['value'];
                    }
                }

                $body = $path . '|' . $xTest;
                $session->submitResponse($event['streamId'], [
                    ['name' => ':status', 'value' => '200'],
                    ['name' => 'content-type', 'value' => 'text/plain'],
                    ['name' => 'content-length', 'value' => (string) strlen($body)],
                ], false);
                $session->submitData($event['streamId'], $body, true);
                $responded = true;
            } elseif ($event['type'] === 'stream_close') {
                $closed = true;
            }
        }
    }

    $out = $session->popOutbound();
    if ($out !== '') {
        fwrite($socket, $out);
    }

    fclose($socket);
    fclose($server);
    exit($responded ? 0 : 4);
}

usleep(200000);

$client = new Nghttp2\Client('localhost', $port);
$response = $client->request('/client-test', [
    'x-test' => 'session-core',
]);
$client->close();

pcntl_waitpid($pid, $status);

var_dump($response['status']);
var_dump(count($response['headers']) >= 2);
var_dump($response['body']);
var_dump(pcntl_wifexited($status));
var_dump(pcntl_wexitstatus($status));
?>
--EXPECT--
int(200)
bool(true)
string(25) "/client-test|session-core"
bool(true)
int(0)

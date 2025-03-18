<?php
header('Access-Control-Allow-Origin: https://netham45.org');
header('Access-Control-Allow-Methods: GET');
header('Access-Control-Allow-Headers: *');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    exit(0);
}

if (!isset($_GET['url'])) {
    http_response_code(400);
    die('URL parameter required');
}

$url = $_GET['url'];

// Validate URL is from the specific GitHub repository
if (!preg_match('/^https:\/\/((api\.)?github\.com\/(netham45\/esp32-scream-receiver|repos\/netham45\/esp32-scream-receiver)|raw\.githubusercontent\.com\/netham45\/esp32-scream-receiver)/', $url)) {
    http_response_code(403);
    die('Only URLs from github.com/netham45/esp32-scream-receiver are allowed');
}

// Create cache directory if it doesn't exist
$cacheDir = __DIR__ . '/cache';
if (!file_exists($cacheDir)) {
    mkdir($cacheDir);
}

// Generate cache key from URL
$cacheKey = md5($url);
$cacheFile = $cacheDir . '/' . $cacheKey;
$cacheMetaFile = $cacheFile . '.meta';

// Skip cache for latest release requests
$skipCache = strpos($url, 'api.github.com/repos/netham45/esp32-scream-receiver/releases') !== false;

// Use cache if it exists and we're not skipping cache
$useCache = !$skipCache && file_exists($cacheFile) && file_exists($cacheMetaFile);

if ($useCache) {
    // Get cached metadata
    $meta = json_decode(file_get_contents($cacheMetaFile), true);
    header('Content-Type: ' . $meta['content_type']);
    readfile($cacheFile);
    exit;
}

// Forward the request
$ch = curl_init();
curl_setopt($ch, CURLOPT_URL, $url);
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_FOLLOWLOCATION, true);
curl_setopt($ch, CURLOPT_USERAGENT, 'ESP32-Flasher-Proxy');

// If it's the GitHub API, we need to handle JSON response
if (strpos($url, 'api.github.com') !== false) {
    header('Content-Type: application/json');
    $response = curl_exec($ch);
    
    // Only cache if not a release request
    if (!$skipCache) {
        file_put_contents($cacheFile, $response);
        file_put_contents($cacheMetaFile, json_encode([
            'time' => time(),
            'content_type' => 'application/json'
        ]));
    }
    
    echo $response;
} else {
    // For binary files, forward the content type
    curl_setopt($ch, CURLOPT_HEADER, true);
    $response = curl_exec($ch);
    $header_size = curl_getinfo($ch, CURLINFO_HEADER_SIZE);
    $headers = substr($response, 0, $header_size);
    $body = substr($response, $header_size);
    
    // Parse and forward content type
    $contentType = 'application/octet-stream';
    if (preg_match('/Content-Type: (.+)/', $headers, $matches)) {
        $contentType = trim($matches[1]);
        header('Content-Type: ' . $contentType);
    }
    
    // Only cache if not a release request
    if (!$skipCache) {
        file_put_contents($cacheFile, $body);
        file_put_contents($cacheMetaFile, json_encode([
            'time' => time(),
            'content_type' => $contentType
        ]));
    }
    
    echo $body;
}

if (curl_errno($ch)) {
    http_response_code(500);
    die('Proxy Error: ' . curl_error($ch));
}

curl_close($ch);
?>

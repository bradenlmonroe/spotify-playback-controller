#include <Arduino.h>
#include <SPI.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <JPEGDEC.h>
#include "esp_heap_caps.h"
#include "secrets.h"

// Known-good wiring from screen_tester.cpp
#define TFT_CS    4
#define TFT_RST   5
#define TFT_DC    6
#define TFT_MOSI  7
#define TFT_SCK   8

// Rotary encoder wiring
#define ENCODER_A    16
#define ENCODER_B    17
#define ENCODER_PUSH 39

Arduino_DataBus *bus =
    new Arduino_HWSPI(TFT_DC, TFT_CS);

Arduino_GFX *gfx =
    new Arduino_ST7796(
        bus,
        TFT_RST,
        1,      // landscape
        false
    );

// ---------- Playback state ----------

struct PlaybackState {
    String title;
    String artist;
    uint32_t durationMs;
    uint32_t progressMs;
    bool playing;
    bool shuffle;
    bool liked;
    String uri;
    String albumArtUrl;
};

PlaybackState playback = {
    "SONG TITLE",
    "Artist",
    222000,     // 3:42
    84000,      // 1:24
    true,
    false,
    false,
    "",
    ""
};

uint32_t positionReceivedAt = 0;

// ---------- Spotify / network state ----------

String spotifyAccessToken;
uint32_t spotifyTokenExpiresAt = 0;
uint32_t lastSpotifyPoll = 0;
uint32_t spotifyRefreshRequestedAt = 0;

constexpr uint32_t SPOTIFY_POLL_INTERVAL = 2000;
constexpr uint32_t SPOTIFY_COMMAND_REFRESH_DELAY = 350;

bool albumArtNeedsUpdate = false;
volatile bool albumArtTaskRunning = false;

constexpr size_t ALBUM_URL_SIZE = 256;
char pendingAlbumArtUrl[ALBUM_URL_SIZE] = "";


// ---------- 480 x 320 layout ----------

constexpr int ALBUM_X = 20;
constexpr int ALBUM_Y = 25;
constexpr int ALBUM_SIZE = 190;

constexpr int INFO_X = 230;

constexpr int BAR_X = 65;
constexpr int BAR_Y = 285;
constexpr int BAR_W = 350;
constexpr int BAR_H = 4;

// ---------- Encoder / control selection ----------

enum ControlIndex {
    CONTROL_SHUFFLE = 0,
    CONTROL_PREVIOUS,
    CONTROL_PLAY_PAUSE,
    CONTROL_NEXT,
    CONTROL_LIKE,
    CONTROL_COUNT
};

int selectedControl = CONTROL_PLAY_PAUSE;

// Set this to -1 if the knob moves opposite the direction you expect.
constexpr int ENCODER_DIRECTION = 1;

int8_t encoderAccumulator = 0;
uint8_t previousEncoderState = 0;

// The SparkFun encoder button is unusual: SW idles LOW and goes HIGH
// when pressed, so use INPUT_PULLDOWN + a RISING-edge interrupt.
volatile bool buttonFlagPressed = false;
volatile uint32_t lastButtonISRTime = 0;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;

void IRAM_ATTR buttonISR()
{
    uint32_t now = millis();

    if (now - lastButtonISRTime > BUTTON_DEBOUNCE_MS) {
        buttonFlagPressed = true;
        lastButtonISRTime = now;
    }
}

void drawAlbumPlaceholder();
void drawSongInfo();
void drawControls();
void resetProgressBar();
bool downloadAndDrawAlbumArt(const String &url);
void updateAlbumArt();
void albumArtTask(void *parameter);

// ---------- WiFi / Spotify ----------

void connectWiFi()
{
    Serial.print("Connecting to WiFi");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
}

bool refreshSpotifyToken()
{
    WiFiClientSecure client;
    client.setInsecure(); // Bring-up only; use certificate validation for production.

    HTTPClient http;

    if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
        Serial.println("Could not connect to Spotify accounts");
        return false;
    }

    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    String body =
        "grant_type=refresh_token"
        "&refresh_token=" + String(SPOTIFY_REFRESH_TOKEN) +
        "&client_id=" + String(SPOTIFY_CLIENT_ID);

    int status = http.POST(body);
    String response = http.getString();

    if (status != 200) {
        Serial.printf("Token refresh failed: HTTP %d\n", status);
        Serial.println(response);
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        Serial.print("Could not parse token JSON: ");
        Serial.println(error.c_str());
        http.end();
        return false;
    }

    spotifyAccessToken = doc["access_token"].as<String>();
    uint32_t expiresIn = doc["expires_in"] | 3600;

    // Refresh one minute before Spotify's reported expiration.
    spotifyTokenExpiresAt = millis() + (expiresIn > 60 ? expiresIn - 60 : expiresIn) * 1000UL;

    Serial.println("Spotify access token obtained");
    http.end();
    return !spotifyAccessToken.isEmpty();
}

String urlEncode(const String &value)
{
    String encoded;
    const char *hex = "0123456789ABCDEF";

    for (size_t i = 0; i < value.length(); ++i) {
        uint8_t c = (uint8_t)value[i];

        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (char)c;
        } else {
            encoded += '%';
            encoded += hex[(c >> 4) & 0x0F];
            encoded += hex[c & 0x0F];
        }
    }

    return encoded;
}

bool spotifySuccess(int status)
{
    return status >= 200 && status < 300;
}

int spotifyRequest(const char *method, const String &url, String &response,
                   const String &body = "")
{
    if (WiFi.status() != WL_CONNECTED || spotifyAccessToken.isEmpty()) {
        return -1;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    if (!http.begin(client, url)) {
        return -1;
    }

    http.addHeader("Authorization", "Bearer " + spotifyAccessToken);

    int status = -1;

    if (strcmp(method, "GET") == 0) {
        status = http.GET();
    }
    else if (strcmp(method, "POST") == 0) {
        if (body.length()) {
            http.addHeader("Content-Type", "application/json");
            status = http.POST(body);
        }
        else {
            // ESP32 HTTPClient does NOT add Content-Length when size == 0.
            // Spotify expects a body length on these bodyless POST commands.
            http.addHeader("Content-Length", "0");
            status = http.sendRequest("POST");
        }
    }
    else if (strcmp(method, "PUT") == 0) {
        if (body.length()) {
            http.addHeader("Content-Type", "application/json");
            status = http.PUT(body);
        }
        else {
            // Explicitly send Content-Length: 0 for pause/shuffle/etc.
            http.addHeader("Content-Length", "0");
            status = http.sendRequest("PUT");
        }
    }
    else if (strcmp(method, "DELETE") == 0) {
        http.addHeader("Content-Length", "0");
        status = http.sendRequest("DELETE");
    }

    // Avoid trying to drain an unknown-length/bodyless error response.
    // Playback GETs and other JSON responses normally have a positive length.
    if (status > 0 && http.getSize() > 0) {
        response = http.getString();
    }

    http.end();
    return status;
}

int spotifyRequestWithRefresh(const char *method, const String &url,
                              String &response, const String &body = "")
{
    int status = spotifyRequest(method, url, response, body);

    if (status == 401 && refreshSpotifyToken()) {
        response = "";
        status = spotifyRequest(method, url, response, body);
    }

    return status;
}

void requestSpotifyStateRefresh()
{
    spotifyRefreshRequestedAt = millis();
}

bool spotifyNext()
{
    String response;
    int status = spotifyRequestWithRefresh(
        "POST", "https://api.spotify.com/v1/me/player/next", response);

    Serial.printf("Next: HTTP %d\n", status);
    if (spotifySuccess(status)) requestSpotifyStateRefresh();
    return spotifySuccess(status);
}

bool spotifyPrevious()
{
    String response;
    int status = spotifyRequestWithRefresh(
        "POST", "https://api.spotify.com/v1/me/player/previous", response);

    Serial.printf("Previous: HTTP %d\n", status);
    if (spotifySuccess(status)) requestSpotifyStateRefresh();
    return spotifySuccess(status);
}

bool spotifyPause()
{
    String response;
    int status = spotifyRequestWithRefresh(
        "PUT", "https://api.spotify.com/v1/me/player/pause", response);

    Serial.printf("Pause: HTTP %d\n", status);
    if (spotifySuccess(status)) requestSpotifyStateRefresh();
    return spotifySuccess(status);
}

bool spotifyPlay()
{
    String response;
    int status = spotifyRequestWithRefresh(
        "PUT",
        "https://api.spotify.com/v1/me/player/play",
        response,
        "{}"
    );

    Serial.printf("Play: HTTP %d\n", status);
    if (spotifySuccess(status)) requestSpotifyStateRefresh();
    return spotifySuccess(status);
}

bool spotifySetShuffle(bool enabled)
{
    String response;
    String url = "https://api.spotify.com/v1/me/player/shuffle?state=";
    url += enabled ? "true" : "false";

    int status = spotifyRequestWithRefresh("PUT", url, response);
    Serial.printf("Shuffle: HTTP %d\n", status);

    if (spotifySuccess(status)) requestSpotifyStateRefresh();
    return spotifySuccess(status);
}

bool spotifyCheckLiked(const String &uri, bool &liked)
{
    if (uri.isEmpty()) return false;

    String response;
    String url = "https://api.spotify.com/v1/me/library/contains?uris=" +
                 urlEncode(uri);

    int status = spotifyRequestWithRefresh("GET", url, response);

    if (status != 200) {
        Serial.printf("Liked-state check: HTTP %d\n", status);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, response)) {
        return false;
    }

    liked = doc[0] | false;
    return true;
}

bool spotifySetLiked(bool liked)
{
    if (playback.uri.isEmpty()) {
        Serial.println("No Spotify URI available for current item");
        return false;
    }

    String response;
    String url = "https://api.spotify.com/v1/me/library?uris=" +
                 urlEncode(playback.uri);

    int status = spotifyRequestWithRefresh(
        liked ? "PUT" : "DELETE", url, response);

    Serial.printf("%s library item: HTTP %d\n",
                  liked ? "Save" : "Remove", status);

    // Current library endpoints return HTTP 200 on success.
    if (status == 200) {
        playback.liked = liked;
        drawControls();
        requestSpotifyStateRefresh();
        return true;
    }

    return false;
}

bool getSpotifyPlayback()
{
    if (spotifyAccessToken.isEmpty()) {
        Serial.println("No Spotify access token");
        return false;
    }

    String response;
    int status = spotifyRequestWithRefresh(
        "GET", "https://api.spotify.com/v1/me/player", response);

    if (status == 204) {
        Serial.println("No active Spotify playback");
        return true;
    }

    if (status != 200) {
        Serial.printf("Playback request failed: HTTP %d\n", status);
        if (!response.isEmpty()) Serial.println(response);
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        Serial.print("Could not parse playback JSON: ");
        Serial.println(error.c_str());
        return false;
    }

    JsonObject item = doc["item"].as<JsonObject>();
    if (item.isNull()) {
        Serial.println("Spotify returned no current item");
        return true;
    }

    String newTitle = item["name"].as<String>();
    String newArtist = item["artists"][0]["name"].isNull()
                           ? ""
                           : item["artists"][0]["name"].as<String>();
    String newUri = item["uri"].isNull() ? "" : item["uri"].as<String>();

    // Spotify returns album artwork in several sizes, widest first.
    // Choose the smallest image that is still at least as large as our
    // album-art box. This avoids downloading the 640x640 image unnecessarily.
    String newAlbumArtUrl = "";
    int bestAlbumWidth = 1000000;

    JsonArray images = item["album"]["images"].as<JsonArray>();
    for (JsonObject image : images) {
        int width = image["width"] | 0;
        String url = image["url"].as<String>();

        if (!url.isEmpty() && width >= ALBUM_SIZE && width < bestAlbumWidth) {
            bestAlbumWidth = width;
            newAlbumArtUrl = url;
        }
    }

    // Fallback to the first image if Spotify did not provide dimensions.
    if (newAlbumArtUrl.isEmpty() && images.size() > 0) {
        newAlbumArtUrl = images[0]["url"].as<String>();
    }

    uint32_t newDuration = item["duration_ms"] | 0;
    uint32_t newProgress = doc["progress_ms"] | 0;
    bool newPlaying = doc["is_playing"] | false;
    bool newShuffle = doc["shuffle_state"] | false;

    bool songChanged = (newUri != playback.uri);
    bool controlsChanged =
        (newPlaying != playback.playing) ||
        (newShuffle != playback.shuffle);

    playback.title = newTitle;
    playback.artist = newArtist;
    playback.durationMs = newDuration;
    playback.progressMs = newProgress;
    playback.playing = newPlaying;
    playback.shuffle = newShuffle;
    playback.uri = newUri;
    playback.albumArtUrl = newAlbumArtUrl;
    positionReceivedAt = millis();

    if (songChanged) {
        bool newLiked = false;
        if (spotifyCheckLiked(playback.uri, newLiked)) {
            if (newLiked != playback.liked) controlsChanged = true;
            playback.liked = newLiked;
        } else {
            playback.liked = false;
        }

        drawSongInfo();
        resetProgressBar();
        controlsChanged = true;

        // Copy the URL while still on loopTask. The album-art FreeRTOS task
        // never reads playback.albumArtUrl, avoiding cross-task Arduino String access.
        strncpy(
            pendingAlbumArtUrl,
            playback.albumArtUrl.c_str(),
            ALBUM_URL_SIZE - 1
        );
        pendingAlbumArtUrl[ALBUM_URL_SIZE - 1] = '\0';

        albumArtNeedsUpdate = true;
    }

    if (controlsChanged) {
        drawControls();
    }

    Serial.printf("Spotify: %s - %s [%s]\n",
                  playback.artist.c_str(),
                  playback.title.c_str(),
                  playback.playing ? "playing" : "paused");

    return true;
}

void updateSpotify()
{
    uint32_t now = millis();

    if (spotifyTokenExpiresAt != 0 &&
        (int32_t)(now - spotifyTokenExpiresAt) >= 0) {
        refreshSpotifyToken();
    }

    bool commandRefreshDue =
        spotifyRefreshRequestedAt != 0 &&
        now - spotifyRefreshRequestedAt >= SPOTIFY_COMMAND_REFRESH_DELAY;

    bool periodicRefreshDue =
        now - lastSpotifyPoll >= SPOTIFY_POLL_INTERVAL;

    if (!commandRefreshDue && !periodicRefreshDue) {
        return;
    }

    spotifyRefreshRequestedAt = 0;
    lastSpotifyPoll = now;
    getSpotifyPlayback();
}

// ---------- Helpers ----------

String formatTime(uint32_t ms)
{
    uint32_t seconds = ms / 1000;
    uint32_t minutes = seconds / 60;
    seconds %= 60;

    char buffer[12];
    snprintf(buffer, sizeof(buffer), "%lu:%02lu",
             (unsigned long)minutes,
             (unsigned long)seconds);

    return String(buffer);
}

uint32_t getCurrentPosition()
{
    if (!playback.playing) {
        return playback.progressMs;
    }

    uint32_t position =
        playback.progressMs + (millis() - positionReceivedAt);

    if (position > playback.durationMs) {
        position = playback.durationMs;
    }

    return position;
}

// ---------- Album art ----------

// JPEGDEC sends decoded RGB565 blocks through this callback.
static int drawAlbumJpegBlock(JPEGDRAW *pDraw)
{
    int x = pDraw->x;
    int y = pDraw->y;
    int w = pDraw->iWidth;
    int h = pDraw->iHeight;

    // The decode origin is chosen so all decoded blocks should stay inside
    // the 190x190 album-art region. Abort the decode if a block would escape.
    if (x < ALBUM_X || y < ALBUM_Y ||
        x + w > ALBUM_X + ALBUM_SIZE ||
        y + h > ALBUM_Y + ALBUM_SIZE) {
        Serial.printf(
            "JPEG block out of bounds: x=%d y=%d w=%d h=%d\n",
            x, y, w, h
        );
        return 0;
    }

    gfx->draw16bitRGBBitmap(
        x,
        y,
        pDraw->pPixels,
        w,
        h
    );

    return 1;
}

bool downloadAndDrawAlbumArt(const String &url)
{
    Serial.println("=== ALBUM ART DOWNLOAD + DECODE ===");

    if (url.isEmpty() || WiFi.status() != WL_CONNECTED) {
        Serial.println("Invalid URL or WiFi disconnected");
        return false;
    }

    Serial.printf("Free heap before: %u\n", ESP.getFreeHeap());
    Serial.printf(
        "Largest block before: %u\n",
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
    );

    Serial.println("URL:");
    Serial.println(url);

    // ---------- Download JPEG ----------
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(5000);

    Serial.println("Starting http.begin...");

    if (!http.begin(client, url)) {
        Serial.println("http.begin FAILED");
        return false;
    }

    Serial.println("http.begin OK");
    Serial.println("Starting GET...");

    int status = http.GET();

    Serial.printf("GET returned: %d\n", status);
    Serial.printf("Free heap after GET: %u\n", ESP.getFreeHeap());

    if (status != 200) {
        Serial.println("Album artwork GET failed");
        http.end();
        client.stop();
        return false;
    }

    int contentLength = http.getSize();

    Serial.printf("Content length: %d\n", contentLength);

    constexpr int MAX_JPEG_BYTES = 80 * 1024;

    if (contentLength <= 0 || contentLength > MAX_JPEG_BYTES) {
        Serial.printf("Invalid/oversized JPEG: %d bytes\n", contentLength);
        http.end();
        client.stop();
        return false;
    }

    uint8_t *jpegBuffer =
        static_cast<uint8_t *>(
            heap_caps_malloc(contentLength, MALLOC_CAP_8BIT)
        );

    if (jpegBuffer == nullptr) {
        Serial.println("JPEG malloc FAILED");
        http.end();
        client.stop();
        return false;
    }

    Serial.printf("Allocated %d bytes for JPEG\n", contentLength);
    Serial.printf("Free heap after allocation: %u\n", ESP.getFreeHeap());

    WiFiClient *stream = http.getStreamPtr();

    int received = 0;
    uint32_t lastDataTime = millis();

    while (received < contentLength) {
        int available = stream->available();

        if (available > 0) {
            int remaining = contentLength - received;
            int toRead = min(available, remaining);

            int count = stream->read(
                jpegBuffer + received,
                toRead
            );

            if (count > 0) {
                received += count;
                lastDataTime = millis();
            }
        }

        if (millis() - lastDataTime > 5000) {
            Serial.println("JPEG download timed out");
            break;
        }

        delay(1);
    }

    Serial.printf(
        "JPEG downloaded: %d/%d bytes\n",
        received,
        contentLength
    );

    // Release TLS/HTTP memory before starting JPEGDEC.
    http.end();
    client.stop();

    Serial.println("HTTP closed");
    Serial.printf("Free heap after HTTP close: %u\n", ESP.getFreeHeap());

    if (received != contentLength) {
        Serial.println("Incomplete JPEG download");
        free(jpegBuffer);
        return false;
    }

    if (received < 2 ||
        jpegBuffer[0] != 0xFF ||
        jpegBuffer[1] != 0xD8) {
        Serial.println("Invalid JPEG header");
        free(jpegBuffer);
        return false;
    }

    Serial.println("Valid JPEG header");

    // ---------- Decode JPEG ----------
    // IMPORTANT: JPEGDEC contains a large JPEGIMAGE structure. Do not put it
    // on this FreeRTOS task's stack. Allocate it from the heap instead.
    Serial.printf(
        "Stack high-water before JPEGDEC allocation: %u\n",
        (unsigned)uxTaskGetStackHighWaterMark(nullptr)
    );

    JPEGDEC *jpeg =
        static_cast<JPEGDEC *>(malloc(sizeof(JPEGDEC)));

    if (jpeg == nullptr) {
        Serial.println("JPEGDEC malloc FAILED");
        free(jpegBuffer);
        return false;
    }

    Serial.printf(
        "JPEGDEC allocated on heap (%u bytes)\n",
        (unsigned)sizeof(JPEGDEC)
    );
    Serial.printf(
        "Free heap after JPEGDEC allocation: %u\n",
        ESP.getFreeHeap()
    );

    Serial.println("Opening JPEG with JPEGDEC...");

    if (!jpeg->openRAM(
            jpegBuffer,
            received,
            drawAlbumJpegBlock
        )) {
        Serial.printf(
            "JPEGDEC open failed: %d\n",
            jpeg->getLastError()
        );

        free(jpeg);
        free(jpegBuffer);
        return false;
    }

    int sourceW = jpeg->getWidth();
    int sourceH = jpeg->getHeight();

    Serial.printf(
        "JPEGDEC open OK: %d x %d\n",
        sourceW,
        sourceH
    );

    // JPEGDEC decode() takes OPTION BITS, not a numeric scale index.
    // JPEG_SCALE_HALF == 2, JPEG_SCALE_QUARTER == 4, etc.
    int scaleLevel = 0;

    while (scaleLevel < 3 &&
           ((sourceW >> scaleLevel) > ALBUM_SIZE ||
            (sourceH >> scaleLevel) > ALBUM_SIZE)) {
        scaleLevel++;
    }

    int decodeOptions = 0;

    switch (scaleLevel) {
        case 1:
            decodeOptions = JPEG_SCALE_HALF;
            break;

        case 2:
            decodeOptions = JPEG_SCALE_QUARTER;
            break;

        case 3:
            decodeOptions = JPEG_SCALE_EIGHTH;
            break;

        default:
            decodeOptions = 0;
            break;
    }

    int decodedW = sourceW >> scaleLevel;
    int decodedH = sourceH >> scaleLevel;

    int drawX = ALBUM_X + (ALBUM_SIZE - decodedW) / 2;
    int drawY = ALBUM_Y + (ALBUM_SIZE - decodedH) / 2;

    Serial.printf(
        "JPEG scale level=%d, options=0x%X, decoded=%d x %d, origin=(%d,%d)\n",
        scaleLevel,
        decodeOptions,
        decodedW,
        decodedH,
        drawX,
        drawY
    );

    gfx->fillRect(
        ALBUM_X,
        ALBUM_Y,
        ALBUM_SIZE,
        ALBUM_SIZE,
        BLACK
    );

    Serial.println("Starting JPEG decode...");

    int decodeResult = jpeg->decode(
        drawX,
        drawY,
        decodeOptions
    );

    int jpegError = jpeg->getLastError();

    jpeg->close();

    Serial.printf(
        "JPEG decode returned: %d, error: %d\n",
        decodeResult,
        jpegError
    );

    free(jpeg);
    Serial.println("JPEGDEC heap object freed");

    free(jpegBuffer);

    Serial.println("JPEG buffer freed");
    Serial.printf("Free heap after free: %u\n", ESP.getFreeHeap());

    if (decodeResult == 0) {
        Serial.println("JPEG decode FAILED");
        return false;
    }

    gfx->drawRect(
        ALBUM_X,
        ALBUM_Y,
        ALBUM_SIZE,
        ALBUM_SIZE,
        WHITE
    );

    Serial.println("Album artwork drawn successfully");
    Serial.println("=== ALBUM ART COMPLETE ===");

    return true;
}



void albumArtTask(void *parameter)
{
    Serial.println("Album art task started");
    Serial.printf(
        "Album task initial stack high-water: %u\n",
        (unsigned)uxTaskGetStackHighWaterMark(nullptr)
    );

    // Make a task-local copy. From this point onward the task does not touch
    // playback.albumArtUrl or any other dynamically allocated URL String.
    char url[ALBUM_URL_SIZE];

    strncpy(
        url,
        pendingAlbumArtUrl,
        ALBUM_URL_SIZE - 1
    );
    url[ALBUM_URL_SIZE - 1] = '\0';

    Serial.print("Task URL: ");
    Serial.println(url);

    if (strlen(url) > 0) {
        if (!downloadAndDrawAlbumArt(String(url))) {
            Serial.println("Album artwork failed; using placeholder");
            drawAlbumPlaceholder();
        }
    } else {
        Serial.println("No album artwork URL");
        drawAlbumPlaceholder();
    }

    Serial.printf(
        "Album task final stack high-water: %u\n",
        (unsigned)uxTaskGetStackHighWaterMark(nullptr)
    );
    Serial.println("Album art task finished");

    albumArtTaskRunning = false;
    vTaskDelete(nullptr);
}

void updateAlbumArt()
{
    if (!albumArtNeedsUpdate || albumArtTaskRunning) {
        return;
    }

    albumArtNeedsUpdate = false;
    albumArtTaskRunning = true;

    BaseType_t result = xTaskCreate(
        albumArtTask,
        "albumArt",
        16384,       // dedicated 16 KB stack for TLS/HTTP work
        nullptr,
        1,
        nullptr
    );

    if (result != pdPASS) {
        Serial.println("Failed to create album art task");
        albumArtTaskRunning = false;
        albumArtNeedsUpdate = true;
    }
}

void drawAlbumPlaceholder()
{
    gfx->fillRect(
        ALBUM_X,
        ALBUM_Y,
        ALBUM_SIZE,
        ALBUM_SIZE,
        RGB565(45, 45, 45)
    );

    gfx->drawRect(
        ALBUM_X,
        ALBUM_Y,
        ALBUM_SIZE,
        ALBUM_SIZE,
        WHITE
    );

    gfx->setTextColor(WHITE);
    gfx->setTextSize(2);

    const char *label = "ALBUM COVER";
    int16_t x1, y1;
    uint16_t w, h;

    gfx->getTextBounds(
        label,
        0,
        0,
        &x1,
        &y1,
        &w,
        &h
    );

    gfx->setCursor(
        ALBUM_X + (ALBUM_SIZE - w) / 2,
        ALBUM_Y + (ALBUM_SIZE - h) / 2
    );

    gfx->print(label);
}

// ---------- Song information ----------

void drawSongInfo()
{
    gfx->fillRect(INFO_X, 20, 245, 90, BLACK);

    gfx->setTextColor(WHITE);

    String shownTitle = playback.title;

    if (shownTitle.length() > 13) {
        shownTitle = shownTitle.substring(0, 10) + "...";
    }

    gfx->setTextSize(3);
    gfx->setCursor(INFO_X, 30);
    gfx->print(shownTitle);

    String shownArtist = playback.artist;

    if (shownArtist.length() > 20) {
        shownArtist = shownArtist.substring(0, 17) + "...";
    }

    gfx->setTextSize(2);
    gfx->setCursor(INFO_X, 72);
    gfx->print(shownArtist);
}

// ---------- Control icons ----------

void drawPlay(int x, int y)
{
    gfx->fillTriangle(
        x, y,
        x, y + 30,
        x + 25, y + 15,
        WHITE
    );
}

void drawPause(int x, int y)
{
    gfx->fillRect(x, y, 8, 30, WHITE);
    gfx->fillRect(x + 15, y, 8, 30, WHITE);
}

void drawPrevious(int x, int y)
{
    gfx->fillRect(x, y, 4, 30, WHITE);

    gfx->fillTriangle(
        x + 27, y,
        x + 27, y + 30,
        x + 5, y + 15,
        WHITE
    );
}

void drawNext(int x, int y)
{
    gfx->fillTriangle(
        x, y,
        x, y + 30,
        x + 22, y + 15,
        WHITE
    );

    gfx->fillRect(x + 24, y, 4, 30, WHITE);
}

void drawShuffle(int x, int y, bool enabled)
{
    uint16_t color =
        enabled ? GREEN : RGB565(180, 180, 180);

    gfx->drawLine(x, y + 5, x + 8, y + 5, color);
    gfx->drawLine(x + 8, y + 5, x + 24, y + 24, color);
    gfx->drawLine(x + 24, y + 24, x + 31, y + 24, color);

    gfx->drawLine(x, y + 24, x + 8, y + 24, color);
    gfx->drawLine(x + 8, y + 24, x + 15, y + 16, color);
    gfx->drawLine(x + 22, y + 8, x + 25, y + 5, color);
    gfx->drawLine(x + 25, y + 5, x + 31, y + 5, color);

    gfx->drawLine(x + 27, y + 1, x + 31, y + 5, color);
    gfx->drawLine(x + 27, y + 9, x + 31, y + 5, color);

    gfx->drawLine(x + 27, y + 20, x + 31, y + 24, color);
    gfx->drawLine(x + 27, y + 28, x + 31, y + 24, color);
}

void drawHeart(int x, int y, bool filled)
{
    uint16_t color = filled ? GREEN : WHITE;

    if (filled) {
        gfx->fillCircle(x + 8, y + 8, 8, color);
        gfx->fillCircle(x + 22, y + 8, 8, color);

        gfx->fillTriangle(
            x, y + 9,
            x + 30, y + 9,
            x + 15, y + 29,
            color
        );
    }
    else {
        gfx->drawCircle(x + 8, y + 8, 8, color);
        gfx->drawCircle(x + 22, y + 8, 8, color);

        gfx->drawLine(x, y + 9, x + 15, y + 29, color);
        gfx->drawLine(x + 30, y + 9, x + 15, y + 29, color);
    }
}

void drawSelectionBox(int control, uint16_t color)
{
    const int boxY = 134;
    const int boxH = 42;

    const int boxX[CONTROL_COUNT] = {224, 274, 331, 379, 429};
    const int boxW[CONTROL_COUNT] = {43, 40, 38, 40, 42};

    gfx->drawRoundRect(boxX[control], boxY, boxW[control], boxH, 5, color);
}

void drawControls()
{
    const int y = 140;

    // Clear only the control area. This avoids redrawing the entire screen.
    gfx->fillRect(220, 130, 255, 50, BLACK);

    drawShuffle(230, y, playback.shuffle);
    drawPrevious(280, y);

    if (playback.playing) {
        drawPause(337, y);
    }
    else {
        drawPlay(337, y);
    }

    drawNext(385, y);
    drawHeart(435, y, playback.liked);

    // Green outline shows which action the encoder button will activate.
    drawSelectionBox(selectedControl, GREEN);
}

void moveSelection(int direction)
{
    int nextControl = selectedControl + direction;

    // Clamp at the two ends instead of wrapping around.
    nextControl = constrain(nextControl, 0, CONTROL_COUNT - 1);

    if (nextControl == selectedControl) {
        return;
    }

    // Erase only the old outline, then draw the new one.
    drawSelectionBox(selectedControl, BLACK);
    selectedControl = nextControl;
    drawSelectionBox(selectedControl, GREEN);

    Serial.printf("selected control %d\n", selectedControl);
}

void activateSelectedControl()
{
    switch (selectedControl) {
        case CONTROL_SHUFFLE:
            if (spotifySetShuffle(!playback.shuffle)) {
                playback.shuffle = !playback.shuffle;
                Serial.println(playback.shuffle ? "enabled shuffle" : "disabled shuffle");
                drawControls();
            }
            break;

        case CONTROL_PREVIOUS:
            if (spotifyPrevious()) {
                Serial.println("skipped backward");
            }
            break;

        case CONTROL_PLAY_PAUSE:
            if (playback.playing) {
                uint32_t frozenPosition = getCurrentPosition();

                if (spotifyPause()) {
                    playback.progressMs = frozenPosition;
                    playback.playing = false;
                    positionReceivedAt = millis();
                    Serial.println("paused playback");
                    drawControls();
                }
            }
            else {
                if (spotifyPlay()) {
                    positionReceivedAt = millis();
                    playback.playing = true;
                    Serial.println("resumed playback");
                    drawControls();
                }
            }
            break;

        case CONTROL_NEXT:
            if (spotifyNext()) {
                Serial.println("skipped forward");
            }
            break;

        case CONTROL_LIKE:
            if (spotifySetLiked(!playback.liked)) {
                Serial.println(playback.liked ? "liked song" : "unliked song");
            }
            break;
    }
}


void updateEncoder()
{
    // Gray-code transition table. A full mechanical detent normally produces
    // four valid transitions, so accumulate them before moving the UI.
    static const int8_t transitionTable[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    uint8_t currentState =
        (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);

    uint8_t transition = (previousEncoderState << 2) | currentState;
    encoderAccumulator += transitionTable[transition];
    previousEncoderState = currentState;

    if (encoderAccumulator >= 4) {
        encoderAccumulator = 0;
        moveSelection(ENCODER_DIRECTION);
    }
    else if (encoderAccumulator <= -4) {
        encoderAccumulator = 0;
        moveSelection(-ENCODER_DIRECTION);
    }

    // Consume the button flag set by the ISR. Keep the actual UI work
    // outside the interrupt so Serial and display drawing remain safe.
    if (buttonFlagPressed) {
        noInterrupts();
        buttonFlagPressed = false;
        interrupts();

        Serial.printf(
            "encoder press -> activate control %d\n",
            selectedControl
        );

        activateSelectedControl();
    }
}

// ---------- Progress bar ----------

int previousFilledWidth = 0;
String previousCurrentTime = "";

void resetProgressBar()
{
    previousFilledWidth = 0;
    previousCurrentTime = "";

    gfx->fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, RGB565(80, 80, 80));
    gfx->fillRect(20, 275, 35, 12, BLACK);

    String total = formatTime(playback.durationMs);
    gfx->fillRect(425, 275, 40, 12, BLACK);
    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);
    int totalWidth = total.length() * 6;
    gfx->setCursor(460 - totalWidth, 279);
    gfx->print(total);
}

void drawProgressBar()
{
    if (playback.durationMs == 0) {
        return;
    }

    uint32_t current = getCurrentPosition();
    float progress = (float)current / (float)playback.durationMs;
    progress = constrain(progress, 0.0f, 1.0f);
    int filledWidth = (int)(progress * BAR_W);

    // Handle seeks, previous-track commands, and song changes that move
    // progress backwards.
    if (filledWidth < previousFilledWidth) {
        gfx->fillRect(BAR_X, BAR_Y, BAR_W, BAR_H, RGB565(80, 80, 80));
        previousFilledWidth = 0;
    }

    if (filledWidth > previousFilledWidth) {
        gfx->fillRect(
            BAR_X + previousFilledWidth,
            BAR_Y,
            filledWidth - previousFilledWidth,
            BAR_H,
            WHITE
        );
        previousFilledWidth = filledWidth;
    }

    String currentTime = formatTime(current);

    if (currentTime != previousCurrentTime) {
        gfx->fillRect(20, 275, 35, 12, BLACK);
        gfx->setTextColor(WHITE);
        gfx->setTextSize(1);
        gfx->setCursor(20, 279);
        gfx->print(currentTime);
        previousCurrentTime = currentTime;
    }
}

// ---------- Complete UI ----------

void drawUI()
{
    gfx->fillScreen(BLACK);

    drawAlbumPlaceholder();
    drawSongInfo();
    drawControls();

    // Progress bar background
    gfx->fillRect(
        BAR_X,
        BAR_Y,
        BAR_W,
        BAR_H,
        RGB565(80, 80, 80)
    );

    // Total time only needs to be drawn once
    String total = formatTime(playback.durationMs);

    gfx->setTextColor(WHITE);
    gfx->setTextSize(1);

    int totalWidth = total.length() * 6;
    gfx->setCursor(460 - totalWidth, 279);
    gfx->print(total);

    drawProgressBar();
}

// ---------- Setup / loop ----------

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("Starting Spotify UI");

    pinMode(ENCODER_A, INPUT_PULLUP);
    pinMode(ENCODER_B, INPUT_PULLUP);
    pinMode(ENCODER_PUSH, INPUT_PULLDOWN);

    previousEncoderState =
        (digitalRead(ENCODER_A) << 1) | digitalRead(ENCODER_B);

    attachInterrupt(
        digitalPinToInterrupt(ENCODER_PUSH),
        buttonISR,
        RISING
    );

    Serial.println("Encoder initialized on A=16, B=17, PUSH=39");
    Serial.println("Button initialized as INPUT_PULLDOWN / RISING interrupt");

    // Keep the same SPI initialization that worked in screen_tester.cpp.
    SPI.begin(TFT_SCK, -1, TFT_MOSI, -1);

    Serial.println("SPI started");

    // Keep the same conservative 1 MHz SPI clock while bringing up the UI.
    if (!gfx->begin(1000000)) {
        Serial.println("Display begin FAILED");

        while (true) {
            delay(1000);
        }
    }

    Serial.printf(
        "Display initialized: %d x %d\n",
        gfx->width(),
        gfx->height()
    );

    connectWiFi();

    if (refreshSpotifyToken()) {
        getSpotifyPlayback();
        lastSpotifyPoll = millis();
    }
    else {
        Serial.println("Spotify authentication failed");
    }

    // getSpotifyPlayback() sets this when playback is available. If Spotify
    // has no active player, initialize it here for the placeholder state.
    if (positionReceivedAt == 0) {
        positionReceivedAt = millis();
    }

    drawUI();

    Serial.println("Spotify UI drawn");
}

void loop()
{
    // While albumArtTask is alive, it owns both networking and the TFT.
    // Avoid encoder-triggered TFT writes, Spotify TLS requests, and progress
    // drawing from loopTask until the artwork operation is finished.
    if (albumArtTaskRunning) {
        delay(1);
        return;
    }

    updateEncoder();
    updateSpotify();

    static uint32_t lastProgressDraw = 0;

    if (millis() - lastProgressDraw >= 500) {
        lastProgressDraw = millis();
        drawProgressBar();
    }

    // Starts only after updateSpotify() has fully returned.
    updateAlbumArt();
}

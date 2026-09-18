#include <Arduino.h>
#include <SPI.h>
#include <Arduino_GFX_Library.h>

// Known-good wiring from screen_tester.cpp
#define TFT_CS    4
#define TFT_RST   5
#define TFT_DC    6
#define TFT_MOSI  7
#define TFT_SCK   8

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
};

PlaybackState playback = {
    "SONG TITLE",
    "Artist",
    222000,     // 3:42
    84000,      // 1:24
    true,
    false,
    false
};

uint32_t positionReceivedAt = 0;

// ---------- 480 x 320 layout ----------

constexpr int ALBUM_X = 20;
constexpr int ALBUM_Y = 25;
constexpr int ALBUM_SIZE = 190;

constexpr int INFO_X = 230;

constexpr int BAR_X = 65;
constexpr int BAR_Y = 285;
constexpr int BAR_W = 350;
constexpr int BAR_H = 4;

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

void drawControls()
{
    const int y = 140;

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
}

// ---------- Progress bar ----------

void drawProgressBar()
{
    static int previousFilledWidth = 0;
    static String previousCurrentTime = "";

    if (playback.durationMs == 0) {
        return;
    }

    uint32_t current = getCurrentPosition();

    float progress =
        (float)current / (float)playback.durationMs;

    progress = constrain(progress, 0.0f, 1.0f);

    int filledWidth = (int)(progress * BAR_W);

    // Only draw the newly completed part of the bar.
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

    // Only redraw current time when the displayed second changes.
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

    positionReceivedAt = millis();

    drawUI();

    Serial.println("Spotify UI drawn");
}

void loop()
{
    // Update only the progress bar twice per second.
    // Later, incoming Spotify data can update the PlaybackState structure.
    static uint32_t lastProgressDraw = 0;

    if (millis() - lastProgressDraw >= 500) {
        lastProgressDraw = millis();
        drawProgressBar();
    }
}

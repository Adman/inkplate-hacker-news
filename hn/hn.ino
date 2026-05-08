/**
 * Inkplate 5 — Hacker News “best story in the last 24 hours” (by score).
 *
 * Uses https://hacker-news.firebaseio.com/v0/ (see Hacker News API README).
 * Flow: load /v0/topstories.json, fetch items until maxItemLookups, keep type
 * "story" with time in [now-86400, now], pick highest score; summarize its URL via
 * NVIDIA integrate chat completions (non-streaming JSON).
 *
 * Code layout:
 * - hn_top_story.* — Hacker News Firebase JSON / best story picker
 * - summary_client.* — HTTPS chat-completion client for summaries
 *
 * Requirements:
 * - Board: Soldered Inkplate 5
 * - Libraries: Inkplate Arduino, ArduinoJson (6.x or 7.x), HNParser (arduino-hn-parser),
 *   ESP32 HTTPClient / WiFiClientSecure
 *
 * Docs: https://docs.soldered.com/inkplate
 */

#ifndef ARDUINO_INKPLATE5
#error "Select Soldered Inkplate5 in the boards menu."
#endif

#if __has_include("secrets.h")
#  include "secrets.h"
#else
#  include "secrets.example.h"
#  warning "secrets.h missing — copy secrets.example.h to secrets.h with your Wi-Fi/NVIDIA secrets."
#endif

/** NVIDIA NIM chat completions (summaries). */
static const char kNvidiaChatUrl[] = "https://integrate.api.nvidia.com/v1/chat/completions";
static const char kNvidiaModel[] = "abacusai/dracarys-llama-3.1-70b-instruct";  // Model name

#include "hn_top_story.h"
#include "summary_client.h"

#include "Inkplate.h"
#include "Fonts/FreeMonoBold18pt7b.h"
#include "Fonts/FreeSans18pt7b.h"

#include <HNParser.h>
#include <esp_sleep.h>
#include <string.h>
#include <time.h>

#ifndef WIFI_TIMEOUT
#define WIFI_TIMEOUT 30
#endif

// Deep sleep — MCU reboots each wake; full redraw happens every interval below.
static constexpr uint32_t kSecondsPerHour = 3600;
static constexpr uint32_t kHoursBetweenDisplayRefresh = 1;
static const uint64_t kSleepBetweenUpdatesUs =
    static_cast<uint64_t>(kHoursBetweenDisplayRefresh * kSecondsPerHour) * 1000000ULL;

static constexpr uint32_t kWifiRetrySleepSeconds = 5;
static const uint64_t kSleepWifiRetryUs =
    static_cast<uint64_t>(kWifiRetrySleepSeconds) * 1000000ULL;

/** Screen edge margin (must match layoutMainTextBox). */
static const int16_t kScreenMargin = 22;
/** Extra air gap below the top edge (in addition to kScreenMargin) so text doesn’t sit flush with the bezel. */
static const int16_t kTopBreathingRoomPx = 16;

/**
 * Inkplate drawTextBox estimates chars/line from box width and fontSize; if the estimate is
 * too small vs real glyph width, text clips past x1. Shrink the box (inset) and bias fontSize
 * high so lines stay inside the panel — no word should hang past the margin.
 */
static const int16_t kTitleInsetX = 18;
static const uint16_t kTitleLineSpacingPx = 36;
/** Passed to drawTextBox only; drives conservative chars/line (actual face is FreeMonoBold18pt7b). */
static const uint16_t kTitleCharsWidthArg = 36;
/** Nudge the horizontal rule under the title upward (smaller gap than model vs drawTextBox). */
static const int16_t kTitleRuleLiftPx = 12;

/** Gap (px) between horizontal rule under title and summary text; keep summary above meta baseline. */
static const int16_t kSummaryTopGapPx = 8;
static const int16_t kSummaryAboveMetaPx = 54;
/** Extra pixels between summary line baselines (added to GFX font yAdvance). */
static const int16_t kSummaryLineSpacingExtraPx = 5;
/** Shown in the summary area while the LLM request is in flight. */
static const char kSummaryPendingMsg[] = "Generating summary...";
/** Horizontal rule above metadata (same width as title rule); pixels above meta baseline. */
static const int16_t kMetaRuleAboveBaselinePx = 35;

Inkplate display(INKPLATE_1BIT);
HNParser hnParser;

/** Word-wrap buffer for proportional summary font (GNU FreeSans 18pt); must fit widest line in pixels/trial. */
static char s_drawGfxLineBuf[256];

static void syncUtcClock();
static void drawGfxFontWrapped(Inkplate &disp, int16_t x0, int16_t yTop, int16_t x1, int16_t yBaselineMax,
                               const char *text, const GFXfont *font, int16_t lineSpacingExtraPx);
static void drawMainView(Inkplate &disp, const char *title, const char *meta, const char *summaryBody,
                         bool drawSummaryBody);

/**
 * Shorten meta with "..." so its bounding box does not extend past maxX (exclusive panel edge).
 */
static void clipGfxLineToRightEdge(Inkplate &disp, char *meta, size_t metaCap, int16_t anchorX,
                                   int16_t anchorY, int16_t maxX) {
  disp.setFont(&FreeMonoBold18pt7b);
  disp.setTextSize(1);

  int16_t bx, by;
  uint16_t bw, bh;
  disp.getTextBounds(meta, anchorX, anchorY, &bx, &by, &bw, &bh);
  if (anchorX + (int32_t)bw <= maxX) {
    return;
  }

  static const char suffix[] = "...";
  const size_t sufLen = strlen(suffix);
  char trial[192];

  size_t len = strlen(meta);
  for (;;) {
    if (len + sufLen + 1 > sizeof(trial)) {
      if (len == 0) {
        meta[0] = '\0';
        return;
      }
      len--;
      continue;
    }

    memcpy(trial, meta, len);
    memcpy(trial + len, suffix, sufLen + 1);

    disp.getTextBounds(trial, anchorX, anchorY, &bx, &by, &bw, &bh);
    if (anchorX + (int32_t)bw <= maxX) {
      const size_t need = len + sufLen + 1;
      if (need <= metaCap) {
        memcpy(meta, trial, need);
      } else {
        strncpy(meta, trial, metaCap - 1);
        meta[metaCap - 1] = '\0';
      }
      return;
    }

    if (len == 0) {
      meta[0] = '\0';
      return;
    }
    len--;
  }
}

/** Safe margins from screen edges (pixels). */
static void layoutMainTextBox(int16_t dw, int16_t dh, int16_t *boxX0, int16_t *boxY0, int16_t *boxX1,
                              int16_t *boxY1, int16_t *metaBaselineY) {
  const int16_t m = kScreenMargin;
  /* Extra footroom so the last title baseline + descenders stays above the meta line. */
  const int16_t footerBand = 56;

  int16_t x0 = m;
  int16_t y0 = m + kTopBreathingRoomPx;
  int16_t x1 = dw - m;
  int16_t y1 = dh - m - footerBand;

  if (x1 < x0 + 32) {
    x0 = 0;
    x1 = dw;
  }
  if (y1 < y0 + 32) {
    y0 = kTopBreathingRoomPx;
    y1 = dh - footerBand;
    if (y1 < y0 + 24) {
      int16_t stretch = y0 + 48;
      if (stretch > dh) {
        stretch = dh;
      }
      y1 = stretch;
    }
  }

  if (x1 > dw) {
    x1 = dw;
  }
  if (y1 > dh) {
    y1 = dh;
  }

  int16_t baseline = dh - m;
  if (baseline >= dh) {
    baseline = dh - 1;
  }
  if (baseline < y1 + 8) {
    baseline = (y1 + dh - 1) / 2;
  }

  *boxX0 = x0;
  *boxY0 = y0;
  *boxX1 = x1;
  *boxY1 = y1;
  *metaBaselineY = baseline;
}

/**
 * Matches Inkplate Graphics::drawTextBox line-breaking so the rule sits under the last visible title line.
 */
static int16_t yBelowWrappedTitle(int16_t tx0, int16_t ty0, int16_t tx1, int16_t ty1,
                                  uint16_t verticalSpacing, uint16_t fontSizeArg,
                                  uint16_t textSizeMultiplier, const char *text,
                                  int16_t metaBaselineY, int16_t dh) {
  if (!text || text[0] == '\0') {
    return (int16_t)(ty0 + verticalSpacing + 8);
  }

  uint16_t fs = (fontSizeArg * 3u) / 4u;
  if (fs == 0u) {
    fs = 1u;
  }
  const int16_t boxW = tx1 - tx0;
  int numCharsPerLine = boxW / (int16_t)(textSizeMultiplier * fs);
  if (numCharsPerLine < 1) {
    numCharsPerLine = 1;
  }

  const int textLen = (int)strlen(text);
  int offset = 0;
  int16_t lastLineY = ty0;

  for (int16_t lineY = ty0; lineY < ty1 - (int16_t)verticalSpacing && offset < textLen;
       lineY += (int16_t)verticalSpacing) {
    lastLineY = lineY;

    int remainingLength = textLen - offset;
    int lineLength = (remainingLength < numCharsPerLine) ? remainingLength : numCharsPerLine;

    char buffer[96];
    if (lineLength >= (int)sizeof(buffer)) {
      lineLength = (int)sizeof(buffer) - 1;
    }
    memcpy(buffer, text + offset, (size_t)lineLength);
    buffer[lineLength] = '\0';

    int lastSpaceIndex = -1;
    for (int j = 0; j < lineLength; ++j) {
      if (buffer[j] == ' ') {
        lastSpaceIndex = j;
      }
    }

    if ((offset + lineLength < textLen) && (text[offset + lineLength] != ' ') && (lastSpaceIndex != -1) &&
        ((lineY + (int16_t)verticalSpacing) < (ty1 - (int16_t)verticalSpacing))) {
      lineLength = lastSpaceIndex + 1;
    }

    offset += lineLength;
    while (offset < textLen && text[offset] == ' ') {
      offset++;
    }

    if (offset >= textLen) {
      break;
    }
  }

  static const int16_t kGapBelowLastLine = 2;
  int16_t yRule = lastLineY + (int16_t)verticalSpacing + kGapBelowLastLine - kTitleRuleLiftPx;

  if (yRule >= ty1) {
    yRule = ty1 - 2;
  }
  if (yRule >= metaBaselineY - 4) {
    yRule = metaBaselineY - 6;
  }
  if (yRule <= lastLineY) {
    yRule = lastLineY + (int16_t)(verticalSpacing / 2) + 2;
  }
  if (yRule >= dh) {
    yRule = dh - 2;
  }
  return yRule;
}

static bool gfxLineFitsWindow(Inkplate &disp, int16_t anchorX, int16_t baselineY, int16_t rightEdgeExclusive,
                              const char *text) {
  int16_t bx, by;
  uint16_t bw, bh;
  disp.getTextBounds(text, anchorX, baselineY, &bx, &by, &bw, &bh);
  return static_cast<int32_t>(bx) + static_cast<int32_t>(bw) <= rightEdgeExclusive;
}

/** Wrap summary using a proportional Adafruit GFX font (measured widths via getTextBounds). */
static void drawGfxFontWrapped(Inkplate &disp, int16_t x0, int16_t yTop, int16_t x1, int16_t yBaselineMax,
                               const char *text, const GFXfont *font, int16_t lineSpacingExtraPx) {
  if (!text || text[0] == '\0' || x1 <= x0 || !font) {
    return;
  }

  disp.setFont(font);
  disp.setTextSize(1);
  disp.setTextColor(BLACK);
  disp.setTextWrap(false);

  const int16_t lineStep = static_cast<int16_t>(font->yAdvance) + lineSpacingExtraPx;
  if (lineStep < 4) {
    return;
  }

  char trial[sizeof(s_drawGfxLineBuf)];
  char *const lineBuf = s_drawGfxLineBuf;
  int lineLen = 0;
  int16_t baseline = static_cast<int16_t>(yTop + lineStep);

  const auto flushLine = [&]() {
    if (lineLen <= 0) {
      return;
    }
    lineBuf[lineLen] = '\0';
    if (baseline <= yBaselineMax) {
      disp.setCursor(x0, baseline);
      disp.print(lineBuf);
    }
    baseline += lineStep;
    lineLen = 0;
  };

  const char *p = text;

  while (*p != '\0' && baseline <= yBaselineMax) {
    while (*p == ' ' || *p == '\t') {
      p++;
    }

    if (*p == '\n' || *p == '\r') {
      flushLine();
      if (baseline > yBaselineMax) {
        return;
      }
      while (*p == '\n' || *p == '\r') {
        p++;
      }
      continue;
    }

    if (*p == '\0') {
      break;
    }

    const char *wStart = p;
    while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
      p++;
    }
    const int wlen = static_cast<int>(p - wStart);
    if (wlen <= 0) {
      continue;
    }

  process_word:
    {
      const int gap = (lineLen > 0) ? 1 : 0;
      if (static_cast<size_t>(lineLen + gap + wlen) >= sizeof(s_drawGfxLineBuf)) {
        if (lineLen > 0) {
          flushLine();
          if (baseline > yBaselineMax) {
            return;
          }
          goto process_word;
        }
        goto emit_long_chunks;
      }

      size_t tlen;
      if (lineLen > 0) {
        memcpy(trial, lineBuf, static_cast<size_t>(lineLen));
        trial[lineLen] = ' ';
        memcpy(trial + lineLen + 1, wStart, static_cast<size_t>(wlen));
        tlen = static_cast<size_t>(lineLen + 1 + wlen);
      } else {
        memcpy(trial, wStart, static_cast<size_t>(wlen));
        tlen = static_cast<size_t>(wlen);
      }
      trial[tlen] = '\0';

      if (gfxLineFitsWindow(disp, x0, baseline, x1, trial)) {
        if (lineLen > 0) {
          lineBuf[lineLen++] = ' ';
        }
        memcpy(lineBuf + lineLen, wStart, static_cast<size_t>(wlen));
        lineLen += wlen;
        continue;
      }

      if (lineLen > 0) {
        flushLine();
        if (baseline > yBaselineMax) {
          return;
        }
        goto process_word;
      }

    emit_long_chunks:
      int off = 0;
      while (off < wlen && baseline <= yBaselineMax) {
        int lo = 1;
        int hi = wlen - off;
        if (hi > static_cast<int>(sizeof(trial)) - 1) {
          hi = static_cast<int>(sizeof(trial)) - 1;
        }
        int best = 0;
        while (lo <= hi) {
          const int mid = (lo + hi) / 2;
          memcpy(trial, wStart + off, static_cast<size_t>(mid));
          trial[mid] = '\0';
          if (gfxLineFitsWindow(disp, x0, baseline, x1, trial)) {
            best = mid;
            lo = mid + 1;
          } else {
            hi = mid - 1;
          }
        }
        if (best <= 0) {
          break;
        }
        memcpy(trial, wStart + off, static_cast<size_t>(best));
        trial[best] = '\0';
        disp.setCursor(x0, baseline);
        disp.print(trial);
        baseline += lineStep;
        off += best;
      }
      continue;
    }
  }

  if (lineLen > 0 && baseline <= yBaselineMax) {
    lineBuf[lineLen] = '\0';
    disp.setCursor(x0, baseline);
    disp.print(lineBuf);
  }
}

/**
 * Full main screen: title, rules, optional summary body (FreeSans), metadata strip (FreeMono).
 * meta is copied before clipping so repeated paints do not truncate the source string.
 */
static void drawMainView(Inkplate &disp, const char *title, const char *meta, const char *summaryBody,
                         bool drawSummaryBody) {
  disp.clearDisplay();
  const int16_t dw = disp.width();
  const int16_t dh = disp.height();

  int16_t boxX0;
  int16_t boxY0;
  int16_t boxX1;
  int16_t boxY1;
  int16_t metaY;

  layoutMainTextBox(dw, dh, &boxX0, &boxY0, &boxX1, &boxY1, &metaY);

  int16_t tx0 = boxX0 + kTitleInsetX;
  int16_t tx1 = boxX1 - kTitleInsetX;
  if (tx1 <= tx0 + 48) {
    tx0 = boxX0 + 8;
    tx1 = boxX1 - 8;
    if (tx1 <= tx0 + 48) {
      tx0 = boxX0;
      tx1 = boxX1;
    }
  }

  disp.drawTextBox(tx0, boxY0, tx1, boxY1, title, 1, &FreeMonoBold18pt7b, kTitleLineSpacingPx, false,
                   kTitleCharsWidthArg);

  const int16_t sepY =
      yBelowWrappedTitle(tx0, boxY0, tx1, boxY1, kTitleLineSpacingPx, kTitleCharsWidthArg, 1, title, metaY, dh);
  const int16_t lineW = tx1 - tx0;
  if (lineW > 0 && sepY >= 0 && sepY < dh) {
    disp.drawFastHLine(tx0, sepY, lineW, BLACK);
  }

  const int16_t summaryTop = static_cast<int16_t>(sepY + kSummaryTopGapPx);
  const int16_t summaryBaselineMax = static_cast<int16_t>(metaY - kSummaryAboveMetaPx);
  if (drawSummaryBody && summaryBody && summaryBody[0] != '\0' && summaryBaselineMax > summaryTop) {
    drawGfxFontWrapped(disp, tx0, summaryTop, tx1, summaryBaselineMax, summaryBody, &FreeSans18pt7b,
                       kSummaryLineSpacingExtraPx);
  }

  int16_t metaSepY = static_cast<int16_t>(metaY - kMetaRuleAboveBaselinePx);
  if (metaSepY <= sepY) {
    metaSepY = static_cast<int16_t>(sepY + 4);
  }
  if (metaSepY >= metaY - 3) {
    metaSepY = static_cast<int16_t>(metaY - 6);
  }
  if (lineW > 0 && metaSepY >= 0 && metaSepY < dh) {
    disp.drawFastHLine(tx0, metaSepY, lineW, BLACK);
  }

  disp.setFont(&FreeMonoBold18pt7b);
  disp.setTextSize(1);

  const int16_t metaMaxX = dw - kScreenMargin - 2;
  char metaClip[192];
  strncpy(metaClip, meta, sizeof(metaClip) - 1);
  metaClip[sizeof(metaClip) - 1] = '\0';
  clipGfxLineToRightEdge(disp, metaClip, sizeof(metaClip), tx0, metaY, metaMaxX);

  disp.setCursor(tx0, metaY);
  disp.print(metaClip);
}

void setup() {
  Serial.begin(115200);

  display.begin();
  display.setTextColor(BLACK);
  display.setTextWrap(false);

  if (!display.connectWiFi(ssid, pass, WIFI_TIMEOUT, true)) {
    const int16_t dw = display.width();
    const int16_t dh = display.height();
    const int16_t edge = 24;

    display.clearDisplay();
    display.setFont(nullptr);
    display.setTextSize(2);
    display.setCursor(edge, dh / 2 - 36);
    display.println(F("WiFi failed"));
    display.setCursor(edge, dh / 2 - 4);
    display.println(F("Check SSID/PASS"));
    display.display();
    esp_sleep_enable_timer_wakeup(kSleepWifiRetryUs);
    (void)esp_deep_sleep_start();
  }

  syncUtcClock();

  char title[384];
  char meta[192];
  char storyUrl[512];
  storyUrl[0] = '\0';

  int score = -1;
  uint32_t submitted = 0;

  unsigned attempts = 0;
  bool hnOk = false;
  while (!fetchTopStoryLast24h(hnParser, title, sizeof(title), meta, sizeof(meta), storyUrl,
                               sizeof(storyUrl), &score, &submitted, 50)) {
    Serial.print(F("HN fetch retry "));
    Serial.println(attempts);
    delay(1500);
    if (++attempts > 15) {
      strncpy(title, "Could not reach Hacker News", sizeof(title) - 1);
      title[sizeof(title) - 1] = '\0';
      strncpy(meta, "HTTPS or JSON error — will retry after sleep", sizeof(meta) - 1);
      meta[sizeof(meta) - 1] = '\0';
      storyUrl[0] = '\0';
      break;
    }
  }

  if (attempts <= 15 && storyUrl[0] != '\0') {
    hnOk = true;
  }

  char summary[576];
  summary[0] = '\0';

  if (hnOk) {
    drawMainView(display, title, meta, kSummaryPendingMsg, true);
    display.display();

    if (!fetchWebpageSummary(kNvidiaChatUrl, kNvidiaApiKey, kNvidiaModel, storyUrl, summary,
                             sizeof(summary))) {
      strncpy(summary, "(Summary unavailable.)", sizeof(summary) - 1);
      summary[sizeof(summary) - 1] = '\0';
    }
  }

  drawMainView(display, title, meta, summary, hnOk);
  display.display();

  esp_sleep_enable_timer_wakeup(kSleepBetweenUpdatesUs);
  (void)esp_deep_sleep_start();
}

void loop() {
  // Deep sleep restarts the ESP32; loop is unused.
}

static void syncUtcClock() {
  // UTC for consistent “last 24 hours” vs API Unix times.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  struct tm timeinfo;
  int tries = 0;
  while (!getLocalTime(&timeinfo, 2500) && tries++ < 40) {
    delay(250);
  }
}

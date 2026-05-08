#include "hn_top_story.h"

#include <ArduinoJson.h>
#include <HNParser.h>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Keep large JSON buffers in BSS, not on loopTask stack (TLS/HN/parser stack depth).
 */
#if ARDUINOJSON_VERSION_MAJOR >= 7
static JsonDocument s_hnIdListDoc;
static JsonDocument s_hnItemDoc;
#else
static DynamicJsonDocument s_hnIdListDoc(16384);
static DynamicJsonDocument s_hnItemDoc(12288);
#endif

static char s_hnBestTitleBuf[384];
static char s_hnBestUrlBuf[512];

bool fetchTopStoryLast24h(HNParser &hn, char *title, size_t title_cap, char *meta, size_t meta_cap,
                          char *story_url, size_t story_url_cap, int *out_score, uint32_t *out_time,
                          unsigned max_item_lookups) {
  const time_t now = time(nullptr);
  if (now < 86400) {
    return false;
  }
  const time_t window_start = now - 86400;

  s_hnIdListDoc.clear();
  s_hnItemDoc.clear();

  if (!hn.getTopStories(s_hnIdListDoc)) {
    return false;
  }

  JsonArray ids = s_hnIdListDoc.as<JsonArray>();
  if (ids.isNull() || ids.size() == 0) {
    return false;
  }

  int best_score = -1;
  s_hnBestTitleBuf[0] = '\0';
  s_hnBestUrlBuf[0] = '\0';
  uint32_t best_time = 0;

  const size_t n = ids.size();
  const unsigned limit = (max_item_lookups < n) ? max_item_lookups : static_cast<unsigned>(n);

  for (unsigned i = 0; i < limit; i++) {
    const int id = ids[i].as<int>();
    if (!hn.getItem(id, s_hnItemDoc)) {
      continue;
    }
    if (s_hnItemDoc["deleted"].as<bool>() || s_hnItemDoc["dead"].as<bool>()) {
      continue;
    }
    const char *type = s_hnItemDoc["type"];
    if (!type || strcmp(type, "story") != 0) {
      continue;
    }
    const uint32_t t = s_hnItemDoc["time"].as<uint32_t>();
    if (t < static_cast<uint32_t>(window_start) || t > static_cast<uint32_t>(now)) {
      continue;
    }
    const int score = s_hnItemDoc["score"] | 0;
    if (score <= best_score) {
      continue;
    }
    const char *tit = s_hnItemDoc["title"];
    if (!tit || tit[0] == '\0') {
      continue;
    }
    best_score = score;
    strncpy(s_hnBestTitleBuf, tit, sizeof(s_hnBestTitleBuf) - 1);
    s_hnBestTitleBuf[sizeof(s_hnBestTitleBuf) - 1] = '\0';
    const char *story_href = s_hnItemDoc["url"];
    if (story_href && story_href[0]) {
      strncpy(s_hnBestUrlBuf, story_href, sizeof(s_hnBestUrlBuf) - 1);
      s_hnBestUrlBuf[sizeof(s_hnBestUrlBuf) - 1] = '\0';
    } else {
      snprintf(s_hnBestUrlBuf, sizeof(s_hnBestUrlBuf), "https://news.ycombinator.com/item?id=%d", id);
    }
    best_time = t;
  }

  if (best_score < 0 || s_hnBestTitleBuf[0] == '\0') {
    return false;
  }

  strncpy(title, s_hnBestTitleBuf, title_cap - 1);
  title[title_cap - 1] = '\0';

  if (story_url && story_url_cap > 0) {
    strncpy(story_url, s_hnBestUrlBuf, story_url_cap - 1);
    story_url[story_url_cap - 1] = '\0';
  }

  char time_buf[40];
  struct tm tm_utc {};
  const time_t submitted_utc = static_cast<time_t>(best_time);
  if (gmtime_r(&submitted_utc, &tm_utc) != nullptr) {
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M UTC", &tm_utc);
  } else {
    strncpy(time_buf, "?", sizeof(time_buf) - 1);
    time_buf[sizeof(time_buf) - 1] = '\0';
  }

  snprintf(meta, meta_cap, "%d pts | %s", best_score, time_buf);

  *out_score = best_score;
  *out_time = best_time;
  return true;
}

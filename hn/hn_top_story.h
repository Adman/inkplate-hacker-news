#pragma once

#include <stddef.h>
#include <stdint.h>

class HNParser;

/**
 * From /v0/topstories.json, fetch at most max_item_lookups items; among type "story" with
 * time in [now-86400, now], choose the highest score. Requires NTP (or other) Unix time.
 * out_id receives the chosen story's HN item id.
 */
bool fetchTopStoryLast24h(HNParser &hn, char *title, size_t title_cap, char *meta, size_t meta_cap,
                          char *story_url, size_t story_url_cap, int *out_score, uint32_t *out_time,
                          uint32_t *out_id, unsigned max_item_lookups);

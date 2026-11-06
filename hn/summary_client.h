#pragma once

#include <stddef.h>

/**
 * Ask a Cursor Cloud Agent to read page_url and summarize it (500 chars max in prompt).
 * Blocks until the agent run ends (typically tens of seconds to a few minutes).
 * api_key: Cursor API key; model: id from GET /v1/models, or nullptr for the account default.
 */
bool fetchWebpageSummary(const char *api_key, const char *model, const char *page_url, char *out,
                         size_t out_cap);

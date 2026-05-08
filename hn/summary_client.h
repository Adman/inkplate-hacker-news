#pragma once

#include <stddef.h>

/**
 * POST a non-streaming chat completion asking the model to summarize page_url (500 chars max in prompt).
 * chat_url: e.g. https://integrate.api.nvidia.com/v1/chat/completions
 * api_key: raw key (Bearer added internally)
 */
bool fetchWebpageSummary(const char *chat_url, const char *api_key, const char *model, const char *page_url,
                         char *out, size_t out_cap);

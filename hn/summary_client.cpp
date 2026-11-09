#include "summary_client.h"

#include <Arduino.h>
#include <CursorAgent.h>
#include <stdio.h>
#include <WiFi.h>

#include <string.h>

static char s_summaryPrompt[896];

bool fetchWebpageSummary(const char *api_key, const char *model, const char *page_url, char *out,
                         size_t out_cap) {
  if (!api_key || !page_url || page_url[0] == '\0' || out_cap < 8) {
    return false;
  }
  out[0] = '\0';

  WiFi.setSleep(false);

  const int n = snprintf(s_summaryPrompt, sizeof(s_summaryPrompt),
                         "Read the web page at %s (use your web fetch tool, or curl if that fails). "
                         "Then reply with a summary of the page in 380 characters maximum, as plain "
                         "text: no markdown, no quotes, no preamble. Do not create or modify any files.",
                         page_url);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(s_summaryPrompt)) {
    return false;
  }

  CursorAgent agent(api_key);
  agent.setModel(model);
  agent.setCleanup(CursorAgentCleanup::Archive);

  if (!agent.prompt(s_summaryPrompt, out, out_cap)) {
    Serial.print(F("Summary agent failed: "));
    Serial.println(agent.lastError());
    return false;
  }

  size_t tail = strlen(out);
  while (tail > 0 && (out[tail - 1] == '\n' || out[tail - 1] == '\r' || out[tail - 1] == ' ')) {
    out[--tail] = '\0';
  }

  return out[0] != '\0';
}

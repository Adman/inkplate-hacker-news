#include "summary_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <stdio.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <string.h>

#if ARDUINOJSON_VERSION_MAJOR >= 7
static JsonDocument s_summaryReqDoc;
static JsonDocument s_summaryRespDoc;
#else
static StaticJsonDocument<3072> s_summaryReqDoc;
/** Full chat-completion JSON (non-streaming); wrappers + ~512-token reply text. */
static StaticJsonDocument<8192> s_summaryRespDoc;
#endif

static char s_summaryRespBuf[6144];
static char s_summaryPayload[2304];
static char s_summaryUserMsg[896];
static char s_summaryAuthHdr[288];
static WiFiClientSecure s_summaryTls;
static HTTPClient s_summaryHttp;

bool fetchWebpageSummary(const char *chat_url, const char *api_key, const char *model, const char *page_url,
                         char *out, size_t out_cap) {
  if (!chat_url || !api_key || !model || !page_url || page_url[0] == '\0' || out_cap < 8) {
    return false;
  }
  out[0] = '\0';

  /*
   * ESP32 Arduino HTTPClient::setTimeout takes uint16_t ms (max ~65535). Values like 120000 wrap.
   * Chunked streaming responses also tend to hit READ_TIMEOUT (-11); use one-shot JSON instead.
   */
  WiFi.setSleep(false);

  snprintf(s_summaryUserMsg, sizeof(s_summaryUserMsg),
           "Summarize the following webpage in 500 characters maximum: %s", page_url);

  s_summaryReqDoc.clear();
  s_summaryReqDoc["model"] = model;
  s_summaryReqDoc["messages"][0]["role"] = "user";
  s_summaryReqDoc["messages"][0]["content"] = s_summaryUserMsg;
  s_summaryReqDoc["temperature"] = 1;
  s_summaryReqDoc["top_p"] = 0.95;
  s_summaryReqDoc["max_tokens"] = 512;
  s_summaryReqDoc["chat_template_kwargs"]["thinking"] = false;
  s_summaryReqDoc["stream"] = false;

  const size_t plen = serializeJson(s_summaryReqDoc, s_summaryPayload, sizeof(s_summaryPayload));
  if (plen == 0 || plen >= sizeof(s_summaryPayload)) {
    return false;
  }

  s_summaryTls.setInsecure();
  s_summaryTls.setHandshakeTimeout(120); // seconds (slow LTE/WiFi TLS handshakes)

  s_summaryHttp.setReuse(false);
  s_summaryHttp.setTimeout(static_cast<uint16_t>(65535));
  if (!s_summaryHttp.begin(s_summaryTls, chat_url)) {
    return false;
  }

  snprintf(s_summaryAuthHdr, sizeof(s_summaryAuthHdr), "Bearer %s", api_key);
  s_summaryHttp.addHeader("Authorization", s_summaryAuthHdr);
  s_summaryHttp.addHeader("Content-Type", "application/json");

  const int http_code = s_summaryHttp.POST(s_summaryPayload);
  if (http_code != HTTP_CODE_OK) {
    Serial.print(F("Summary HTTP "));
    Serial.print(http_code);
    if (http_code == HTTPC_ERROR_READ_TIMEOUT) {
      Serial.print(F(" READ_TIMEOUT"));
    }
    Serial.println();
    s_summaryHttp.end();
    return false;
  }

  WiFiClient *stream = s_summaryHttp.getStreamPtr();
  size_t body_len = 0;
  const unsigned long body_start_ms = millis();
  while (s_summaryHttp.connected() || stream->available()) {
    while (stream->available() && body_len + 1 < sizeof(s_summaryRespBuf)) {
      const int n =
          stream->readBytes(&s_summaryRespBuf[body_len], sizeof(s_summaryRespBuf) - 1 - body_len);
      if (n <= 0) {
        break;
      }
      body_len += static_cast<size_t>(n);
    }
    if (!s_summaryHttp.connected() && !stream->available()) {
      break;
    }
    if (millis() - body_start_ms > 65000) {
      Serial.println(F("Summary HTTP body read stalled"));
      break;
    }
    delay(1);
  }
  s_summaryRespBuf[body_len] = '\0';

  s_summaryHttp.end();

  s_summaryRespDoc.clear();
  if (deserializeJson(s_summaryRespDoc, s_summaryRespBuf)) {
    Serial.println(F("Summary JSON parse failed"));
    return false;
  }

  JsonArray choices = s_summaryRespDoc["choices"].as<JsonArray>();
  if (choices.isNull() || choices.size() == 0) {
    return false;
  }

  const char *msg = choices[0]["message"]["content"];
  if (!msg || msg[0] == '\0') {
    return false;
  }

  strncpy(out, msg, out_cap - 1);
  out[out_cap - 1] = '\0';

  size_t tail = strlen(out);
  while (tail > 0 && (out[tail - 1] == '\n' || out[tail - 1] == '\r' || out[tail - 1] == ' ')) {
    out[--tail] = '\0';
  }

  return out[0] != '\0';
}

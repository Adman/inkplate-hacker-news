# Inkplate Hacker News

Firmware for **[Soldered Inkplate 5](https://docs.soldered.com/inkplate)** that periodically shows **the highest-scoring Hacker News story from the last 24 hours**, with an **AI-generated summary** of the linked article rendered on e-paper alongside the headline, score, and submission time.

## What it does

1. Connects to Wi‑Fi and syncs **UTC** over NTP (needed for the 24 h window).
2. Fetches [`/v0/topstories`](https://github.com/HackerNews/API) from the HN Firebase API via **HNParser**, walks up to **50** IDs, and picks the **`story`** with the **best score** whose submission timestamp falls in **`[now − 24h, now]`**.
3. Draws title, separators, footer metadata (**points** and **timestamp**), and **“Generating summary…”** while a **single non-streaming** chat-completion request summarizes the article URL (**NVIDIA Integrate API**).
4. Redraws the full screen with the final summary (or **`(Summary unavailable.)`**).
5. Enters **deep sleep** for **1 hour** (configurable in code); each wake repeats the pipeline from a cold boot-style `setup()` path.

Fonts bundled under `hn/Fonts/`:

- **`FreeMonoBold18pt7b`** — title and metadata  
- **`FreeSans18pt7b`** — summary body  

## Hardware

- [**Inkplate 5**](https://docs.soldered.com/inkplate) (Arduino board definition must target **Inkplate 5**, as enforced by `#ifndef ARDUINO_INKPLATE5`)

## Arduino IDE prerequisites

Install these libraries (**Library Manager** or vendor instructions):

| Library | Role |
|---------|------|
| **Inkplate** (Soldered) | Display, Wi‑Fi helpers, `drawTextBox`, etc. |
| **ArduinoJson** (6.x or 7.x) | JSON parsing for HN and chat API |
| **HNParser** (arduino-hn-parser) | Thin client for Hacker News Firebase JSON |

ESP32‑Arduino core supplies `HTTPClient`, `WiFi`, `WiFiClientSecure`, and sleep APIs.

Open the sketch: **`hn/hn.ino`**.

## Configuration

### Secrets (`Wi‑Fi` + NVIDIA API key)

Sensitive values live in **`hn/secrets.h`**, which is **git‑ignored**.

1. Copy the template:

   ```bash
   cp hn/secrets.example.h hn/secrets.h
   ```

2. Edit **`hn/secrets.h`** and set **`ssid`**, **`pass`**, and **`kNvidiaApiKey`**.

If `secrets.h` is absent, **`hn/secrets.example.h`** is compiled instead (with placeholders) so the sketch still builds; the firmware will **not work** until your real **`secrets.h`** is present.

See also `WIFI_TIMEOUT` (defaults to **30 s** in `hn/hn.ino`; override with `-DWIFI_TIMEOUT=…` when building).

### Other options (inside `hn/hn.ino`)

Non-secret Integrate defaults stay in the sketch:

1. **`kNvidiaChatUrl`** — NVIDIA chat completions endpoint (**`/v1/chat/completions`** on Integrate by default).
2. **`kNvidiaModel`** — model slug accepted by that endpoint.

Treat **`secrets.h`** and built firmware binaries as **secrets**: do not publish them, rotate API keys after any leak, and keep **`hn/secrets.h`** out of commits (see `.gitignore`).

## Tunables (behavior)

Rough map of useful constants near the top of `hn/hn.ino`:

| Constant | Effect |
|----------|--------|
| `kHoursBetweenDisplayRefresh` | Hours between wakes / full refresh cycles |
| `kWifiRetrySleepSeconds` | Deep sleep interval after Wi‑Fi failure |
| `fetchTopStoryLast24h(..., 50)` (last argument) | Max story IDs inspected from **top stories** |

Layout (margins, title rule position, summary band, line spacing) is controlled by constants such as `kScreenMargin`, `kTitleInsetX`, `kTitleLineSpacingPx`, `kSummaryAboveMetaPx`, `kSummaryLineSpacingExtraPx`, etc.

## External services

- **Hacker News** — public read-only Firebase JSON (`hacker-news.firebaseio.com`).
- **NVIDIA Integrate / NIM chat completions** — summarizes the winning story URL via HTTPS POST (non‑streaming payload in firmware).
- **NTP** — `pool.ntp.org`, `time.google.com`.

## Troubleshooting

- **Board menu** must be **Inkplate 5**; otherwise the sketch fails compilation with `#error "Select Soldered Inkplate5…"`.
- **Serial** at **115200** logs HN retries and summary HTTP issues.
- If HN repeatedly fails after **15** attempts, the device shows an error headline and sleeps on the Wi‑Fi retry timer.
- E‑paper will **refresh twice** when a summary is fetched: once for **“Generating summary…”**, once for the final text.
- If you see **`#warning`** about **`secrets.h`**, copy **`secrets.example.h`** → **`secrets.h`** and fill in credentials.


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

Edit the top of `hn/hn.ino`:

1. **`ssid`** / **`pass`** — Wi‑Fi credentials (`WIFI_TIMEOUT` defaults to 30 s; override with `-DWIFI_TIMEOUT=…` if needed).
2. **`kNvidiaChatUrl`** — NVIDIA chat completions endpoint (default: Integrate **`/v1/chat/completions`**).
3. **`kNvidiaApiKey`** — **Bearer** API key for that service.
4. **`kNvidiaModel`** — model slug accepted by Integrate.

**Security:** the sketch ships with credentials inlined for convenience during development. For any shared or production repo, prefer **secrets outside source** (ignored header, `--build-property`, CI secrets, etc.) and **rotate** keys if they were ever pushed or bundled in a firmware binary you do not fully control.

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

## License

This project is released under the **MIT License** — see [`LICENSE`](LICENSE).

Copyright © 2026 Adrián Matejov.

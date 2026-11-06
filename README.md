# Inkplate Hacker News

Firmware for **[Soldered Inkplate 5](https://docs.soldered.com/inkplate)** that periodically shows **the highest-scoring Hacker News story from the last 24 hours**, with an **AI-generated summary** of the linked article rendered on e-paper alongside the headline, score, and submission time.

## What it does

1. Connects to Wi‑Fi and syncs **UTC** over NTP (needed for the 24 h window).
2. Fetches [`/v0/topstories`](https://github.com/HackerNews/API) from the HN Firebase API via **HNParser**, walks up to **50** IDs, and picks the **`story`** with the **best score** whose submission timestamp falls in **`[now − 24h, now]`**.
3. Draws title, separators, footer metadata (**points** and **timestamp**), and **“Generating summary…”** while a **Cursor Cloud Agent** reads the article URL and summarizes it. The agent runs on a Cursor-hosted VM with internet access, so it fetches the page itself instead of guessing from the URL.
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
| **ArduinoJson** (6.x or 7.x) | JSON parsing for HN and Cursor API |
| **HNParser** (arduino-hn-parser) | Thin client for Hacker News Firebase JSON |
| **CursorAgent** (arduino-cursor-agent) | Cursor Cloud Agents API client (summaries) |

ESP32‑Arduino core supplies `HTTPClient`, `WiFi`, `WiFiClientSecure`, and sleep APIs.

Open **`hn/hn.ino`**.

## Configuration

### Secrets (`Wi‑Fi` + Cursor API key)

Sensitive values live in **`hn/secrets.h`**, which is **git‑ignored**.

1. Copy the template:

   ```bash
   cp hn/secrets.example.h hn/secrets.h
   ```

2. Edit **`hn/secrets.h`** and set **`ssid`**, **`pass`**, and **`kCursorApiKey`** (create one at [Cursor Dashboard → API Keys](https://cursor.com/dashboard); your plan must include Cloud Agents).

If `secrets.h` is absent, **`hn/secrets.example.h`** is compiled instead (with placeholders) so the sketch still builds; the firmware will **not work** until your real **`secrets.h`** is present.

See also `WIFI_TIMEOUT` (defaults to **30 s** in `hn/hn.ino`; override with `-DWIFI_TIMEOUT=…` when building).

### Other options (inside `hn/hn.ino`)

- **`kCursorModel`** — Cloud Agent model id (default **`claude-sonnet-5-5`**, Claude Sonnet 5.5; list valid ids with `curl -u YOUR_KEY: https://api.cursor.com/v1/models`).

The summary prompt, poll interval, timeout, and agent cleanup (archived after each run) are set in **`hn/summary_client.cpp`**.

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
- **Cursor Cloud Agents API** (`api.cursor.com/v1/agents`) — one no-repo agent run per wake summarizes the winning story URL. The firmware creates the agent, polls its run every 10 s (up to 5 min), and archives it afterwards. Each run is billed as Cloud Agent usage on your Cursor plan.
- **NTP** — `pool.ntp.org`, `time.google.com`.

## Troubleshooting

- **Board menu** must be **Inkplate 5**; otherwise the sketch fails compilation with `#error "Select Soldered Inkplate5…"`.
- **Serial** at **115200** logs HN retries and summary agent failures (`Summary agent failed: …` with the HTTP status or final run status).
- A summary takes roughly **tens of seconds to a few minutes** (cloud VM start + page fetch); **“Generating summary…”** stays on screen meanwhile.
- If HN repeatedly fails after **15** attempts, the device shows an error headline and sleeps on the Wi‑Fi retry timer.
- E‑paper will **refresh twice** when a summary is fetched: once for **“Generating summary…”**, once for the final text.
- If you see **`#warning`** about **`secrets.h`**, copy **`secrets.example.h`** → **`secrets.h`** and fill in credentials.


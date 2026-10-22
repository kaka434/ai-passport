<p align="right">
  <a href="football-live-board.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Football Live Board

A live-score board developed on the `feature/football-live-board` branch on top of
the `main` baseline. The device pulls football matches over Wi-Fi from the public
[Dongqiudi](https://www.dongqiudi.com/match) score feed and renders them with a
purpose-built Chinese UI. Provisioning happens entirely on the panel: pick the
Wi-Fi row on the settings screen, choose a scanned SSID, type the password on the
on-screen keyboard, then long-press OK to connect. Networks that worked once are
remembered, so the password is only typed the first time.

The on-screen page title reads "Match Board" in Chinese; the firmware file keeps its
original `FoloToy-AI-Passport-full.bin` name.

> **Verification status.** Host side: seven application logic suites (model, feed
> parsing, navigation, configuration, keyboard, known networks, match detail), the
> font coverage gate and the repository checks all pass. ESP-IDF 5.5.3 builds cleanly
> and produces a merged image that flashes from 0x0; `verify_firmware.py` confirms the
> bootloader, partition table and application segments line up. **Device side is
> verified** on an ESP32-C3 replica board with an ST7789P3 240×320 panel, three ADC
> keys and no PSRAM: Wi-Fi association and its persistence across reboots, access
> point scanning, SNTP time sync, the TLS handshake, list refresh over the full
> 401-match response, match-detail parsing at every observed payload size
> (187 bytes not-yet-started through 37 KB finished), how the Chinese glyphs look on
> the panel, key navigation and the provisioning flows. **Not measured**: long-run
> stability, power draw and battery readings.

## Screens and controls

Six screens: two main screens, one match detail screen, two on-screen Wi-Fi setup
screens and one phone-provisioning screen. There is no tab bar and no focus card;
the score screen is just a column of matches, two lines each.

| Screen | Contents |
| --- | --- |
| Scores | One column of matches: live first, then today's fixtures, then today's finished matches. Each row shows league and state on the first line, then home team, score and away team on the second |
| Detail | Page 1: score bar plus a goal timeline (home on the left, away on the right, minute in the middle; penalties marked, own goals marked). Page 2: formations plus eight statistics |
| Settings | Wi-Fi setup, phone setup, IP address, refresh interval, auto screen-off, brightness, refresh now, clear credentials |
| Networks | Scanned SSIDs; previously used networks are pinned to the top and tagged as saved |
| Password | A 6x7 on-screen keyboard for the selected secured network |
| Phone | Hotspot name, password and address, plus a two-step hint; the screen mirrors the connection state |

The score screen shows five rows at a time; longer lists scroll with up/down, and
the footer shows the position, the OK hint and the time of the last successful
refresh. The detail screen shows eight goals at a time and says how many are left
over when there are more.

**When today has no whitelisted match** the score screen switches to the nearest
upcoming match day instead of showing an empty list. An amber banner appears above
the list: a short label on the left, and on the right the date plus how many days
away it is.

```text
[next match day]                   10-24 · 3 days away
```

The list then holds that day's fixtures and drops from five rows to four to make
room for the banner. For the first two days the count reads "tomorrow" and "the day
after tomorrow" rather than a number; from the third day on only the month-day is
shown. The left label needs the text width plus the ellipsis: DOTS mode reserves the
ellipsis cell before laying out, so five 12 px ideographs need a 72 px label and a 70
px one truncates to "Next ma...".

Seeing any other day - including tomorrow - is a deliberate page rather than an
automatic jump: press Up on the first row or Down on the last one. If none of the next seven days has a match, the empty state says there are
no matches in the coming week.

| Button | Scores | Detail | Settings | Networks | Password | Phone |
| --- | --- | --- | --- | --- | --- | --- |
| Up/down click | Move the selection; pressing Up on the first row or Down on the last one flips one day | Flip between goals and statistics | Move the selection | Move the selection | Move the keyboard cursor | Nothing |
| OK click | Open the selected match | Fetch this match again | Run the selected row | Pick this network | Press the current key | Restart the hotspot |
| OK long press | Go to settings | Back to scores, same match | Go back to scores | Back to settings | Submit and connect | Stop the hotspot and go back |

Refresh now moved off the score screen (OK now opens the detail screen) to row four
of the settings screen; the list still refreshes itself every 60, 120 or 300 seconds.

The first settings row reports the **link** only: it shows the SSID once an IP is
assigned and says the connection failed only when there is no link. Fetch and time
sync failures are **data** problems and are explained by the empty state on the
score screen instead. Keeping them apart matters because a fetch can fail while the
link is perfectly healthy, and collapsing both into "connection failed" produces a
screen that shows an address and a failure at the same time.

After two minutes without input the backlight drops to 18%; any button restores it
to 100%.

## Network setup

There are two paths, and the first one is recommended.

### Phone web setup (recommended)

No app to install, any phone with a browser works, and the password is typed on a
real keyboard:

1. Long-press OK to reach the settings screen, select the second row and press OK.
2. The device turns itself into a hotspot named `FoloToy-Setup` with password
   `folotoy123` (WPA2 needs at least eight characters). The screen shows the
   hotspot name, the password and the address `192.168.4.1`.
3. Join that hotspot on the phone and open `http://192.168.4.1`.
4. Tap scan in the page, wait a few seconds for the list, pick the target network,
   type the password and submit.
5. The device starts connecting. The page polls every 500 ms and reports success;
   the screen switches to connected as well, then closes the hotspot and returns
   to settings after eight seconds.

The station interface keeps running while the hotspot is up (the radio is in
`WIFI_MODE_APSTA`), so joining the hotspot does not disturb the upstream link.
Twenty seconds without a connection counts as a failure; both the screen and the
page say so, OK retries and a long press closes the hotspot.

### On-screen setup (fallback)

The device can still be provisioned without a phone:

1. Long-press OK to reach the settings screen, select the first row and press OK.
2. The device scans immediately; access points are listed strongest first, with
   previously used ones pinned to the top and tagged as saved.
3. Select a network with up/down:
   - saved: the password is already local, OK connects straight away;
   - open: connects straight away;
   - secured: opens the password screen.
4. On the password screen a long press submits and connects, then returns to
   settings, which shows the connection state and finally the SSID.
5. On the network list a long press backs out without connecting.

The keyboard has 42 keys in six columns and seven rows: `a-z`, `0-9`, `.`, `-`, `_`,
`@`, plus backspace and a case toggle. The password field echoes only the last
character and masks the rest with `*`. Once the link is up the password is written
to NVS, so that SSID can be picked directly next time.

### Reusing a scan

When the device boots without saved credentials, `app_net_start()` runs one scan
in the background. Opening the network list afterwards reuses that result if it is
less than five minutes old instead of scanning again, so the list appears with
almost no delay and the radio does not have to sweep twice.

The USB serial console remains as a fallback for batch flashing and remote
diagnosis:

```text
wifi set <ssid> <password>     save and connect (empty password means open network)
wifi status                    show the saved SSID and interval, never the password
wifi clear                     drop the credentials
board interval 120             refresh period: 60/120/300 seconds, default 120
board refresh                  refresh immediately
board detail <match_id>        fetch one match detail (diagnostics, no buttons)
```

Credentials are stored as plain text in the `fb_board` NVS namespace, and the five
most recent networks live in another record of the same namespace. This board has
no NVS encryption, so treat it as an object that holds a Wi-Fi password.

## Data source

Two requests. The list endpoint:

```text
GET https://www.dongqiudi.com/magicball/v1/list/match_list
      ?language=zh-CN&cmp_type=soccer&tab_type=all
Referer: https://www.dongqiudi.com/match
```

- `cmp_type=soccer` restricts the feed to football; the endpoint also serves
  basketball.
- **The `Referer` header is mandatory.** Without it the site answers HTTP 567 with
  an HTML error page.
- `tab_type`, `limit`, `size`, `page`, `status`, `date` and `start` have no effect:
  the endpoint always returns the same full snapshot. Measured responses range from
  0.5 to 1.6 MB and 400 to 1500 matches depending on where the season is.
- **It only covers "now through 23:30 today"** and contains no future dates at all.
  Tomorrow's fixtures need the other endpoint described below.
- Team and competition names are **already Chinese**, so no English-to-Chinese
  mapping table is needed.

Because the response reaches 1.6 MB and the C3 has no PSRAM, parsing is streaming:
`esp_http_client_read` hands 4 KB at a time to `app_feed_write`, and a small state
machine accumulates one match object (about 1.2 KB) into a fixed 2.5 KB buffer,
parses it, then discards it. Memory use is independent of the response size. The
largest object across a real 401-match response measured 1,918 bytes, leaving about
thirty percent headroom.

Read the status code **after** `esp_http_client_fetch_headers()`, never before.
`esp_http_client_open()` only connects and sends the request; not a byte of the
response has arrived, so the status code is still the zero-initialised field. Compare
that against 200 and every healthy response is rejected, which shows up in the log as
the list fetch reporting `HTTP 0` while the TLS handshake, the certificate check and
the server's 200 are all perfectly fine - an easy afternoon lost to blaming the
network.

### What is displayed

The board answers "what is on today, and what already finished today", so
`feed_accept()` applies three filters:

1. **Competition**: only the English Premier League, La Liga, Serie A, Bundesliga,
   Ligue 1, the Chinese Super League, China League One, the Champions League and the
   Europa League survive. The other thousand-plus entries in the feed (Europa
   Conference League, Championship, League Two, Brasileirao, J-League, MLS and so on)
   are dropped here. Matching strips a trailing "league" word and then compares for
   equality, so a competition name that carries that suffix still matches, while a
   longer name that merely embeds the same two characters is not mistaken for La Liga.
2. **Date**: only matches whose kick-off falls on the day being shown in China
   Standard Time. Live matches are exempt, because a game that kicked off at 23:30 and
   is still running past midnight is exactly what the viewer wants at that moment.
3. **Per-category caps**: 6 live, 20 of the day's fixtures, 14 of the day's finished
   matches, 40 in total, which is exactly `APP_MAX_MATCHES`. Keeping the sum of the
   caps at or below the list capacity is deliberate: a streaming parser cannot go
   back and evict, so otherwise finished matches arriving early would crowd out the
   fixtures that come later in the response. A day that is not today is single-purpose
   and gets the whole list for its one category: a past day is all finished, a future
   day is all fixtures.

The caps were measured, not guessed: a Saturday can hold 30 whitelisted fixtures
(Premier League through Ligue 1 plus the two Chinese leagues), and a Champions League
match day adds nine or ten early-morning ties on top. The previous 24-row list with a
four-match finished cap truncated both.

Finished matches stay at the end of the list with their final score, so after a
full day the score screen doubles as the day's fixture list and results.

### The dawn window

`match_list` anchors its window on the **UTC** day: a request at 14:11 CST returns
07:30 on the current Beijing day through 09:06 the next one. Beijing 00:00 to 07:30 is
never in that window, and every Champions League and Europa League tie kicks off
inside it, so before this fix the board simply never showed them. `start` and `date`
parameters on `match_list` are ignored, so the gap has to be filled from a second
source:

```text
GET https://www.dongqiudi.com/magicball/v1/list/schedule_list
      ?language=zh-CN&tab_type=result&cmp_type=soccer&start=YYYY-MM-DD%20HH:00:00
```

`tab_type=result` covers the 24 hours from `start` and holds only finished matches, so
it supplies exactly the missing segment. The rows are appended into the list that
`match_list` already filled - `app_feed_init_append()` keeps what is there and only
accepts finished matches - and de-duplicated by match id.

`schedule_list` cannot replace `match_list`: none of its tabs (`result`, `fixture`,
`all`, `live`, `playing`) ever returns a match that is in progress, so a live match
would disappear from the board. It is a supplement, not a substitute.

The extra request costs about 220 KB, so it is throttled: before 07:00 every refresh
tops the segment up (the last early kick-off may still be running), and after 07:00
the segment is fetched once and then frozen until the day rolls over.


### Next match day

When a day has no whitelisted match at all - international breaks and midweek gaps
both do this - the board does not sit on an empty screen. It looks forward for the
nearest day that has one. `match_list` cannot supply that, so a second endpoint is
used:

```text
GET https://www.dongqiudi.com/magicball/v1/list/schedule_list
      ?language=zh-CN&tab_type=fixture&cmp_type=soccer&start=YYYY-MM-DD%20HH:00:00
Referer: https://www.dongqiudi.com/match
```

- **`start` is UTC** and the response covers the 24 hours from that instant. To get
  one Beijing day, set `start` to that day's 00:00 CST minus eight hours, which is
  `16:00:00` on the previous day.
- `tab_type=schedule` returns zero rows; only `fixture` works.
- The space must be percent-encoded as `%20`. A literal `+` returns the same data
  but `%20` is safer.
- A full day holds 130 to 1300 matches; after the competition filter it often comes
  down to single digits.

Probing is **lazy**: when today has matches this request is never sent, so the
steady-state traffic cost is zero. Only a genuinely empty day triggers a walk from
day 1 to day 7, stopping at the first day that has a whitelisted match. The result,
including "nothing found", is cached for 30 minutes - re-probing every empty day
would download several megabytes for nothing.

The probe publishes its state before the network call and bumps the revision, so
the UI can show that it is searching; otherwise a few seconds of waiting looks like
a hang. A total network failure and "genuinely no matches" are treated differently:
the former keeps the old data and reports an error rather than claiming the week is
empty.

Probing is only a fallback now. Once the dawn window is filled, a day almost always
has something on it, so the probe no longer fires during the day and tomorrow's ties
would be invisible. They are reached by paging instead - see below.

### Paging across days

`app_net_set_view_day()` selects which day the board shows: `0` today, `-1` the
previous day, `1` to `7` the next one to seven days. The score screen flips it by
pressing Up on the first row or Down on the last one, and the serial console has
`board day [n]` for the same thing.

- A day other than today is fetched from `schedule_list` with `tab_type=result` (past
  days, finished matches only) or `tab_type=fixture` (future days, fixtures only).
- Those days barely change, so each is fetched once and cached against its CST day
  start; paging back to a day that is already loaded publishes nothing and does not
  even flash "fetching".
- `day_offset` stays `0` for a paged day. `day_offset` means "today was empty and this
  is the day we jumped to", which is a different thing from "the user asked for this
  day"; the header reads `view_day` to decide between `Replay 10-21`, `Tomorrow
  10-23`, `The day after 10-25` and `N days later`.
- A refresh takes seconds, so a user can page away while a request is in flight.
  Every publish therefore compares `s_view_day` with the view captured at the start of
  the fetch and drops the result when they differ, then re-wakes the fetch task. Without
  that guard the screen showed the previous view's list for a few seconds before
  flipping to the requested one.

The dawn segment belongs to today's view only, and `s_parsed` is a single buffer, so
entering another day invalidates it; switching back to today re-fetches the segment
instead of showing a list that silently lost its early kick-offs.

HTTPS uses the ESP-IDF certificate bundle, so the system clock must be valid before
the first request. Once an IP is assigned, SNTP runs against `ntp.aliyun.com`,
`cn.pool.ntp.org` and `pool.ntp.org`. If it fails, the UI says so explicitly
instead of quietly skipping certificate validation.

### Match detail

Fetched only when the user opens a match, as a separate request:

```text
GET https://api.dongdianqiu.com/data/overview/match/{match_id}
Referer: https://topic.dongdianqiu.com/
```

This response measured 6-38 KB across a 21-match sample, the largest being 37,943
bytes. Parsing has to stream: the board runs with roughly 45 KB of free heap and TLS
wants 20 KB of that, so a whole-body buffer does not fit. The first attempt used a
36 KB temporary buffer and the device answered "out of memory" on every match - the
heap was fragmented down to a 10 KB largest free block.

`app_detail_parse_stream()` therefore takes a read-a-chunk callback and keeps a 1 KB
refillable window underneath. The parser is a pure forward scan, so window
boundaries are invisible to it; the host test feeds the same response in chunks of
1, 3, 7, 64, 511, 1024 and 4096 bytes and requires results identical to the
whole-buffer path. Memory use is independent of the response size.

The detail task and the list refresh share one HTTP lock so two TLS sessions never
handshake at the same time, which the C3 heap cannot afford.

Fields used:

| Data | Field |
| --- | --- |
| Goals | `events.{minute}.teamAEvents[]` / `teamBEvents[]`, split by `code`: `G` goal, `PG` penalty, `OG`/`UG` own goal; everything else (`YC`, `RC`, `SI`, `SO`) is dropped |
| Minute | carried by the outer object's key (`"45+2"`); the event itself only has `minute_extra` |
| Running score | the event's `score`, for example `"2-1"` |
| Formations | `statistics.team_A.formation` / `team_B.formation` |
| Statistics | `statistics.list[]`, each `{type, team_A:{value}, team_B:{value}}` |

The detail endpoint escapes **every** Chinese character in the response as `\uXXXX`,
including player names, so the escape branch has to decode real code points. The list
endpoint serves raw UTF-8, and for a while the parser assumed that for both: names
came out as `?` in the goal timeline. The decoder now handles one to four byte
encodings and surrogate pairs (an unpaired surrogate degrades to U+FFFD), and
`app_fonts_sanitize()` drops zero-width format characters (U+200B and friends) instead
of rendering them as a lone question mark.

Two parsing traps, both pinned by host tests:

- `minute` arrives as the string `"45"` in some matches and as the number `45` in
  others, so it is read as text.
- Penalties are often written in `reason` rather than as a `PG` code, and `reason`
  comes **before** `code` in the object. The hint has to be remembered and applied
  after the whole object is parsed, otherwise the later `code=G` overwrites it.

A third one is about types rather than values: `events` and `statistics` do not have
a fixed type. They are objects `{}` when there is data, **empty arrays `[]`** when
there is not (a not-yet-started response is 187 bytes:
`{"status":0,"match_status":"Fixture","events":[],"statistics":[]…}`), and
occasionally `null`. All three are accepted; object-only parsing failed every
not-yet-started match. A match paused at half time can even mix the two — `events`
already an object while `statistics` is still `[]` — so it looks like it has events
but the whole response dies on the statistics section.

**Stadium name and match-day weather are not shown.** Neither field exists in this
response and `matchFormation` in `app/detail` is `null`; without a stadium there are
no coordinates to look weather up with. Showing less is better than inventing data.

Formations are empty strings for many matches (only those with a confirmed starting
line-up carry one); the UI prints a dash instead of leaving the row blank.

## Fonts

Four subsets generated from Source Han Sans by `tools/gen_fonts.py`:

| Size | Coverage | Used for |
| --- | --- | --- |
| 12 px | ASCII, Latin-1, the whole GB2312 repertoire plus extra CJK characters seen in the feed (7630 code points) | league names, match state, footer |
| 15 px | same as above | team names, settings rows, SSID names, battery |
| 19 px | the 347 characters used by static UI text | header title, empty-state hints, setup page |
| 22 px | `0-9`, `-`, `v`, `s` | score |

The full inventory is the union of three sources: string literals in the
application sources, the whole GB2312 repertoire, and `assets/fonts/data_chars.txt`
harvested from a real response. GB2312 keeps the subset future-proof against new
club and competition names; the harvested file adds the Japanese kanji and variant
forms that GB2312 does not cover.

Characters outside the subset are rendered as `?` by `app_fonts_sanitize()` rather
than as an empty box. The only case in the current feed is one club name written in
Turkish; no Source Han Sans release carries that glyph, so this is a documented
degradation rather than a missed character. SSID names go through the same
sanitiser, so an access point with an emoji in its name shows `?`.

## LVGL memory

LVGL runs on its own built-in TLSF pool, sized by `CONFIG_LV_MEM_SIZE_KILOBYTES` and
currently 40 KB. It is a static array in DRAM: not shared with the system heap and
not extensible at run time.

Running out of it is worse than it sounds. A failed LVGL allocation does not return
`NULL`; it trips `LV_ASSERT_MALLOC`, and the default assert handler is `while(1)`.
The task watchdog is not configured to panic, so the board does not reboot - the
last frame stays on screen and every button goes dead. It looks exactly like a hang.

So the object count per screen has to stay modest. The password screen taught this
the hard way: 42 keys built as a panel plus a label meant 87 objects and 260-odd
local style properties on one screen, and the 24 KB pool failed to allocate against
a fragmented heap - selecting an SSID and pressing OK froze the device. A key is now
a single label with a background, 42 objects in total, and the pool went to 40 KB.

`build_body()` logs the numbers from `lv_mem_monitor()` after every rebuild. Check
that line after touching the UI instead of finding out on the device.

## Code map

```text
main/app_model.[ch]    match model, ordering, competition whitelist, date and time labels (host tested)
main/app_json.[ch]     streaming parser and the three display filters (host tested)
main/app_detail.[ch]   streaming match detail parser: goals, formations, statistics (host tested)
main/app_nav.[ch]      six-screen, three-button navigation state machine (host tested)
main/app_config.[ch]   config struct, refresh presets, input validation (host tested)
main/app_keyboard.[ch] on-screen keyboard model: 42 keys, cursor, case toggle (host tested)
main/app_known.[ch]    merge and lookup of remembered networks (host tested)
main/app_store.[ch]    NVS persistence (config plus network history)
main/app_net.[ch]      Wi-Fi STA, access-point scanning, SNTP, streaming HTTPS fetch, snapshot publishing
main/app_portal.[ch]   phone setup: SoftAP, HTTP server, embedded setup page, connect state machine
main/app_console.c     USB serial provisioning commands (fallback)
main/app_ui.c          all six screens (hand-drawn, not the demo test menu)
main/app_fonts.c       missing-glyph fallback to '?'
tools/gen_fonts.py     regenerate the font subsets from sources, GB2312 and a capture
tools/check_fonts.py   fail the gate on a stale inventory or a missing glyph
```

Threading: the button callback only runs the wake-key guard and enqueues an event.
An input task touches LVGL while holding `bsp_lvgl_lock()`. One network task owns
the radio and TLS and publishes results into a mutex-protected snapshot; a scan
task is woken only while the network list is open and runs a blocking
`esp_wifi_scan_start`; phone setup adds an HTTP server task and a 500 ms poll task
that are both torn down when setup ends; the 1-second UI timer copies the snapshot
when the revision changes. No LVGL object is ever touched outside the LVGL context
or without the lock. Radio bring-up is guarded by its own mutex so that neither the
network task nor the scan task can call `esp_wifi_init()` twice. The network task
stack is 8 KB (headroom for the mbedtls handshake) and the 40-match staging list
lives in static storage, not on the stack.

The wake-key guard is inherited from the baseline: after a deep-sleep button wake
the chip restarts with that key still held, the button driver sees it a few hundred
milliseconds later and classifies it as a long press, which would flip a screen
during boot. The guard swallows every button action until that key is released, and
re-syncs from the measured voltage once the driver is initialised.

Memory budget: 4 KB snapshot, 4 KB parser staging, 4 KB HTTP read buffer, about
3 KB of provisioning state and a 40 KB LVGL pool. Static DRAM is 191 KB
(195,508 bytes, 60.85% of 321,296), leaving roughly 123 KB of runtime heap for
Wi-Fi and TLS; Bluetooth is disabled to make room.

The same match list used to be stored four times - parser output, next-day probe
output, the published snapshot and the copy the UI pulled out - 14.9 KB in total.
The probe now reuses the parser buffer and publishes straight into the snapshot,
which saves 3.6 KB. Wi-Fi buffers went from 10/32/32 to 6/10/10 and the lwIP send
and receive windows from 5,760 to 4,096, costing a few extra round trips per
request. The dynamic Wi-Fi buffers are allocated per frame rather than held for the
lifetime of the driver, so that change mostly affects throughput and does little for
the largest free block - something only the device made clear.

The real runtime ledger, measured with `heap_caps_get_free_size`: Wi-Fi and lwIP
consume about 75 KB, a TLS session takes another 20 KB (16 KB inbound plus 4 KB
outbound), so during a detail request only 25 KB is free and the largest contiguous
block is 10 KB. That is the whole explanation for "the total looks sufficient but
the big allocation fails".

`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN` stays at the 16 KB default. Do not shrink it on
the theory that the certificate chain is only a few KB: the setting bounds a single
TLS record, not the chain. Measured against Dongqiudi (nginx), a 1.6 MB response
arrives in records of 16,401 bytes - 16,384 plaintext plus 17 bytes of TLS 1.3
overhead, matching nginx's default `ssl_buffer_size 16k`. At 8 KB the failure mode
is thoroughly misleading: the link comes up, an IP is assigned, the server answers
200, and then mbedtls rejects the eighth record as `INVALID_RECORD`, so the board
never loads a single match.

## Known limitations

- One refresh downloads 1.64 MB, so the presets start at 60 seconds and default to
  120. Shorter intervals cost battery and are more likely to be rate limited.
- Only today's matches in nine competitions are shown, plus the one day the user pages
  to. Widening the competition set means editing `ALLOWED_LEAGUES` and the date filter
  in `feed_accept()`.
- Team names are bounded by the 240 px panel: anything longer than about five
  ideographs is truncated with an ellipsis.
- Scanning is blocking, so the network list takes roughly 3 to 5 seconds to fill;
  the footer shows a scanning hint meanwhile.
- The on-screen keyboard covers 40 ASCII characters. A password containing Chinese
  characters or emoji still needs `wifi set` on the serial console.
- Score freshness depends on the upstream feed and the selected refresh interval.
- There is no automatic deep sleep. Idle time only dims the backlight to 18%, while
  Wi-Fi keeps fetching on the configured interval.

[![Github Repo](https://img.shields.io/badge/github-repo-blue?logo=github)](https://github.com/jayng9663/VN-Presence)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus)](https://en.cppreference.com/w/cpp/23)

# VN-Presence

Discord Rich Presence daemon for Visual Novels on Linux. Detects games launched via **Lutris** or **Steam**, looks them up on [VNDB](https://vndb.org), and shows the title, cover art, and total playtime in your Discord status.

### Voice channel

| With cover art | Cover suppressed (explicit) |
|:---:|:---:|
| ![Voice channel presence with cover](docs/images/voice_with_cover.png) | ![Voice channel presence without cover](docs/images/voice_no_cover.png) |

### User activity

| With cover art | Cover suppressed (explicit) |
|:---:|:---:|
| ![User activity with cover](docs/images/activity_with_cover.png) | ![User activity without cover](docs/images/activity_no_cover.png) |

---

## Features

- Detects games from **Lutris** (exact name from wrapper) and **Steam** (AppID → ACF → store name), including Flatpak Steam
- Fuzzy title matching against VNDB using trigram similarity + full-width character normalisation
- Fallback: searches the **release** endpoint and follows the relation back to the parent VN
- Suppresses explicit cover images (sexual ≥ 1.80 or violence ≥ 1.80, or fewer than 5 image votes) — title still shows
- Playtime from **Lutris DB** (`pga.db`) or **Steam VDF** (`localconfig.vdf`) — no API key needed
- Discord elapsed timer reflects **prior playtime + current session**, not just this session
- Editable `cache.csv` for aliases, hard-links, and SKIP entries — reloaded live while running
- `ignore.txt` with exact-match for short entries (< 4 chars) to prevent false positives like `sh`
- Shows the VN title in Discord's member list ("Playing サクラノ詩…"), the VNDB rating on cover hover, and a **Steam Store** button for Steam games
- Reconnects on its own when Discord restarts or starts after the daemon, and re-sends the current presence
- Backs off VNDB retries while offline (30 s, doubling up to 10 min) instead of retrying every poll

---

## How it works

### Main loop

Every 5 seconds the daemon runs detection, debounces the result, checks the cache, queries VNDB if needed, and updates Discord.

```mermaid
flowchart TD
    TICK([Every 5 s]) --> DET[Detect games<br>Lutris + Steam]
    DET --> FILT[Drop ignored and<br>no-match titles]
    FILT --> ANY{Any games?}
    ANY -- no --> CLR[Clear presence]
    ANY -- yes --> DEB{Stable for<br>2 polls?}
    DEB -- yes --> CUR{Shown VN<br>still running?}
    CUR -- no --> RES[Resolve candidates<br>earliest-started first]
    RES -- VN found --> SET[Update Discord presence]
    RES -- none --> CLR
    DEB -- no --> WAIT([Wait for next poll])
    CUR -- yes --> WAIT
    SET --> WAIT
    CLR --> WAIT
```

Each candidate is resolved like this, stopping at the first one that turns out to be a VN:

```mermaid
flowchart TD
    C[Candidate] --> LOOK{cache.csv}
    LOOK -- fresh hit --> USE[Use this VN]
    LOOK -- expired hit --> REF["Refresh by stored VNDB id<br>(keeps old data if it fails)"] --> USE
    LOOK -- "miss / alias" --> VNDB[Search VNDB<br>by title or alias]
    LOOK -- SKIP --> NEXT[Try next candidate]
    VNDB -- match --> SAVE[Save to cache.csv] --> USE
    VNDB -- no match --> NM[Skip title<br>until restart] --> NEXT
    VNDB -- network error --> BO["Retry after 30 s<br>doubling up to 10 min"] --> NEXT
```

---

### Detection

```mermaid
flowchart TD
    P[Scan /proc every 5 s] --> L{lutris-wrapper<br>in cmdline?}
    P --> S{SteamAppId<br>in environ?}
    L -- yes --> LN[Game name from<br>wrapper args]
    S -- "yes, not Proton<br>or a runtime" --> SN[Game name from<br>appmanifest_ID.acf]
    LN --> ST[Read starttime<br>/proc/pid/stat field 22]
    SN --> ST
    ST --> SORT[Sort candidates<br>by starttime ascending]
```

Lutris passes the game name explicitly as command-line arguments after `lutris-wrapper`, so the name is always exact. Steam injects `SteamAppId` as an environment variable into every game process — the daemon reads it from `/proc/<pid>/environ`, then finds the matching `.acf` file across all Steam library paths (including custom drives read from `libraryfolders.vdf`). Every distinct running AppID becomes its own candidate, so several Steam games running at once are all detected.

After all candidates are collected, the daemon reads **field 22** (`starttime`) from each process's `/proc/<pid>/stat`. Candidates are then **sorted ascending by starttime**, so the process that launched first is always tried first during VNDB resolution. This makes multi-candidate priority deterministic and reproducible across polls.

> [!NOTE]
> See the [/proc/\[PID\]/stat field reference](docs/proc_stat_fields.md) for a full breakdown of all stat fields.

> [!TIP]
> Run with `--verbose` to see candidates list in the debug output:
> ```
> [DEBUG] src/main.cpp:73 4 game candidate(s) found:
>   [DEBUG] src/main.cpp:75   [lutris] pid=2037324  name="終ノ空 remake"  starttime(clock ticks)=4906595
>   [DEBUG] src/main.cpp:75   [lutris] pid=2086738  name="X-Plane 12"  starttime(clock ticks)=4906701
>   [DEBUG] src/main.cpp:75   [steam-appid] pid=2037822  name="心象天儀本線 ~Per aspera ad astra~ Demo"  starttime(clock ticks)=4906857
>   [DEBUG] src/main.cpp:75   [lutris] pid=2038526  name="サクラノ詩－櫻の森の上を舞う－"  starttime(clock ticks)=4907260
> ```

---

### VNDB resolution

```mermaid
flowchart TD
    Q[Search query] --> VN[POST /kana/vn<br>top result by search rank]
    VN --> SIM{Title or alt title<br>similarity ≥ 0.35?}
    SIM -- yes --> MATCH[VnInfo returned]
    SIM -- no --> REL[POST /kana/release]
    REL --> FOUND{Release linked<br>to a VN?}
    FOUND -- yes --> ID[Fetch parent VN<br>by exact ID]
    ID --> MATCH
    FOUND -- no --> NOMATCH[No match<br>try next candidate]
```

Before the similarity check, full-width ASCII (`！２→!2`, `（→(`) is normalised to half-width so Japanese game names from Lutris match VNDB's stored titles. There is also a substring boost — if the query appears inside the returned title, the score is raised to at least 0.6, handling cases where the detected name is a short prefix of a long VNDB title.

---

### Cache TTL

VNDB results are stored persistently in `cache.csv`. On each cache hit the daemon compares the entry's `cached_at` timestamp against `VNDB_CACHE_TTL`. If the entry is older than the TTL, it is treated as expired and re-fetched from VNDB **by its stored id** (not by title, so hard-links and earlier matches are never replaced), updating the stored data and timestamp. If that refresh fails (offline, VNDB down, or no match), the old cached data is still used.

```mermaid
flowchart TD
    HIT[Cache hit] --> AGE{Older than<br>VNDB_CACHE_TTL?}
    AGE -- no --> USE[Use cached VnInfo]
    AGE -- yes --> REQUERY[Re-fetch from VNDB<br>by stored vndb_id]
    REQUERY -- ok --> UPD[Update cache.csv<br>and cached_at] --> USE
    REQUERY -- "failed (keep old data)" --> USE
```

This means ratings, cover images, and release dates in the cache are automatically refreshed over time without any manual intervention. The log line when a re-query fires looks like:

```
[INFO] Cache expired for "..."  age=1442min/1440min — re-querying VNDB (v562)
```

---

### Playtime → Discord elapsed timer

```mermaid
flowchart TD
    PROC["proc_start = btime<br>+ starttime ÷ CLK_TCK"] --> SRC{Source?}
    SRC -- lutris --> LDB[Lutris pga.db<br>hours, by VN title]
    SRC -- steam-appid --> VDF[Steam localconfig.vdf<br>minutes, by AppID]
    LDB --> PT{Playtime<br>found?}
    VDF --> PT
    PT -- yes --> TS["start_ts =<br>proc_start − playtime"]
    PT -- no --> TS0["start_ts =<br>proc_start"]
    TS --> D1[Timer shows<br>session + prior playtime]
    TS0 --> D2[Timer shows<br>this session only]
```

---

### Cache file (`cache.csv` / `cache.db`)

Located at `~/.config/vn-discord-rpc/cache.csv` (default) or `cache.db` when `CACHE_USE_DB = true`. Reloaded automatically whenever the file changes on disk.

| Column | Purpose |
|---|---|
| `key` | Detected Visual Novels name (the search term) |
| `alias` | Redirect this key to a different search term |
| `vndb_id` | `v562`, `v67`, `SKIP`, or empty |
| `title` | Romanised VNDB title (auto-filled) |
| `alt_title` | Original script title, e.g. Japanese (auto-filled) |
| `image_url` | Cover image URL (auto-filled, blank if suppressed: explicit, or fewer than 5 image votes) |
| `image_sexual` | VNDB sexual rating 0–2 (auto-filled) |
| `image_violence` | VNDB violence rating 0–2 (auto-filled) |
| `image_votecount` | Number of VNDB votes behind the image ratings (auto-filled) |
| `rating` | VNDB community rating 0–100 (auto-filled) |
| `released` | Release date (auto-filled) |
| `cached_at` | Unix timestamp of last write (auto-filled) |

**Examples:**

```csv
# Alias — fix wrong or garbled detection
Nice boat!,School Days,,,,,,,,

# Skip — suppress presence for this title
妹ぱらだいす！２,,SKIP,,,,,,,

# Hard-link — bypass VNDB search, point directly to an entry
# (title, cover, etc. are filled in from VNDB by id; the id you set is kept)
My VN Title,,v67,,,,,,,
```

---

### Ignore list (`ignore.txt`)

Located at `~/.config/vn-discord-rpc/ignore.txt`. One entry per line, `#` for comments. Reloaded automatically while running.

Matching rules:
- Entry **< 4 characters**: exact match (case-insensitive) — (ie. prevents `sh` matching `Higurashi`)
- Entry **≥ 4 characters**: case-insensitive substring match

The default file includes common false-positives: Steam runtimes, Proton, Wine helpers, and launchers.

> [!NOTE]
> The daemon never writes to ignore.txt itself. If a title has no VNDB match, it is skipped
> until the daemon restarts, then checked once more. Network or server errors (offline, HTTP 429/5xx)
> don't count as no match — the title is retried later, waiting 30 s and doubling up to 10 min.
> To stop a title from ever being queried, add it to ignore.txt or add a SKIP entry to cache.csv.

---

## Configuration (`include/config.hpp`)

> [!CAUTION]
> Be caution on changing `IMAGE_SEXUAL` and `IMAGE_VIOLENCE` thresholds in `include/config.hpp`.
> Those thresholds are set to `1.80` (slightly below VNDB's **Explicit** level of `2.00`)
> because VNDB image ratings are user-reported and may be underrated by a small margin —
> the 0.20 buffer ensures borderline explicit covers are still suppressed.
> Raising the threshold above `2.00` would cause explicit (pornographic) cover art to appear
> in your Discord status, visible to everyone on your friends list. Displaying explicit
> content in Discord Rich Presence may violates [Discord's Terms of Service](https://discord.com/terms)
> and **may result in a permanent account ban**.

| Constant | Default | Description |
|---|---|---|
| `DISCORD_APP_ID` | `1482345564698841189` | Discord application ID |
| `DISCORD_STATUS_DISPLAY` | `2` | What the member list shows after "Playing": `0` app name, `1` state line, `2` VN title |
| `RPC_SHOW_RATING` | `true` | Append the VNDB rating to the cover's hover text |
| `IMAGE_SEXUAL` | `1.80` | Maximum threshold value for sexual before being suppressed |
| `IMAGE_VIOLENCE` | `1.80` | Maximum threshold value for violence before being suppressed |
| `IMAGE_VOTECOUNT` | `5` | Covers with fewer image votes than this are suppressed (rating not trusted yet) |
| `VNDB_MIN_SIMILARITY` | `0.35` | Minimum trigram score to accept a match |
| `POLL_INTERVAL` | `5s` | How often to scan for running processes |
| `VNDB_CACHE_TTL` | `24h` | How long a `cache.csv` or `cache.db` entry is valid before re-querying VNDB |
| `VNDB_RETRY_MIN` / `VNDB_RETRY_MAX` | `30s` / `600s` | Retry delay after a VNDB network/server error (doubles each time) |
| `STABLE_TITLE_POLLS` | `2` | Polls a title must be stable before acting |
| `CACHE_USE_DB` | `false` | **[Experimental]** Use SQLite (`cache.db`) instead of CSV (`cache.csv`) |

---

## Building

> [!IMPORTANT]
> Initialise submodules before building — the three submodules (discord-presence,
> nlohmann/json, cpp-logger) are not downloaded automatically. The first CMake configure
> also needs network access: discord-presence fetches fmt and glaze through CPM.
> ```bash
> git submodule update --init --recursive
> ```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Dependencies

| Library | Purpose |
|---|---|
| [discord-presence](https://github.com/EclipseMenu/discord-presence) | Discord Rich Presence (modern C++ rewrite) |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing |
| [cpp-logger](https://github.com/jayng9663/cpp-logger) | Logging |
| [libcurl](https://curl.se/libcurl/) | HTTP requests to VNDB API |
| [libsqlite3](https://www.sqlite.org/) | Read Lutris playtime database |

---

## Usage

> [!TIP]
> Run with `--verbose` if a title is not being detected or matched — the debug output shows
> exactly which process was found, what VNDB returned, and the similarity score.

```bash
./build/VN-Presence            # normal
./build/VN-Presence --verbose  # debug logging
./build/VN-Presence --log-file /path/to/vn.log  # also log to a file (default: ./vn-presence.log)
./build/VN-Presence --help
```

Launch a game through **Lutris** or **Steam**, and the daemon will detect it automatically. Press `Ctrl+C` to quit cleanly.
Discord doesn't need to be running first — the daemon waits for it and reconnects if it restarts.

> [!NOTE]
> Discord only shows Rich Presence buttons (**View on VNDB**, **Steam Store**) to other people,
> not on your own profile.

### Run at login (systemd user service)

`cmake --install` also installs a user unit pointing at the installed binary:

```bash
sudo cmake --install build                  # binary → /usr/local/bin, unit → /usr/local/lib/systemd/user
systemctl --user daemon-reload
systemctl --user enable --now VN-Presence.service
journalctl --user -u VN-Presence -f         # follow the log
```

To install somewhere else, set the prefix when configuring (`cmake -B build -DCMAKE_INSTALL_PREFIX=...`)
so the unit's `ExecStart` path matches.

---

## File locations

| File | Path |
|---|---|
| Cache (CSV) | `~/.config/vn-discord-rpc/cache.csv` |
| Cache (SQLite, experimental) | `~/.config/vn-discord-rpc/cache.db` |
| Ignore list | `~/.config/vn-discord-rpc/ignore.txt` |
| Lutris DB | `~/.local/share/lutris/pga.db` |
| Steam VDF | `~/.local/share/Steam/userdata/<id>/config/localconfig.vdf` (also checked under `~/.steam/steam` and Flatpak's `~/.var/app/com.valvesoftware.Steam/.local/share/Steam`) |

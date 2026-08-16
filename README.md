# duckdb-livetennis

A [DuckDB](https://duckdb.org) community extension for the
[Live Tennis API](https://livetennisapi.com): query live tennis matches,
upcoming fixtures and the player roster as SQL table functions. Coverage is
every tour equally — ATP, WTA, Challenger, ITF and the junior Grand Slam draws.

Built from the official
[DuckDB extension template](https://github.com/duckdb/extension-template)
(C++, DuckDB v1.5.4).

## Installation

Once available in the community extension repository:

```sql
INSTALL livetennis FROM community;
LOAD livetennis;
```

## Authentication

Every function needs a Live Tennis API key. A **free key** (no card) is
self-serve at <https://livetennisapi.com/subscribe/free>.

Three ways to provide it, checked in this order:

```sql
-- 1. DuckDB secret (recommended)
CREATE SECRET my_tennis_key (TYPE livetennis, API_KEY 'ltk_...');

-- 2. Session setting
SET livetennis_api_key = 'ltk_...';
```

```sh
# 3. Environment variable
export LIVE_TENNIS_API_KEY=ltk_...
```

The key is sent as `Authorization: Bearer <key>`. In `duckdb_secrets()` the
key is redacted.

## Functions

| Function | API endpoint | Returns |
|---|---|---|
| `live_tennis_matches()` | `GET /matches?status=live` | Matches currently in play, with the latest score |
| `live_tennis_fixtures()` / `live_tennis_fixtures(tour)` | `GET /fixtures` | Upcoming scheduled fixtures, earliest first. `tour` is one of `atp`, `wta`, `challenger`, `itf`, `juniors` |
| `live_tennis_players(search)` | `GET /players?search=` | Player search by name (ranked players first) |

All three paginate through the API automatically and return the full result
set. `NULL`s in the output are the API's own nulls — real "unknown / not
applicable" states, never invented values.

### Examples

```sql
LOAD livetennis;
CREATE SECRET (TYPE livetennis, API_KEY 'ltk_...');

-- What is on court right now, best-of-5 first?
SELECT tournament, p1_name, p2_name, sets, games, points, is_tiebreak
FROM live_tennis_matches()
ORDER BY format DESC, tournament;

-- Live ATP matches on clay
SELECT p1_name, p2_name, round, sets
FROM live_tennis_matches()
WHERE tour = 'atp' AND surface = 'clay';

-- Tomorrow's WTA fixtures that already have a start time
SELECT start_time, tournament, round, player1_name, player2_name
FROM live_tennis_fixtures('wta')
WHERE start_time IS NOT NULL
ORDER BY start_time;

-- Find a player
SELECT player_id, name, tour, country, ranking, ranking_points
FROM live_tennis_players('sinner');

-- Join: is anyone ranked top-100 on court right now?
SELECT m.tournament, m.p1_name, m.p2_name, m.p1_ranking, m.p2_ranking
FROM live_tennis_matches() m
WHERE LEAST(m.p1_ranking, m.p2_ranking) <= 100;
```

### Schemas

`live_tennis_matches()`: `match_id BIGINT`, `tournament VARCHAR`,
`tournament_id VARCHAR`, `tour VARCHAR`, `surface VARCHAR`, `indoor BOOLEAN`,
`format VARCHAR`, `round VARCHAR`, `round_code VARCHAR`, `status VARCHAR`,
`event_status VARCHAR`, `is_doubles BOOLEAN`, `scheduled_time TIMESTAMPTZ`,
`p1_id BIGINT`, `p1_name VARCHAR`, `p1_country VARCHAR`, `p1_ranking INTEGER`
(same four for `p2_`), `sets INTEGER[]`, `games INTEGER[][]` (per player, one
entry per set), `points VARCHAR[]` (tennis point strings: `"0"`, `"15"`,
`"40"`, `"AD"`; entries can be NULL), `server INTEGER`, `is_tiebreak BOOLEAN`,
`win_probability_p1 DOUBLE`, `danger DOUBLE`, `score_timestamp TIMESTAMPTZ`.

`live_tennis_fixtures([tour])`: `fixture_id BIGINT`, `event_date DATE`,
`start_time TIMESTAMPTZ` (NULL until the order of play assigns a time),
`tour VARCHAR`, `tournament VARCHAR`, `round VARCHAR`, `round_code VARCHAR`,
`surface VARCHAR`, `status VARCHAR`, `player1_id BIGINT`,
`player1_name VARCHAR`, `player2_id BIGINT`, `player2_name VARCHAR`.
Player ids are NULL when the participant is not yet resolved to the roster;
names are always present.

`live_tennis_players(search)`: `player_id BIGINT`, `name VARCHAR`,
`tour VARCHAR`, `country VARCHAR`, `ranking INTEGER`,
`ranking_points INTEGER`, `ranking_movement VARCHAR`, `hand VARCHAR`,
`backhand INTEGER`, `birthday DATE`, `is_doubles_team BOOLEAN`.

## Tiers and rate limits (honest facts)

Everything this extension exposes works on the **FREE** tier:
live/upcoming matches, current scores, player search and fixtures, at
**30 requests/minute and 100 requests/day** per key. Each function call is at
least one HTTP request (one per 200 result rows).

Higher tiers unlock more of the API (not currently surfaced by this
extension) and raise the limits:

- **BASIC** — historical results, point-by-point tapes, the 1968–2022
  archive, head-to-head. 60 req/min, 1,000/day.
- **PRO** — match events, market prices, monthly bulk history packages, the
  rank-ordered rankings listing. 300 req/min, 10,000/day.
- **ULTRA** — model analysis and the live `win_probability_p1`/`danger`
  fields on score objects (below ULTRA those columns are simply NULL),
  in-play statistics, rally/charting data, WebSocket feeds, webhooks.
  600 req/min, 500,000/day.

Details: <https://livetennisapi.com/pricing> ·
API reference: <https://docs.livetennisapi.com>

## Errors

The extension turns API failures into clear SQL errors instead of empty
results: a missing key tells you the three ways to configure one; `401` means
the key was rejected; `403` names the tier wall; `429` is the rate limit;
`400` carries the API's own message (e.g. an unrecognised `tour` value).

## Building from source

```sh
git clone --recurse-submodules https://github.com/livetennisapi/duckdb-livetennis.git
cd duckdb-livetennis
# vcpkg is required (for OpenSSL); see the extension template docs
export VCPKG_TOOLCHAIN_PATH=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
make
```

The build produces `./build/release/duckdb` (shell with the extension
pre-loaded) and
`./build/release/extension/livetennis/livetennis.duckdb_extension`.

### Tests

```sh
make test                                  # offline tests (no key needed)
LIVE_TENNIS_API_KEY=ltk_... make test      # also runs the live smoke tests
```

The live tests are gated behind `require-env LIVE_TENNIS_API_KEY` and are
skipped when the variable is not set.

## License

MIT. This extension is maintained by the Live Tennis API team.

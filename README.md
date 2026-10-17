# DwarfStar (ds4) model servers via `orchd`

Five checkpoints, one GPU, one HTTP front door. Stock `orchd` 0.3.4 + a small
C sidecar (`arbiter`) that raises whichever model a request names.

- **Target shape:** one grove per model + a controller grove for the arbiter.
- **Why:** orchd `fell` tears down a whole state dir and there is no
  per-service stop, so a grove has to *be* a service. See [Roadmap](#roadmap).

## Layout

```
ds4-runner/
├── arbiter.c  tinyjson.{c,h}  Makefile   # front door + switch engine
├── .orchrc                               # scope=user
├── arbiter.orch                          # grove "ctl"
└── services/
    ├── ds4server.orch                    # grove "ds4server"
    ├── ds41server.orch
    ├── ds4flash-q2.orch
    ├── ds4flash-q2-legacy.orch
    └── glm53server.orch
```

`services/` is the registry — the arbiter globs it and reads each spec via
`orch parse`. No hard-coded roster.

## Roster

| Service | Model | Size | Port | `ENV MODELS` (advertised ids) |
| --- | --- | ---: | ---: | --- |
| `ds4server` | V4 Flash (fixed 0731) | 91 GiB | 8888 | `deepseek-v4-flash`, `deepseek-v4-pro` |
| `ds41server` | V4.1 Flash Q2 | 341 GiB | 8887 | `deepseek-v4.1-flash` |
| `ds4flash-q2` | V4 Flash IQ2XXS | 81 GiB | 8890 | `deepseek-v4-flash-iq2` |
| `ds4flash-q2-legacy` | V4 Flash IQ2XXS (pre-0731) | 81 GiB | 8891 | `deepseek-v4-flash-iq2-legacy` |
| `glm53server` | GLM 5.3 Flash Q2 | 90 GiB | 8889 | `glm-5.2`, `glm-5.2-chat`, `glm-5.2-reasoner` |
| `arbiter` | (front door) | — | 8899 | — |

## Build

```sh
make     # cc -O2 -std=c11 -o arbiter arbiter.c tinyjson.c
```

`arbiter.orch` runs `~/ds4-runner/arbiter`, so build before first grow.

## Specs

`.orchrc`: `scope=user`.

`arbiter.orch` (own grove):

```dockerfile
SERVICE arbiter
RUN bash -c 'exec ~/ds4-runner/arbiter --dir ~/ds4-runner --orch ~/.local/bin/orch --orchd ~/.local/bin/orchd --listen 127.0.0.1:8899'
HEALTHCHECK http://127.0.0.1:8899/health
RESTART always
```

`services/ds4server.orch` (one per model; the rest match):

```dockerfile
SERVICE ds4server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/<file>.gguf --host 0.0.0.0 --port 8888 --cors --ctx 100000 --ssd-streaming --kv-disk-dir ~/.ds4/server-kv --kv-disk-space-mb 16384 --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
ENV MODELS=deepseek-v4-flash,deepseek-v4-pro
STDOUT /tmp/ds4-server.log
STDERR /tmp/ds4-server.log
HEALTHCHECK http://127.0.0.1:8888/v1/models
RESTART no
```

- `RUN bash -c 'cd … && exec …'` — launchd won't expand `~` in a workdir;
  `exec` keeps the PID.
- `nice -n 10` — no `NICE` directive; rides the command line.
- shared `DS4_LOCK_FILE` → single GPU holder.
- `RESTART no` — lock losers `exit(2)` and must stay down.
- `ENV MODELS` — names the front door advertises for this grove.

## Commands

```sh
ROOT=~/ds4-runner; ORCHD=~/.local/bin/orchd
ctl() { $ORCHD --project-dir $ROOT --state-dir ~/.orch/g/ctl \
              --orchfile $ROOT/arbiter.orch --namespace ctl --user "$@"; }
mdl() { $ORCHD --project-dir $ROOT --state-dir ~/.orch/g/$1 \
              --orchfile $ROOT/services/$1.orch --namespace $1 --user "${@:2}"; }

ctl grow                            # install + start the arbiter
mdl ds4server grow                  # start one model
mdl glm53server fell --keep-data    # stop exactly that model
```

Flags precede the subcommand (`--user` is not global in 0.3.4). Keep only the
`ctl` grove installed; grow/fall model groves on demand.

## arbiter

| Route | Purpose |
| --- | --- |
| `GET /health` | liveness |
| `GET /state` | `{"active","port","pid"}` |
| `GET /models` | each grove + `pid` + `healthy` |
| `POST /use/<name>` | switch; blocks until ready |
| `POST /stop` | fell every model grove |
| `GET /v1/models` | every advertised id |
| `POST /v1/chat/completions` | resolve → raise → relay |

Flags: `--dir --orch --orchd --state-root --listen --timeout --dump`.

**Switch:** `orchd survey --json` per grove → live PID = holder; `fell` the
others → `grow` target → poll `HEALTHCHECK` to 200. Fork-per-connection, so a
switch or a stream never blocks the accept loop.

**Front door:** `wme`/any OpenAI client points at `http://127.0.0.1:8899/v1`
once. `/v1/models` lists all ids; on `/v1/chat/completions` the arbiter resolves
`"model"` → grove, raises it, relays the response verbatim (streaming included).
No `/use` needed. `ENV MODELS` maps station names → groves.

## Mutual exclusion

`DS4_LOCK_FILE` = exclusive non-blocking `flock`. First process wins; later ones
print `another ds4 process is already running (pid N); refusing to start` and
`exit(2)`. All services share `/tmp/ds4-gpu.lock`; the file holds the holder PID.

> Never pair a shared lock with `RESTART always` — losers `exit(2)` → boot loop.

`DISABLED true` services aren't generated at all, so `grow <name>` can't start
them. Omit the block instead.

## Performance

- **Resident when it fits.** ≤90 GiB (Q2 81 GiB, GLM 90 GiB) → drop
  `--ssd-streaming`. 91 GiB / 341 GiB need it.
- **`--batched-session` and speculation are exclusive.** For low single-stream
  latency: drop batching, add `--dspark --mtp-model …DSpark-support-0731.gguf`
  (V4 Flash) or `--mtp` (GLM; V4.1 has none).
- **Leave the expert cache automatic** (~75% RAM); `--ssd-streaming-cache-experts`
  oversize displaces resident weights. `--ssd-streaming-preload-experts N` is a knob.
- **`--ctx` scales KV memory.** Drop `nice -n 10` for peak throughput.

## Roadmap

Today: 5 model groves + `ctl`, and `arbiter`. End state: **one grove**, arbiter
deleted. Three orchd gaps force the split; each closes one:

| Today's constraint | Missing capability | Effect |
| --- | --- | --- |
| no stop-by-name; `fell` = whole state dir | per-service stop, native to the **`orchdi`** platform (pidfile-tracked leaves) | models can share a grove |
| spec read once; edit needs `fell`+`grow` (launchd resurrects stale args) | **spec adoption** — reconcile the spec in place | one file = roster + selected model |
| nothing declares two services mutually exclusive | **`EXCLUSIVE <resource>`** (+`PREEMPT`) | orchd does the GPU swap itself |

Then `services/*.orch` merges into one file and `arbiter.c` goes away:

```dockerfile
SERVICE glm53server
RUN … --port 8889 …
EXCLUSIVE gpu
PREEMPT ds4server
```

switch = `orchd --platform orchdi grow glm53server`.

Not in 0.3.4: no `EXCLUSIVE`/`PREEMPT` directive, no `stop` subcommand,
`clean()` removes the whole state dir. The arbiter is a hand-rolled version of
this loop: grove = service, `fell` = stop, `flock` = exclusive slot.

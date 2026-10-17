# DwarfStar (ds4) model servers via `orchd`

Five DeepSeek/GLM checkpoints, one GPU, switched over HTTP. Built on stock
`orchd` 0.3.4 — no forks — plus a tiny C sidecar (`arbiter`) that flips which
model is serving without anyone killing a process by hand.

The unusual part is the shape: **one grove per model**, not one Orchfile for all.
[Why](#why-one-grove-per-model) explains the constraint; [From five groves to
one](#from-five-groves-to-one) explains exactly what would collapse it back.

---

## Why one grove per model

A grove is the unit orchd can tear down. In 0.3.4 that unit is too coarse to
hold several models that must run **one at a time**:

1. **There is no per-service stop.** `orchd` has `grow` (start), `fell`
   (tear down), `survey`, `logs` — no `stop <service>`.
2. **`fell` is scoped to the *state dir*, not the service.** Its `clean()`
   walks `<state-dir>/units`, boots out and uninstalls everything listed, then
   deletes the directory. One shared state dir ⇒ `fell` kills the whole world.
3. **Composition can only add, never remove.** Overlays merge services
   (scalars last-wins, `ENV` by key, `REQUIRES`/`AFTER` appended); there is no
   directive to *drop* a service. So "these five models, run one" isn't
   expressible as one file that you edit down.
4. **A rewrite isn't adopted.** launchd keeps the old in-memory job when a
   plist is rewritten; changing args needs `bootout` + re-`bootstrap`.

So the model that **is** reliably expressible is: *a grove = a service*. Each
model gets its own spec file, its own `--state-dir`, and its own
`--namespace` (the namespace prefixes the label, so the pidfile/unit names
can't collide). `fell` on one grove stops exactly one model.

The arbiter gets a grove of its own for the same reason: if it shared a grove
with a model, `fell`ing that model to switch away would kill the controller.

---

## Layout

```
ds4-runner/
├── arbiter.c  tinyjson.{c,h}  Makefile   # the switch front door (C, no deps)
├── .orchrc                               # scope=user
├── arbiter.orch                          # grove "ctl"     — the arbiter itself
└── services/
    ├── ds4server.orch                    # grove "ds4server"
    ├── ds41server.orch                   # grove "ds41server"
    ├── ds4flash-q2.orch                  # grove "ds4flash-q2"
    ├── ds4flash-q2-legacy.orch           # grove "ds4flash-q2-legacy"
    └── glm53server.orch                  # grove "glm53server"
```

`services/` **is** the registry: the arbiter has no hard-coded roster. It globs
`services/*.orch`, runs `orch parse` on each, and reads back the name, port and
health URL. Drop a file in and it appears in `/models`.

## Roster

| Service | Model | Size | Port |
| --- | --- | ---: | ---: |
| `ds4server` | DeepSeek V4 Flash (fixed build) | 91 GiB | 8888 |
| `ds41server` | DeepSeek V4.1 Flash Q2 | 341 GiB | 8887 |
| `ds4flash-q2` | DeepSeek V4 Flash IQ2XXS | 81 GiB | 8890 |
| `ds4flash-q2-legacy` | DeepSeek V4 Flash IQ2XXS (pre-0731) | 81 GiB | 8891 |
| `glm53server` | GLM 5.3 Flash Q2 | 90 GiB | 8889 |
| `arbiter` | (not a model) switch front door | — | 8899 |

---

## Build

```sh
make            # cc -O2 -std=c11 -o arbiter arbiter.c tinyjson.c
```

The `RUN` line points at `~/ds4-runner/arbiter`, so build before first grow.

## Specs

`arbiter.orch` (its own grove):

```dockerfile
ORCH_VERSION 1.0.0-rc

SERVICE arbiter
RUN bash -c 'exec ~/ds4-runner/arbiter --dir ~/ds4-runner --orch ~/.local/bin/orch --orchd ~/.local/bin/orchd --listen 127.0.0.1:8899'
STDOUT /tmp/ds4-arbiter.log
STDERR /tmp/ds4-arbiter.log
HEALTHCHECK http://127.0.0.1:8899/health
RESTART always
```

Every model file is the same shape (`services/ds4server.orch`):

```dockerfile
ORCH_VERSION 1.0.0-rc

SERVICE ds4server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4-Flash-…-fixed-0731.gguf --host 0.0.0.0 --port 8888 --cors --ctx 100000 --ssd-streaming --kv-disk-dir ~/.ds4/server-kv --kv-disk-space-mb 16384 --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
STDOUT /tmp/ds4-server.log
STDERR /tmp/ds4-server.log
HEALTHCHECK http://127.0.0.1:8888/v1/models
RESTART no
```

Notes:

- **`RUN bash -c 'cd … && exec …'`** — launchd won't expand `~` in a working
  dir, and orchd resolves a relative `WORKDIR` against the project dir. The
  inner shell `cd`s; `exec` keeps the PID.
- **`nice -n 10`** — the grammar has no `NICE`, so it rides the command line.
- **One shared `DS4_LOCK_FILE`** across every model → the GPU has a single
  holder (see [Mutual exclusion](#mutual-exclusion)).
- **`RESTART no`** on models — lock losers must stay down, not boot-loop. Only
  the arbiter is `RESTART always`.
- **`STDOUT`/`STDERR`** keep the familiar `/tmp/*.log` paths.

---

## Commands

Each grove is addressed by three flags — project dir, state dir, namespace —
plus its spec file. Wrapped for brevity:

```sh
ROOT=~/ds4-runner; ORCHD=~/.local/bin/orchd

# controller grove
ctl() { $ORCHD --project-dir $ROOT --state-dir ~/.orch/g/ctl \
              --orchfile $ROOT/arbiter.orch --namespace ctl --user "$@"; }

# a model grove (pass the model name as the 2nd arg)
mdl() { $ORCHD --project-dir $ROOT --state-dir ~/.orch/g/$1 \
              --orchfile $ROOT/services/$1.orch --namespace $1 --user "${@:2}"; }

ctl grow                     # install + start the arbiter
mdl ds4server grow           # install + start one model
mdl glm53server fell --keep-data   # stop exactly that model
```

Survey/tail a grove the same way (`mdl ds4server survey`, `mdl ds4server logs ds4server`).
Flags must precede the subcommand; `--user` is not a global arg in 0.3.4.

**Leave only the controller installed.** Model groves are grown on demand by
the arbiter. Any model plist left installed is resurrected by launchd at login;
the models then race the shared `flock`, one wins, the rest `exit(2)` and stay
down (`RESTART no`). Harmless, but the clean state is: `ctl` installed, model
groves grown/fallen as needed.

---

## Switching models — `arbiter`

Apps never kill processes. They ask the arbiter on `127.0.0.1:8899`.

| Endpoint | Purpose |
| --- | --- |
| `GET /health` | arbiter liveness |
| `GET /state` | `{"active": <name>\|null, "port": <int>\|null, "pid": <int>\|null}` |
| `GET /models` | each model + its orchd `pid` + `healthy`, plus the active one |
| `POST /use/<name>` (or `/use` `{"model":"…"}`) | switch; blocks until ready |
| `POST /stop` | stop every model grove |

```sh
curl -s http://127.0.0.1:8899/state
curl -s -X POST http://127.0.0.1:8899/use/glm53server
curl -s -X POST 'http://127.0.0.1:8899/use/ds41server?timeout=1800'
```

A switch (`switch_to` in `arbiter.c`):

1. **Who is running is asked of orchd, not launchd.** For each grove the
   arbiter runs `orchd … survey --json` and reads the reported `pid` — a live
   PID is the holder. (Trust the PID, not `state`: launchd reports
   `state = active` while orchd 0.3.4 only maps the literal `running`, so
   `state` reads `inactive` even for a live job.)
2. If the target is already the holder **and** answers its health URL, return
   its port — no-op.
3. Otherwise `fell --keep-data` every other grove orchd reports with a live
   PID (that model is the holder; its `DS4_LOCK_FILE` is released as it exits).
4. `grow` the target grove, then poll its `HEALTHCHECK` until HTTP 200 or
   `--timeout` (default 900 s) — orchd says *started*, health says *serving*.
   Both are needed: a PID exists before the weights are loaded.
5. Return `{"ok":true,"model":…,"port":…}`.

The call **blocks** until the model is serving, so the caller can immediately
use the port. The accept loop is single-threaded, so switches serialize
themselves.

Flags: `--dir` (spec root, default `.`), `--orch`/`--orchd` (binary paths),
`--state-root` (default `~/.orch/g`), `--listen host:port`, `--timeout`, and
`--dump` (print the roster it discovered and exit).

---

## From five groves to one

The six-grove layout is not the target shape — it's the target shape expressed
with the primitives orchd 0.3.4 actually ships. The target is **one grove**.
Three capabilities, each removing one constraint from
[above](#why-one-grove-per-model), get us there:

| Constraint today | Capability that removes it | Once it exists |
| --- | --- | --- |
| no stop-by-name; `fell` = whole state dir | **per-service stop**, natural on the **`orchdi`** platform (orchd supervises directly, each leaf pidfile-tracked in `<state>/run/<label>.pid`) | stop one model without touching the rest → they can share a grove |
| a grove's spec is read once; changing it means `fell`+`grow` (and launchd resurrects stale args) | **spec adoption** — a running grove re-reads and reconciles its single spec in place | edit one file to change the roster *or* the selected model; no teardown |
| nothing tells orchd two services can't run at once | **`EXCLUSIVE <resource>`** (+ a `PREEMPT` displacement policy) — a named slot held by at most one service | orchd itself guarantees one GPU holder and orders the swap |

With all three, the whole roster and the switch live in **one file**:

```dockerfile
ORCH_VERSION 1.0.0-rc

SERVICE ds4server
RUN bash -c 'cd ~/projects/ds4 && exec ~/projects/ds4/ds4-server … --port 8888 …'
HEALTHCHECK http://127.0.0.1:8888/v1/models
EXCLUSIVE gpu

SERVICE ds41server
RUN … --port 8887 …
EXCLUSIVE gpu
PREEMPT ds4server

SERVICE glm53server
RUN … --port 8889 …
EXCLUSIVE gpu
PREEMPT ds4server
```

and the switch becomes a single verb:

```sh
orchd --platform orchdi grow glm53server
# orchd sees glm53server wants the exclusive "gpu" slot, stops whatever holds
# it (and anything named in PREEMPT), then starts glm53server. No arbiter.
```

Under `--platform orchdi` there is **no launchd at all**: orchd writes a
`SuperviseSpec` per service and spawns `orchd supervise` itself, detached and
tracked by pidfile — the OS init that forces the coarse teardown granularity
is simply gone (see `platform/orchdi.rs` and `platform/orchdi/lifecycle.rs` in
orchd). Per-service stop is native there; the only reason it isn't reachable
from the CLI is that `engine.rs` calls `lifecycle::stop(&[], config)` with an
empty list. It is *one line of wiring* away.

The arbiter, then, is a hand-rolled version of exactly this reconcile loop:
**a grove is a service, `fell` is stop, the arbiter is the reconciler, and the
exclusive slot is ds4's own `flock`.** When per-service stop, spec adoption,
and `EXCLUSIVE` land, `services/*.orch` merges back into this single file and
`arbiter.c` is deleted.

> Verified against orchd 0.3.4 and the Orch grammar: there is no `EXCLUSIVE` /
> `PREEMPT` directive, no `stop` subcommand, and `clean()` removes the whole
> state dir. The snippet above is the design, not something that runs today.

---

## Mutual exclusion

DS4 guards instances itself: with `DS4_LOCK_FILE` set it takes an exclusive,
non-blocking `flock` at startup — first process wins; any later one prints
`another ds4 process is already running (pid N); refusing to start` and
`exit(2)`. Default `/tmp/ds4.lock`; released on exit.

Every service here shares `/tmp/ds4-gpu.lock`, making the GPU single-holder.

> ⚠️ Never pair a shared lock with `RESTART always` — losers `exit(2)`, which
> counts as failure, and launchd relaunches them forever.

`DISABLED true` is **not** a "don't run this" switch in this build: such a
service isn't generated at all, so `grow <name>` can't start it. Omit the
block, or rely on the shared lock + `RESTART no`.

---

## Directives

| Directive | Use |
| --- | --- |
| `REQUIRES a b` | hard dep — wait until ready |
| `AFTER a` | ordering dep |
| `HEALTHCHECK http://…` / `cmd` | this service's readiness; dependents' poll target |
| `ONESHOT true` | run-to-completion |
| `DISABLED true` | declared, not installed (see above) |
| `USER`, `STOP`, `RELOAD` | host lifecycle |
| `STDOUT`/`STDERR`, `ENV`, `ENV_FILE` | plumbing |
| `RESTART`, `RESTART_DELAY` | crash behaviour |

`REQUIRES`/`AFTER`/`STOP`/`post_stop` make orchd wrap the service in `orchd
supervise`; the launchd job is then the supervisor, not the model.

---

## Performance

- **Resident beats streaming when it fits.** ≤90 GiB models (V4 Flash Q2 81 GiB,
  GLM 5.3 90 GiB) run resident — drop `--ssd-streaming`. 91 GiB and 341 GiB need
  it. Resident means one model at a time (guaranteed above).
- **`--batched-session` and speculation are mutually exclusive.** This file
  batches (many clients). For lowest single-stream latency, drop batching and add
  `--dspark --mtp-model …DSpark-support-0731.gguf` (V4 Flash) or `--mtp` (GLM 5.3;
  V4.1 has no MTP).
- **Leave the expert cache automatic.** The auto budget is ~75% of RAM; an
  oversized `--ssd-streaming-cache-experts` displaces resident weights.
  `--ssd-streaming-preload-experts N` is a measurement knob.
- **Right-size `--ctx`** — it scales KV/buffer memory.
- **Drop `nice -n 10`** for peak throughput (it's there to keep the Mac responsive).

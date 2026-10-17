# DwarfStar (ds4) model servers via `orchd`

One `Orchfile` replaces the hand-written `com.dwarfstar.*.plist` agents, and a
small `arbiter` sidecar lets an app switch models over HTTP without killing
processes. Stock `orchd` 0.3.1, no forks.

---

## Roster

| Service | Model | Size |
| --- | --- | ---: |
| `ds4server` | DeepSeek V4 Flash (fixed build) | 91 GiB |
| `ds4flash-q2` | DeepSeek V4 Flash Q2 | 81 GiB |
| `ds4flash-q2-legacy` | DeepSeek V4 Flash Q2 (pre‑0731) | 81 GiB |
| `ds41server` | DeepSeek V4.1 Flash Q2 | 341 GiB |
| `glm53server` | GLM 5.3 Flash Q2 | 90 GiB |
| `arbiter` | (not a model) switch front door | — |

Add a `SERVICE` block for any other checkpoint (`download_model.sh` is the menu).

---

## Orchfile

Beside a one-line `.orchrc`:

```
namespace=com.dwarfstar
scope=user
```

```dockerfile
ORCH_VERSION 1.0.0-rc

SERVICE arbiter
RUN bash -c 'exec nice -n 5 ~/ds4-runner/arbiter --listen 127.0.0.1:8899'
STDOUT /tmp/ds4-arbiter.log
STDERR /tmp/ds4-arbiter.log
HEALTHCHECK http://127.0.0.1:8899/health
RESTART always

SERVICE ds4server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4-Flash-Layers37-42Q4KExperts-OtherExpertLayersIQ2XXSGateUp-Q2KDown-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-fixed-0731.gguf --host 0.0.0.0 --port 8888 --cors --ctx 100000 --ssd-streaming --kv-disk-dir ~/.ds4/server-kv --kv-disk-space-mb 16384 --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
STDOUT /tmp/ds4-server.log
STDERR /tmp/ds4-server.log
HEALTHCHECK http://127.0.0.1:8888/v1/models
AFTER arbiter
RESTART no

SERVICE ds4flash-q2
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf --host 0.0.0.0 --port 8890 --cors --ctx 32768 --ssd-streaming --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
STDOUT /tmp/ds4-flash-q2.log
STDERR /tmp/ds4-flash-q2.log
HEALTHCHECK http://127.0.0.1:8890/v1/models
AFTER arbiter
RESTART no

SERVICE ds4flash-q2-legacy
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf --host 0.0.0.0 --port 8891 --cors --ctx 32768 --ssd-streaming --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
STDOUT /tmp/ds4-flash-q2-legacy.log
STDERR /tmp/ds4-flash-q2-legacy.log
HEALTHCHECK http://127.0.0.1:8891/v1/models
AFTER arbiter
RESTART no

SERVICE ds41server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4.1-Flash-Q2.gguf --host 0.0.0.0 --port 8887 --cors --ctx 65536 --ssd-streaming --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
STDOUT /tmp/ds41-server.log
STDERR /tmp/ds41-server.log
HEALTHCHECK http://127.0.0.1:8887/v1/models
AFTER arbiter
RESTART no

SERVICE glm53server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/GLM-5.3-Flash-Q2.gguf --host 0.0.0.0 --port 8889 --cors --backend metal --ctx 32768 --ssd-streaming --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
STDOUT /tmp/ds4-glm53.log
STDERR /tmp/ds4-glm53.log
HEALTHCHECK http://127.0.0.1:8889/v1/models
AFTER arbiter
RESTART no
```

### Notes

- **`RUN bash -c 'cd … && exec …'`** — launchd won't expand `~` in
  `WorkingDirectory`, and `orchd` resolves a relative `WORKDIR` against the
  project dir. The inner shell `cd`s to the real dir; `exec` keeps the PID.
- **`nice -n 10`** — the grammar has no `NICE`, so it rides the command line.
- **One shared `DS4_LOCK_FILE`** across every model → the GPU has a single holder
  (see *Mutual exclusion*).
- **`RESTART no`** on models — lock losers must stay down, not boot-loop. The
  arbiter is the only `RESTART always`.
- **`AFTER arbiter` + `HEALTHCHECK`** — a dependency makes `orchd` wrap the
  service in `orchd supervise`, which polls the dep's `HEALTHCHECK` before
  starting. Works under launchd (the wrapper *is* the job).
- **`STDOUT`/`STDERR`** keep the `/tmp/*.log` paths instead of orchd's defaults.

---

## Commands

Run from this directory:

```sh
orchd --user grow            # write plists, install, start all
orchd --user grow ds4server  # start one
orchd --user survey [--json] # state + PID
orchd --user logs ds4server  # tail
orchd --user fell [--keep-data]  # stop and remove units
```

`grow` is three composable stages if you need to hack the middle:

```sh
orchd sow    # spec    → cuttings
orchd plant  # cuttings → beds
orchd tend   # beds     → running
```

### Reload gotcha

`grow` on an already-loaded job rewrites the plist but launchd keeps the old
in-memory job (old args). Force a reload:

```sh
launchctl bootout gui/$(id -u)/com.dwarfstar.ds4server && orchd --user grow ds4server
# or everything:
orchd --user fell --keep-data && orchd --user grow
```

---

## Switching models — `arbiter`

Apps never kill processes; they ask the arbiter (`127.0.0.1:8899`) for a model.

| Endpoint | Purpose |
| --- | --- |
| `GET /health` | arbiter liveness |
| `GET /state` | `{"active": <name>\|null, "port": <int>\|null}` |
| `GET /models` | each model + `running`/`healthy` |
| `POST /use/<name>` (or `/use` `{"model":...}`) | switch; blocks until ready |
| `POST /stop` | boot out all models |

```sh
curl -s http://127.0.0.1:8899/state
curl -s -X POST http://127.0.0.1:8899/use/glm53server
curl -s -X POST 'http://127.0.0.1:8899/use/ds41server?timeout=1800'
```

The arbiter **reads the spec** at startup — `SERVICE` names, each `RUN`'s
`--port`, each `HEALTHCHECK`, and `namespace` from `.orchrc`. No roster of its
own; change the Orchfile and it follows.

A switch: if the target is already serving, return; else boot out every running
model (`bootout` TERMs the `supervise` wrapper, which `killpg`s the model and
releases the flock), `bootstrap` the target, poll its `HEALTHCHECK` until 200 or
timeout (default 900 s), then return the port. The call blocks so the model is up
when it answers; switches are serialised.

At login every model starts, they race the shared lock, one wins, the rest
`exit(2)` and stay down. Call `/use` to pick.

### Replacing it with `orchdi`

The arbiter is a sidecar, not framework work — it needs no `orchd` change. It
stands in for what `orchdi` will do natively once the spec gains an exclusive
slot:

```dockerfile
SERVICE glm53server
EXCLUSIVE gpu
PREEMPT   ds4server
```

Then apps call `orchd --user grow glm53server` and this script is deleted.

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
service isn't generated at all, so `grow <name>` can't start it. Omit the block,
or rely on the shared lock + `RESTART no`.

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

# Running DwarfStar (ds4) model servers with `orchd`

One `Orchfile` replaces a pile of hand-written `com.dwarfstar.*.plist`
LaunchAgents. `orchd` turns it into native launchd jobs — start order, restart,
logs, env, clean stop — and a tiny **arbiter** sidecar lets an application
switch which model is serving **without killing a process by hand**.

DwarfStar ships a whole zoo of models, not just DeepSeek and GLM. Any of them can
be a service here.

---

## The model roster

`download_model.sh` is the menu; this runner currently covers the checkpoints
on disk:

| Service | Model | File | Size |
| --- | --- | --- | ---: |
| `ds4server` | DeepSeek V4 Flash (fixed build) | `…Layers37-42Q4KExperts…fixed-0731.gguf` | 91 GiB |
| `ds4flash-q2` | DeepSeek V4 Flash Q2 | `…IQ2XXS-w2Q2K…imatrix-0731.gguf` | 81 GiB |
| `ds4flash-q2-legacy` | DeepSeek V4 Flash Q2 (pre‑0731) | `…IQ2XXS-w2Q2K…imatrix.gguf` | 81 GiB |
| `ds41server` | DeepSeek V4.1 Flash Q2 | `DeepSeek-V4.1-Flash-Q2.gguf` | 341 GiB |
| `glm53server` | GLM 5.3 Flash Q2 | `GLM-5.3-Flash-Q2.gguf` | 90 GiB |

plus the `arbiter` (not a model). The same pattern extends to the rest of the
catalogue — **DeepSeek PRO** (`pro-q2-imatrix`, `pro-q4-split`), **Qwen3.8 Flash
Next** (`qwen38-q2`, `qwen38-q4k`), the GLM community quants (`glm-antirez-*`,
`glm-unsloth-q4`), and the vision encoders (`*-vision`) — just add another
`SERVICE` block.

---

## The Orchfile

Lives in this directory beside a one-line `.orchrc`:

```
namespace=com.dwarfstar
scope=user
```

`namespace` prefixes the launchd label (`com.dwarfstar.ds4server`), so `orchd`
owns the same agent names the old plists used.

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

### Why it looks like that

- **`RUN bash -c 'cd … && exec …'`** — launchd does **not** expand `~` in
  `WorkingDirectory`, and `orchd` resolves a relative `WORKDIR` against the
  project dir (giving `/Users/you/ds4-runner/~/…`). The inner shell `cd`s to the
  real project dir and `exec` keeps the PID, matching the old `WorkingDirectory`.
- **`nice -n 10`** — the plists had `Nice 10`; the Orchfile grammar has no `NICE`
  directive, so it rides the command line.
- **`ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock`** — **every model shares one lock**, so
  the GPU is mutually exclusive at the kernel level (see below).
- **`RESTART no`** on the models — the losers of the lock race must *stay down*
  instead of boot-looping. `RESTART always` → `KeepAlive`; `RESTART no` → no
  `KeepAlive`. (The arbiter is the one thing that *is* `RESTART always`.)
- **`AFTER arbiter` + `HEALTHCHECK`** — declaring a dependency makes `orchd` wrap
  the service in `orchd supervise`, which polls the dependency's `HEALTHCHECK`
  before starting the model. This is the "intricate dependency system", and it
  works under **launchd** too (the wrapper *is* the launchd job).
- **`STDOUT`/`STDERR`** keep the `/tmp/*.log` destinations instead of orchd's
  default `~/Library/Logs/<label>.{out,err}.log`.
- **Ports** are unique per model so the roster is unambiguous:
  `8888 8887 8889 8890 8891` (+ arbiter `8899`).

---

## Day to day

Run from this directory (that's how `.orchrc` and `./Orchfile` are found):

```sh
orchd --user grow            # sow → plant → tend: write plists, install, start
orchd --user grow ds4server  # start just one
orchd --user survey          # table of services: state, PID
orchd --user survey --json
orchd --user logs ds4server  # tail one service (--follow)
orchd --user fell            # stop everything and remove the units
orchd --user fell --keep-data
```

`grow` is three composable JSON stages if you want to hack in the middle:

```sh
orchd sow    # spec    → cuttings  (pair each service with its exec set)
orchd plant  # cuttings → beds     (render native launchd/systemd files)
orchd tend   # beds     → running  (write, install, start)
```

### Reload gotcha

`orchd grow` on an **already-loaded** job rewrites the plist but launchd keeps the
old in-memory job, so the process keeps its old args. Force a real reload:

```sh
orchd --user fell --keep-data && orchd --user grow
# or per service:
launchctl bootout gui/$(id -u)/com.dwarfstar.ds4server && orchd --user grow
```

---

## Switching models — the arbiter

The end-harness should never `kill` anything. It talks to the **arbiter**
(`arbiter`, a small always-on Python sidecar on `127.0.0.1:8899`) and asks for the
model it wants. The arbiter stops the current holder, starts the target, and only
answers once the target is serving.

```
harness ──HTTP──> arbiter ──┬─ launchctl bootout  gui/<uid> <current plist>
                            └─ launchctl bootstrap gui/<uid> <target plist>
```

| Endpoint | Purpose |
| --- | --- |
| `GET /health` | liveness of the arbiter itself |
| `GET /state` | `{"active": "<name>"\|null, "port": <int>\|null}` |
| `GET /models` | every model with `running` / `healthy` flags |
| `POST /use/<name>` or `POST /use` `{"model":"<name>"}` | switch; blocks until ready |
| `POST /stop` | boot out every model (arbiter stays up) |

```sh
curl -s http://127.0.0.1:8899/state
curl -s -X POST http://127.0.0.1:8899/use/glm53server     # switch, waits
# or, with a longer load budget for the big ones:
curl -s -X POST 'http://127.0.0.1:8899/use/ds41server?timeout=1800'
```

**The arbiter derives itself from this Orchfile.** At startup it reads `./Orchfile`
(`SERVICE` names, the `--port` in each `RUN`, each `HEALTHCHECK`) and the
`namespace` from `./.orchrc`. It holds **no roster of its own** — change the
Orchfile and the arbiter follows. It refuses to start if no service declares a
port, and warns if the services don't share one `DS4_LOCK_FILE`.

What a switch does, in order:

1. if the target is already the healthy holder → return immediately;
2. otherwise boot out each currently-running model (launchd TERMs the `supervise`
   wrapper, which `killpg`s the model, so the `flock` is released);
3. `launchctl bootstrap` the target (or `orchd --user grow <name>` if its plist was
   never installed);
4. poll the target's `HEALTHCHECK` until 200 or `--timeout` (default 900 s);
5. return `{"ok":true,"model":…,"port":…}`.

Because the load step is slow, the switch call **blocks** — that's the point:
when it returns, the model is up. `POST /use` is idempotent and serialised, so
concurrent callers can't race the GPU.

> At login every model starts at once; they contend on the shared lock, one wins,
> the rest `exit(2)` and (being `RESTART no`) stay down. So exactly one model
> serves after login — call `/use` to pick which.

### Letting `orchdi` replace this

The arbiter is deliberately a *sidecar*, not framework work — it needs **no
changes to `orchd`**. It's a stand-in for what `orchdi` (orchd's own middle-man,
the `orchd supervise` process) will do natively once the **Orch spec** grows an
exclusive/singleton slot. When the spec can express something like:

```dockerfile
SERVICE glm53server
EXCLUSIVE gpu          # one holder per named resource
PREEMPT   ds4server    # stop the current holder, then take the slot
```

…the harness will just call `orchd --user grow glm53server` and this script can be
deleted. Until then, everything lives here in `dwarfstar-runner`, on stock
`orchd`.

---

## Mutual exclusion (one GPU, many models)

DwarfStar has its own instance guard. With `DS4_LOCK_FILE` set, the server takes
an **exclusive, non-blocking `flock`** at startup:

- first process: locks the file and writes its PID;
- any later process: `LOCK_EX | LOCK_NB` returns `EWOULDBLOCK`, prints
  `another ds4 process is already running (pid N); refusing to start`, and
  `exit(2)`.

Unset, it defaults to `/tmp/ds4.lock`. The lock is **per file path** and released
when the process exits. Every service in this file points at the *same* path
(`/tmp/ds4-gpu.lock`), which is what makes the GPU a single-holder resource.

> ⚠️ **Never combine a shared lock with `RESTART always`.** The losers `exit(2)`,
> which counts as failure, so launchd relaunches them forever — a boot loop of
> "refusing to start". That's why the models use `RESTART no`; the arbiter drives
> which one runs.

**`DISABLED`:** in this orchd build a `DISABLED true` service is not generated or
installed at all, so `orchd grow <name>` **cannot** start it. Don't use it as a
"don't run this one" switch — either omit the `SERVICE`, or rely on the shared
lock + `RESTART no` (what this file does).

---

## More than start/stop

| Directive | Use for |
| --- | --- |
| `REQUIRES a b` | hard dep — don't start until `a`,`b` are ready |
| `AFTER a` | soft ordering — start after `a` is up |
| `HEALTHCHECK http://…` / `cmd` | readiness of *this* service, and the poll target for its dependents |
| `ONESHOT true` | run-to-completion (e.g. a conversion/prefetch step) |
| `DISABLED true` | declared, not installed (see note above) |
| `USER`, `STOP`, `RELOAD` | host-mode lifecycle |
| `STDOUT`/`STDERR`, `ENV`, `ENV_FILE` | plumbing |
| `RESTART`, `RESTART_DELAY` | crash behaviour |

Declaring `REQUIRES`/`AFTER` (or `STOP`/`post_stop`) makes `orchd` wrap the
service in `orchd supervise`, which enforces the gate by polling the dependency's
`HEALTHCHECK`. The generated launchd job is then the *supervisor*, not the model
directly — `orchd survey` still shows it as one service.

---

## Performance knobs that matter

- **Resident beats streaming when it fits.** On a 128 GB Mac the ≤90 GiB models
  (V4 Flash Q2 at 81 GiB, GLM 5.3 Q2 at 90 GiB) can run *resident* — drop
  `--ssd-streaming` for the fastest decode. 91 GiB and 341 GiB still need it.
  Resident assumes you run **one model at a time** (which the shared lock + arbiter
  guarantee).
- **`--batched-session N` vs speculation are mutually exclusive.** While batching
  is active the engine does *not* use MTP/DSpark. Here we chose batching (many
  clients). For lowest single-stream latency, drop `--batched-session` and add:
  - DeepSeek V4 Flash: `--dspark --mtp-model ~/projects/ds4/gguf/DeepSeek-V4-Flash-DSpark-support-0731.gguf`
  - GLM 5.3: `--mtp` (built-in draft block; V4.1 has no MTP)
- **Leave the expert cache automatic.** Don't pass
  `--ssd-streaming-cache-experts 96GB` unless measured; the auto budget is already
  ~75% of RAM, and an oversized value can displace the always-resident
  non-routed weights and slow decode. (`--ssd-streaming-preload-experts N` is a
  measurement knob.)
- **Right-size `--ctx`.** It scales KV/buffer memory. `100K` is only worth it if
  you actually use 100K; the streaming examples here use 32768.
- **`nice -n 10`** keeps the Mac responsive at some cost to model latency; drop it
  for peak throughput.

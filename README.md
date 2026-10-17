# Running DwarfStar (ds4) model servers with `orchd`

A single `Orchfile` replaces a pile of hand-written `com.dwarfstar.*.plist`
LaunchAgents. `orchd` converts the file into native launchd jobs — start order,
restart, logs, env, and a clean stop — so you can fling any DwarfStar model up
with one command.

DwarfStar ships a whole zoo of models, not just DeepSeek and GLM. Any of them can
be a service here.

---

## The model roster

`download_model.sh` is the menu:

| Family | Targets | Notes |
| --- | --- | --- |
| **DeepSeek V4 Flash** | `ds4f-q2` (~81 GiB), `ds4f-q2-q4`, `ds4f-q4`, `ds4f-mxfp4`, `ds4f-dspark`, `ds4f-vision-*` | 0731 build; Q2 is the 96/128 GB starting point |
| **DeepSeek V4.1 Flash** | `ds41f-q2` (341 GiB), `ds41f-q4` (483 GiB), `ds41f-vision` | own GGUF/graph; 189 GiB Engram tables read from disk |
| **DeepSeek V4 PRO** | `pro-q2-imatrix`, `pro-q4-split` | 512 GB resident target, or SSD streaming |
| **GLM 5.3 Flash** | `glm53-q2` (~197 GiB), `glm53-q4` (178 GiB), `glm53-fp8`, `glm53-full-q2`, `glm53-vision` | Metal streaming supported |
| **GLM (alt quants)** | `glm-unsloth-q4`, `glm-antirez-q2`, `glm-antirez-iq2xxs`, `glm-antirez-q4` | community quants |
| **Qwen3.8 Flash Next** | `qwen38-q2` (alias `qwen38-iq2`), `qwen38-q4k`, `qwen38-vision` | native session batching |

Every one is just another `SERVICE` block.

---

## The Orchfile

Lives in this directory alongside a one-line `.orchrc`:

```
namespace=com.dwarfstar
scope=user
```

`namespace` prefixes the launchd label (`com.dwarfstar.ds4server`), so `orchd`
owns the exact same agent names the old plists used.

```dockerfile
ORCH_VERSION 1.0.0-rc

# DS4 inference servers, migrated 1:1 from ~/Library/LaunchAgents/com.dwarfstar.*.plist
# Tuned: expert cache left automatic (dropped oversized 96GB budget + explicit
# preload-experts), native session batching enabled. Grow: orchd --user grow

SERVICE ds4server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4-Flash-Layers37-42Q4KExperts-OtherExpertLayersIQ2XXSGateUp-Q2KDown-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-fixed-0731.gguf --host 0.0.0.0 --port 8888 --cors --ctx 100000 --ssd-streaming --kv-disk-dir ~/.ds4/server-kv --kv-disk-space-mb 16384 --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-flash.lock
STDOUT /tmp/ds4-server.log
STDERR /tmp/ds4-server.log
RESTART always

SERVICE ds41server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4.1-Flash-Q2.gguf --host 0.0.0.0 --port 8887 --cors --ctx 65536 --ssd-streaming --batched-session 4'
ENV DS4_LOCK_FILE=/tmp/ds41-flash.lock
STDOUT /tmp/ds41-server.log
STDERR /tmp/ds41-server.log
RESTART always
```

### Why it looks like that

- **`RUN bash -c 'cd … && exec …'`** — launchd does **not** expand `~` in
  `WorkingDirectory`, and `orchd` resolves a relative `WORKDIR` against the
  project dir (giving `/Users/you/ds4-runner/~/…`). The inner shell `cd`s to the
  real project dir, and `exec` keeps the PID, so it's equivalent to the old
  `WorkingDirectory` key.
- **`nice -n 10`** — the plists had `Nice 10`; the Orchfile grammar has no
  `NICE` directive, so it rides the command line.
- **`RESTART always`** → `KeepAlive` (crash → relaunch). `RESTART on-failure`
  → `KeepAlive` with `SuccessfulExit=false`.
- **`STDOUT`/`STDERR`** keep the old `/tmp/*.log` destinations instead of
  orchd's default `~/Library/Logs/<label>.{out,err}.log`.
- **`ENV DS4_LOCK_FILE=…`** — the per-model GPU mutex (see below).

---

## Day to day

Run these **from this directory** (that's how `.orchrc` and `./Orchfile` are found):

```sh
orchd --user grow          # sow → plant → tend: write plists, install, start
orchd --user survey        # table of services: state, PID
orchd --user survey --json # machine-readable
orchd --user logs ds4server# tail one service (--follow)
orchd --user fell          # stop everything and remove the units
orchd --user fell --keep-data
```

`orchd grow` is really three composable JSON stages if you want to hack in the middle:

```sh
orchd sow    # spec   → cuttings   (pair each service with its exec set)
orchd plant  # cutting→ beds       (render native launchd/systemd files)
orchd tend   # beds   → running    (write, install, start)
```

### Reload gotcha

`orchd grow` on an **already-loaded** job rewrites the plist but launchd keeps
the old in-memory job — the process keeps its old args. Force a true reload:

```sh
orchd --user fell --keep-data && orchd --user grow
# or, per service:
launchctl bootout gui/$(id -u)/com.dwarfstar.ds4server && orchd --user grow
```

---

## Mutual exclusion (the "one GPU, many models" problem)

DwarfStar has its own instance guard. If `DS4_LOCK_FILE` is set, the server takes
an **exclusive, non-blocking `flock`** on it at startup:

- first process: locks the file and writes its PID;
- any later process: `flock(…, LOCK_EX | LOCK_NB)` returns `EWOULDBLOCK`, prints
  `another ds4 process is already running (pid N); refusing to start`, and
  `exit(2)`.

Unset, it defaults to `/tmp/ds4.lock`. The lock is **per file path** and is
released when the process exits.

That gives you two policies:

**1. Concurrency (what's configured here).** Each model gets its *own* lock file
(`ds4-flash`, `ds41-flash`, `ds4-glm53`), so several servers coexist. The lock
only stops an accidental duplicate of the *same* model. Heads-up: one Metal GPU
serves them all, so concurrent decode contends — good for "always available",
bad for peak latency.

**2. Single-model mutex (serialise the GPU).** Point every service at the *same*
lock file:

```dockerfile
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock   # shared by all SERVICES
```

Now whichever model starts first wins the GPU; the rest refuse and exit.

> ⚠️ **Don't combine a shared lock with `RESTART always`.** The losers `exit(2)`,
> which is a failure, so launchd relaunches them forever — a boot loop of
> "refusing to start". To use the shared-lock mutex, keep the losers *stopped*:
> mark them `DISABLED true` and start only the one you want.

### orchd-level mutex: `DISABLED` + targeted `grow`

The clean way to pick one heavy model at a time is to declare them all, disable
the ones you don't want running, and name the one to launch:

```dockerfile
SERVICE ds4server
RUN … --port 8888 …
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock   # shared guard
DISABLED true

SERVICE glm53
RUN … --port 8889 …
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
DISABLED true
```

```sh
orchd --user grow ds4server     # start this one; the DISABLED ones stay down
orchd --user survey
orchd --user fell ds4server     # free the GPU, then grow another
```

`grow` with no names starts every *enabled* service; `grow <name>` starts a
specific one even if `DISABLED` — that's your model switch.

---

## More than start/stop: orchestration primitives

| Directive | Use for |
| --- | --- |
| `REQUIRES a b` | hard dep — don't start until `a`,`b` are ready |
| `AFTER a` | ordering only |
| `HEALTHCHECK http://…` / `cmd` | readiness gate; also makes `survey` show `ready` |
| `READINESS_TIMEOUT 120s` | how long to wait for the healthcheck |
| `ONESHOT true` | run-to-completion (e.g. a conversion/prefetch step) |
| `DISABLED true` | declared, not auto-started |
| `USER`, `STOP`, `RELOAD` | host-mode lifecycle |
| `STDOUT`/`STDERR`, `ENV`, `ENV_FILE` | plumbing |
| `RESTART`, `RESTART_DELAY`, `START_LIMIT_BURST` | crash behaviour |

Readiness is the missing piece for DS4: give a service a healthcheck and
`orchd survey` turns its `inactive` column into real `ready` state, and any
`REQUIRES`-dependent service will actually wait for the model to load.

```dockerfile
SERVICE ds41server
RUN …
HEALTHCHECK http://127.0.0.1:8887/v1/models
READINESS_TIMEOUT 180s
```

---

## Performance knobs that matter

- **Leave the expert cache automatic.** Don't pass
  `--ssd-streaming-cache-experts 96GB` unless you've measured it. The auto budget
  is already ~75% of RAM; an explicit oversized value can displace the
  always-resident non-routed weights and *slow decode*. (`--ssd-streaming-preload-experts N`
  is documented as a measurement knob.)
- **`--batched-session N`** keeps N resident sessions and batches concurrent
  requests. Without it there's a single session. Cost: N × context memory. V4
  Flash shares one prefill workspace across slots (cheap); **V4.1 Flash does
  not**, so 4 × 64K context is a real ~33 GiB.
- **Right-size `--ctx`.** It scales KV/buffer memory; 100K is only worth it if
  you actually use 100K.
- **One Model at a time for peak speed.** See the mutex section.

# Running DwarfStar (ds4) model servers with `orchd`

One `Orchfile` replaces a pile of hand-written `com.dwarfstar.*.plist`
LaunchAgents. `orchd` turns it into native launchd jobs — start order, restart,
logs, env, clean stop — so you can bring any DwarfStar model up with one command.

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

The same pattern extends to the rest of the catalogue — **DeepSeek PRO**
(`pro-q2-imatrix`, `pro-q4-split`), **Qwen3.8 Flash Next** (`qwen38-q2`,
`qwen38-q4k`), the GLM community quants (`glm-antirez-*`, `glm-unsloth-q4`), and
the vision encoders (`*-vision`) — just add another `SERVICE` block.

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

SERVICE ds4server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4-Flash-Layers37-42Q4KExperts-OtherExpertLayersIQ2XXSGateUp-Q2KDown-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-fixed-0731.gguf --host 0.0.0.0 --port 8888 --cors --ctx 100000 --ssd-streaming --kv-disk-dir ~/.ds4/server-kv --kv-disk-space-mb 16384 --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-flash.lock
STDOUT /tmp/ds4-server.log
STDERR /tmp/ds4-server.log
RESTART always

SERVICE ds4flash-q2
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix-0731.gguf --host 0.0.0.0 --port 8890 --cors --ctx 32768 --ssd-streaming --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-flash-q2.lock
STDOUT /tmp/ds4-flash-q2.log
STDERR /tmp/ds4-flash-q2.log
RESTART always

SERVICE ds4flash-q2-legacy
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf --host 0.0.0.0 --port 8891 --cors --ctx 32768 --ssd-streaming --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-flash-q2-legacy.lock
STDOUT /tmp/ds4-flash-q2-legacy.log
STDERR /tmp/ds4-flash-q2-legacy.log
RESTART always

SERVICE ds41server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/DeepSeek-V4.1-Flash-Q2.gguf --host 0.0.0.0 --port 8887 --cors --ctx 65536 --ssd-streaming --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds41-flash.lock
STDOUT /tmp/ds41-server.log
STDERR /tmp/ds41-server.log
RESTART always

SERVICE glm53server
RUN bash -c 'cd ~/projects/ds4 && exec nice -n 10 ~/projects/ds4/ds4-server --model ~/projects/ds4/gguf/GLM-5.3-Flash-Q2.gguf --host 0.0.0.0 --port 8889 --cors --backend metal --ctx 32768 --ssd-streaming --batched-session 4 --threads 8'
ENV DS4_LOCK_FILE=/tmp/ds4-glm53.lock
STDOUT /tmp/ds4-glm53.log
STDERR /tmp/ds4-glm53.log
RESTART always
```

### Why it looks like that

- **`RUN bash -c 'cd … && exec …'`** — launchd does **not** expand `~` in
  `WorkingDirectory`, and `orchd` resolves a relative `WORKDIR` against the
  project dir (giving `/Users/you/ds4-runner/~/…`). The inner shell `cd`s to the
  real project dir and `exec` keeps the PID, matching the old `WorkingDirectory`.
- **`nice -n 10`** — the plists had `Nice 10`; the Orchfile grammar has no `NICE`
  directive, so it rides the command line.
- **`RESTART always`** → `KeepAlive`. `RESTART on-failure` → `KeepAlive` with
  `SuccessfulExit=false`.
- **`STDOUT`/`STDERR`** keep the `/tmp/*.log` destinations instead of orchd's
  default `~/Library/Logs/<label>.{out,err}.log`.
- **Ports** are unique per model so they can coexist: `8888 8887 8889 8890 8891`.
- **`ENV DS4_LOCK_FILE=…`** — the per-model GPU mutex (see below).

---

## Day to day

Run from this directory (that's how `.orchrc` and `./Orchfile` are found):

```sh
orchd --user grow           # sow → plant → tend: write plists, install, start
orchd --user grow ds4server # start just one
orchd --user survey         # table of services: state, PID
orchd --user survey --json
orchd --user logs ds4server # tail one service (--follow)
orchd --user fell           # stop everything and remove the units
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

## Mutual exclusion (one GPU, many models)

DwarfStar has its own instance guard. With `DS4_LOCK_FILE` set, the server takes
an **exclusive, non-blocking `flock`** at startup:

- first process: locks the file and writes its PID;
- any later process: `LOCK_EX | LOCK_NB` returns `EWOULDBLOCK`, prints
  `another ds4 process is already running (pid N); refusing to start`, and
  `exit(2)`.

Unset, it defaults to `/tmp/ds4.lock`. The lock is **per file path** and released
when the process exits. That gives two policies:

**1. Concurrency (what this file does).** Each model gets its *own* lock file, so
several servers coexist; the lock only stops an accidental duplicate of the
*same* model. One GPU still serves them all, so concurrent decode contends —
great for "always available", worse for peak latency.

**2. Single-model mutex (serialise the GPU).** Point every service at the *same*
lock file:

```
ENV DS4_LOCK_FILE=/tmp/ds4-gpu.lock
```

Now whichever model starts first wins; the rest refuse and exit.

> ⚠️ **Never combine a shared lock with `RESTART always`.** The losers `exit(2)`,
> which counts as failure, so launchd relaunches them forever — a boot loop of
> "refusing to start". For a shared-lock mutex set the losers to `RESTART no` so
> they stay down (the winner simply isn't auto-restarted either — you drive it).

**Note on `DISABLED`:** in this orchd build a `DISABLED true` service is not
generated or installed at all, so `orchd grow <name>` **cannot** start it. To
switch models, either keep only the model you want in the file, or bracket the
`SERVICE` blocks you don't want (the file parses fine with a subset), or share the
lock with `RESTART no`.

---

## More than start/stop

| Directive | Use for |
| --- | --- |
| `REQUIRES a b` | hard dep — don't start until `a`,`b` are ready |
| `AFTER a` | ordering only |
| `HEALTHCHECK http://…` / `cmd` | readiness gate for `REQUIRES` dependents |
| `ONESHOT true` | run-to-completion (e.g. a conversion/prefetch step) |
| `DISABLED true` | declared, not installed (see note above) |
| `USER`, `STOP`, `RELOAD` | host-mode lifecycle |
| `STDOUT`/`STDERR`, `ENV`, `ENV_FILE` | plumbing |
| `RESTART`, `RESTART_DELAY` | crash behaviour |

---

## Performance knobs that matter

- **Resident beats streaming when it fits.** On a 128 GB Mac the ≤90 GiB models
  (V4 Flash Q2 at 81 GiB, GLM 5.3 Q2 at 90 GiB) can run *resident* — drop
  `--ssd-streaming` for the fastest decode. 91 GiB and 341 GiB still need it.
  Resident assumes you run **one model at a time**.
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

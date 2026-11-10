# DwarfStar (ds4) model servers via `orchd`

DwarfStar model checkpoints share one GPU behind a single HTTP address. A client sends
an OpenAI request to `http://127.0.0.1:8899/v1` and names the model it wants. A
small C program called the arbiter decides which model holds the GPU, starts it
when it is down, and forwards the request. Everything else runs on stock
`orchd` 0.5.0, which needs `orch` 0.3 or newer.

## How to use

```sh
./run      # start the front door
./stop     # stop everything
```

`./run` also starts `watch`, which posts a macOS notification, "Watch out!
Dwarfstar is cooking …", when a runner model pushes the chip into thermal
throttling, memory pressure or swap growth, when a server logs a failure such
as running out of GPU memory, or when the disk holding the models runs low.
`./watch --test` sends one to check that notifications reach you. Its
thresholds are the `WATCH_*` ARGs in the `Orchfile`.

The models run ds4 from the checkout in `DWARFSTAR_DIR`, which defaults to
`~/ds4`. If yours is elsewhere, pass it when you start the front door:

```sh
ORCH_ARG_DWARFSTAR_DIR=~/src/ds4 ./run
```

The arbiter keeps that value and hands it to every model it starts, so set it
once per `./run`. `ORCH_ARG_<name>` is orch's way to override any Orchfile
`ARG`.

The runner keeps its own files under `~/.ds4/dwarfstar-runner`, beside ds4's
saved sessions in `~/.ds4/kvcache`:

- `logs/<service>.log`, `logs/arbiter.log` and `logs/watch.log`: everything
  each server, the arbiter and the watch print, a failure before ds4 starts
  included.
- `kv/<service>/`: each model's disk KV cache, capped by the service's
  `--kv-disk-space-mb`. A vision model shares its text sibling's cache.

The front door reads its roster from the `Orchfile`, which has one `SERVICE`
per model beside the arbiter and the watch. A model is advertised only while every file in its `ENV WEIGHTS` is
on disk, so models appear as you download them. A request for one whose files
are missing gets a 404 naming the absent file.

To add a model of your own, or change a shipped one, without editing the
`Orchfile`, put an Orchfile in `servers/`. Git ignores that directory, and every
`servers/*.orch` is laid over the `Orchfile`:

```sh
ln -s ~/my-models/llama.orch servers/llama.orch
```

Start once, then point any OpenAI client at `http://127.0.0.1:8899/v1` and name a
model. That address is the only one a client needs to know. Models appear and
disappear as requests arrive, so there are no per-model commands to run.

## Models

One service per DwarfStar download, set up for a 128 GB Apple Silicon Mac.
Each runs `${DWARFSTAR_DIR}/ds4-server` with its GGUFs under
`${DWARFSTAR_DIR}/gguf/`; fetch a model with `./download_model.sh <target>` in
the ds4 checkout. *Resident* models are held
in memory; *streaming* models are larger than RAM and read routed experts from
SSD (`--ssd-streaming`). Speculative decoding is on wherever the model supports
it with a resident target: DSpark for V4 Flash, the built-in MTP block for GLM
and Qwen.

| Service | Model ids | ds4 download target | Mode | Port |
|---|---|---|---|---|
| `ds4server` | `deepseek-v4-flash` | `ds4f-q2-q4`, `ds4f-dspark` | resident, DSpark | 8888 |
| `ds4flash-q2` | `deepseek-v4-flash-iq2` | `ds4f-q2`, `ds4f-dspark` | resident, DSpark | 8890 |
| `ds4flash-q2-legacy` | `deepseek-v4-flash-iq2-legacy` | none (pre-0731 checkpoint) | resident | 8891 |
| `ds4flash-q4` | `deepseek-v4-flash-q4` | `ds4f-q4` | streaming | 8892 |
| `ds4flash-mxfp4` | `deepseek-v4-flash-mxfp4` | `ds4f-mxfp4` | streaming | 8893 |
| `ds4vision-q2` | `deepseek-v4-flash-vision`, `deepseek-v4-flash-vision-q2` | `ds4f-vision-q2`, `ds4f-vision-dspark` | resident, DSpark, vision | 8894 |
| `ds4vision-q2-q4` | `deepseek-v4-flash-vision-q2-q4` | `ds4f-vision-q2-q4`, `ds4f-vision-dspark` | resident, DSpark, vision | 8895 |
| `ds4vision-mxfp4` | `deepseek-v4-flash-vision-mxfp4` | `ds4f-vision-mxfp4` | streaming, vision | 8896 |
| `ds41server` | `deepseek-v4.1-flash` | `ds41f-q2` | streaming | 8887 |
| `ds41flash-vision` | `deepseek-v4.1-flash-vision` | `ds41f-q2`, `ds41f-vision` | streaming, vision | 8900 |
| `ds41flash-q4` | `deepseek-v4.1-flash-q4` | `ds41f-q4` | streaming | 8897 |
| `ds41flash-mxfp4` | `deepseek-v4.1-flash-mxfp4` | `ds41f-mxfp4` | streaming | 8898 |
| `ds4pro-q2` | `deepseek-v4-pro-q2` | `pro-q2-imatrix` | streaming, slow on 128 GB | 8901 |
| `glm53server` | `glm-5.3-flash`, `glm-5.3-flash-chat`, `glm-5.3-flash-reasoner` | `glm53-q2` | resident, MTP | 8889 |
| `glm53flash-vision` | `glm-5.3-flash-vision` | `glm53-q2`, `glm53-vision` | resident, MTP, vision | 8903 |
| `glm53flash-q4` | `glm-5.3-flash-q4` | `glm53-q4` | streaming | 8902 |
| `glm53full-q2` | `glm-5.3`, `glm-5.3-q2` | `glm53-full-q2` | streaming | 8904 |
| `glm52-iq2xxs` | `glm-5.2-iq2xxs` | `glm-antirez-iq2xxs` | streaming | 8905 |
| `glm52-q2` | `glm-5.2-q2` | `glm-antirez-q2` | streaming | 8906 |
| `glm52-q4` | `glm-5.2-q4` | `glm-antirez-q4` | streaming | 8907 |
| `glm52-unsloth-q4` | `glm-5.2-unsloth-q4` | `glm-unsloth-q4` | streaming | 8908 |
| `qwen38-q4k` | `qwen3.8-flash-next`, `qwen3.8-flash-next-q4` | `qwen38-q4k` | resident, MTP | 8910 |
| `qwen38-q2` | `qwen3.8-flash-next-q2` | `qwen38-q2` | resident, MTP | 8909 |
| `qwen38-q4k-vision` | `qwen3.8-flash-next-vision` | `qwen38-q4k`, `qwen38-vision` | resident, MTP, vision | 8911 |

Model ids are unique across services because the arbiter answers with the
first service, in file order, that lists the id.

The `-chat` and `-reasoner` ids pick thinking per request: ds4-server turns
thinking off for `glm-5.3-flash-chat` and on for `glm-5.3-flash-reasoner`, and
leaves its default (on) for the plain id. A request's own `thinking`,
`reasoning_effort` or `think` field takes precedence over the id.

A vision service loads the same weights as its text sibling plus an encoder,
so the pair cannot run together; the shared lock keeps one at a time.

Services run without `--batched-session`, which turns DSpark and MTP off. Add
it to a service when several clients share one model at once.

### GPU memory for resident models

A resident model of about 90 GiB needs more GPU wired memory than macOS grants
by default on a 128 GB Mac. Without it the first prefill fails with
`kIOGPUCommandBufferCallbackErrorOutOfMemory`. Raise the limit once per boot:

```sh
sudo sysctl iogpu.wired_limit_mb=110000
```

That matches Metal's recommended working set on a 128 GB M5 Max and leaves
room for the system. It is a ceiling, not a reservation.

## Adapting it to your use case

The front door accepts any server that follows the conventions below, whether
it is a `SERVICE` in the `Orchfile` or in a file of your own under `servers/`.

- Set `ENV MODELS` to the names clients will ask for. That is what makes a
  service a model; the arbiter ignores a service without it.
- Give each service its own name. A service in `servers/` with the name of one
  in the `Orchfile` changes that one.
- Set `DISABLED true`. orchd then starts the service only when the arbiter
  names it. Without it, `./run` starts the model with the front door, and
  launchd starts it at every login.
- Set `HEALTHCHECK` to an `http://` URL that answers 200 once the service is
  ready. The arbiter takes the model's address from it and relays requests
  there.
- A file in `servers/` that does not validate over the `Orchfile` is left out
  of the roster. The arbiter reads the roster when it starts, so run `./stop`
  and `./run` after a change.
- Set `ENV WEIGHTS` to the files the server cannot start without, separated by
  commas; `~` and `${ARG}` references work. The arbiter checks them on every
  request and advertises or raises the model only while all are on disk.
  Without it the model is always advertised.
- Give every service that must not run at the same time the same
  `DS4_LOCK_FILE`.

The services must answer an OpenAI request, because the arbiter relays the body
unchanged and reads the `model` field from it. Anything that accepts
`POST /v1/chat/completions` with a `model` key will work.

Two settings of one model become two services. The weights stay on disk once,
and the shared lock keeps the pair from running together.

The front door enforces one holder across the whole roster. Every switch stops
the other models, so all models are mutually exclusive by construction. Two
models that need to be resident at the same time cannot be expressed here.

## How it works

[orchd](https://github.com/adiled/orchd) starts and supervises processes from a
declarative file called an Orchfile. An Orchfile lists the services to run, the
command for each service, its environment, and a probe that reports whether the
service is ready. The Orchfile spec is documented under [Write an
Orchfile](https://github.com/adiled/orchd#write-an-orchfile), and the model that
gives it its vocabulary of groves, sow, plant and tend is described in [The
Orchard Model](https://github.com/adiled/orchd/blob/main/ORCHARD.md).

The `Orchfile` here is the whole runner: the arbiter, the watch, and a
`SERVICE` per model. They share one grove, named `dwarfstar` in `.orchrc`, with
its state in `~/.orch/g/dwarfstar`. Each service's working directory and log
file are set with `WORKDIR`, `STDOUT` and `STDERR`, so a `RUN` line is the bare
command.

`./run` is `orchd grow`. The models are `DISABLED`, so that starts the arbiter
and the watch and nothing else. `./stop` is `orchd fell`, which takes down the
whole grove, a running model included. To look at the grove yourself, run this
from the repository:

```sh
orchd --state-dir ~/.orch/g/dwarfstar survey
```

The arbiter resolves the `Orchfile` with `orch resolve --json` and takes every
service that sets `ENV MODELS` as its roster. For each request it resolves the
model name, asks `orchd survey` which model holds the GPU, stops it with
`orchd fell <model>`, starts the target with `orchd grow <model>`, waits for
the target's `HEALTHCHECK` to answer, and relays the request to that address.
Felling and growing one model leaves the rest of the grove alone, the arbiter
included. One switch runs at a time. Response streams pass through unchanged,
and every connection forks its own process so a long switch cannot hold up
other clients.

The public interface is OpenAI's `/v1`. The `ENV MODELS` line in each service
lists the names its model answers to, and `ENV WEIGHTS` the files it needs. Both
are ordinary `ENV` directives, set in the server's environment like any other,
that the arbiter also reads from `orch resolve --json`.

ds4 holds an exclusive lock on `DS4_LOCK_FILE`, and every service points at the
same file. One model can hold the GPU at a time, whichever way it was started.
The arbiter stops the current model before it starts the next, which keeps the
lock free during normal use.

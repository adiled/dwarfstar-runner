# DwarfStar (ds4) model servers via `orchd`

DwarfStar model checkpoints share one GPU behind a single HTTP address. A client sends
an OpenAI request to `http://127.0.0.1:8899/v1` and names the model it wants. A
small C program called the arbiter decides which model holds the GPU, starts it
when it is down, and forwards the request. Everything else runs on stock
`orchd` 0.4.0.

## How to use

```sh
./run      # start the front door
./stop     # stop everything
```

The templates run ds4 from the checkout in `DWARFSTAR_DIR`, which defaults to
`~/ds4`. If yours is elsewhere, pass it when you start the front door:

```sh
ORCH_ARG_DWARFSTAR_DIR=~/src/ds4 ./run
```

The arbiter keeps that value and hands it to every model it starts, so set it
once per `./run`. `ORCH_ARG_<name>` is orch's way to override any Orchfile
`ARG`.

The runner keeps its own files under `~/.ds4/dwarfstar-runner`, beside ds4's
saved sessions in `~/.ds4/kvcache`:

- `logs/<template>.log` and `logs/arbiter.log`: each server's and the
  arbiter's output.
- `kv/<template>/`: each model's disk KV cache, capped by the template's
  `--kv-disk-space-mb`. A vision template shares its text sibling's cache.

A server that fails before ds4 starts, for instance on a wrong
`DWARFSTAR_DIR`, reports it in orchd's launchd log instead:
`~/Library/Logs/<template>.<template>.err.log`.

The front door reads its roster from `servers/`. The repository ships examples in
`servers-template/`, and on a fresh clone `servers/` is empty, so `./run` will
exit with `no specs matched`. Link every template in, or copy the ones you want:

```sh
for f in servers-template/*.orch; do ln -s "../$f" servers/; done
cp servers-template/ds4server.orch servers/
```

A template is advertised only while every file in its `ENV WEIGHTS` is on disk,
so models appear as you download them. A request for one whose files are
missing gets a 404 naming the absent file, instead of waiting on a server that
cannot start.

Or bring in an Orchfile you keep elsewhere:

```sh
ln -s ~/my-models/llama.orch servers/llama.orch
```

Start once, then point any OpenAI client at `http://127.0.0.1:8899/v1` and name a
model. That address is the only one a client needs to know. Models appear and
disappear as requests arrive, so there are no per-model commands to run.

## Templates

One template per DwarfStar download, set up for a 128 GB Apple Silicon Mac.
Each runs `${DWARFSTAR_DIR}/ds4-server` with its GGUFs under
`${DWARFSTAR_DIR}/gguf/`; fetch a model with `./download_model.sh <target>` in
the ds4 checkout. *Resident* models are held
in memory; *streaming* models are larger than RAM and read routed experts from
SSD (`--ssd-streaming`). Speculative decoding is on wherever the model supports
it with a resident target: DSpark for V4 Flash, the built-in MTP block for GLM
and Qwen.

| Template | Model ids | ds4 download target | Mode | Port |
|---|---|---|---|---|
| `ds4server` | `deepseek-v4-flash` | `ds4f-q2-q4`, `ds4f-dspark` | resident, DSpark | 8888 |
| `ds4flash-q2` | `deepseek-v4-flash-iq2` | `ds4f-q2`, `ds4f-dspark` | resident, DSpark | 8890 |
| `ds4flash-q2-legacy` | `deepseek-v4-flash-iq2-legacy` | pre-0731 Q2, no longer offered | resident | 8891 |
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
| `glm53server` | `glm-5.2`, `glm-5.2-chat`, `glm-5.2-reasoner` | `glm53-q2` | resident, MTP | 8889 |
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

`glm53server` serves GLM 5.3 Flash under the `glm-5.2` ids, the names its
clients already ask for. Model ids are unique across templates because the
arbiter answers with the first file, in name order, that lists the id.

A vision template loads the same weights as its text sibling plus an encoder,
so the pair cannot run together; the shared lock keeps one at a time.

Server-side session batching (`--batched-session`) is left off: it disables
DSpark and MTP, and GLM 5.3 and streaming V4.1 gain little from it. Add it to a
template when several clients share one model at once.

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

The front door accepts any server that follows the conventions below. The
directory `servers/` is ignored by git, so the Orchfiles in it are yours to
arrange however you like.

- The arbiter globs `servers/*.orch`, so the directory and the extension are
  fixed.
- It reads the first service in the file, so keep one service per Orchfile. A
  second service in the file would be grown by orchd and remain invisible to the
  arbiter.
- Name the file after its service. The service name becomes the grove's state
  directory and namespace, and `run` and `stop` derive the same name from the
  file name. When the two differ, `./stop` will not find the grove the arbiter
  created.
- The `RUN` line must contain `--port N`. The arbiter reads the port from that
  flag and connects to `127.0.0.1:N`.
- Set `HEALTHCHECK` to a URL that answers 200 once the service is ready. Without
  it the arbiter falls back to `http://127.0.0.1:N/v1/models`.
- Set `ENV MODELS` to the names clients will ask for. Without it the arbiter
  advertises the service name.
- Set `ENV WEIGHTS` to the files the server cannot start without, separated by
  commas; `~` and `${ARG}` references work. The arbiter checks them on every
  request and advertises or raises the grove only while all are on disk.
  Without it the grove is always advertised.
- Give every service that must not run at the same time the same
  `DS4_LOCK_FILE`.

The services must answer an OpenAI request, because the arbiter relays the body
unchanged and reads the `model` field from it. Anything that accepts
`POST /v1/chat/completions` with a `model` key will work.

Two settings of one model become two Orchfiles. The weights stay on disk once,
and the shared lock keeps the pair from running together.

The front door enforces one holder across the whole roster. Every switch stops
the other groves, so all models are mutually exclusive by construction. Two
models that need to be resident at the same time cannot be expressed here.

## How it works

[orchd](https://github.com/adiled/orchd) starts and supervises processes from a
declarative file called an Orchfile. An Orchfile lists the services to run, the
command for each service, its environment, and a probe that reports whether the
service is ready. The Orchfile spec is documented under [Write an
Orchfile](https://github.com/adiled/orchd#write-an-orchfile), and the model that
gives it its vocabulary of groves, sow, plant and tend is described in [The
Orchard Model](https://github.com/adiled/orchd/blob/main/ORCHARD.md).

Each model here has its own Orchfile in `servers/`. The arbiter reads that
directory, parses every Orchfile with `orch parse`, and treats the result as the
roster, so the available models are the files that are present. Each Orchfile is
planted into its own grove, which is a namespace with its own state directory.
Separate groves are what allow the arbiter to stop one model while the rest stay
untouched.

The arbiter runs in its own grove. For each request it resolves the model name,
asks `orchd survey` which grove holds the GPU, stops the other groves with
`orchd fell`, starts the target with `orchd grow`, waits for the target's
`HEALTHCHECK` to answer, and relays the request to that model's port. Response
streams pass through unchanged, and every connection forks its own process so a
long switch cannot hold up other clients.

The public interface is OpenAI's `/v1`. The `ENV MODELS` line in each Orchfile
lists the names its grove answers to, and `ENV WEIGHTS` the files it needs. Both
are ordinary `ENV` directives, set in the server's environment like any other,
that the arbiter also reads from `orch parse`.

ds4 holds an exclusive lock on `DS4_LOCK_FILE`, and every Orchfile points at the
same file. One model can hold the GPU at a time, whichever way it was started.
The arbiter stops the current model before it starts the next, which keeps the
lock free during normal use.

## Roadmap

This repository runs one grove per model plus a controller grove for the arbiter.
The arbiter exists because orchd does not yet cover the work directly, so the
shape of this repository tracks the state of that project.

What orchd 0.4.0 does not do yet:

- `fell` removes the whole state directory of a grove, and no verb stops one
  service on its own. The `orchdi` platform contains `stop(services)`, and the
  CLI does not expose it.
- `RELOAD` is in the Orchfile grammar and unimplemented, so an edit needs `fell`
  and `grow`. Tracked as [orchd#54](https://github.com/adiled/orchd/issues/54).
- `RECREATE` is unimplemented. Tracked as
  [orchd#59](https://github.com/adiled/orchd/issues/59).

With a per-service stop and an in-place reload, the models could share a
single grove. The arbiter would still choose which model runs, and the
grove-per-model split would disappear.

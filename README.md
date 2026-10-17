# DwarfStar (ds4) model servers via `orchd`

Five model checkpoints share one GPU behind a single HTTP address. A client sends
an OpenAI request to `http://127.0.0.1:8899/v1` and names the model it wants. A
small C program called the arbiter decides which model holds the GPU, starts it
when it is down, and forwards the request. Everything else runs on stock
`orchd` 0.3.4.

## How to use

```sh
./run      # start the front door
./stop     # stop everything
```

The front door reads its roster from `servers/`. The repository ships examples in
`servers-template/`, and on a fresh clone `servers/` is empty, so `./run` will
exit with `no specs matched`. Copy what you need, or symlink it in from wherever
you keep it:

```sh
cp servers-template/ds4server.orch servers/
ln -s ~/my-models/llama.orch servers/llama.orch
```

Start once, then point any OpenAI client at `http://127.0.0.1:8899/v1` and name a
model. That address is the only one a client needs to know. Models appear and
disappear as requests arrive, so there are no per-model commands to run.

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
lists the names its grove answers to.

ds4 holds an exclusive lock on `DS4_LOCK_FILE`, and every Orchfile points at the
same file. One model can hold the GPU at a time, whichever way it was started.
The arbiter stops the current model before it starts the next, which keeps the
lock free during normal use.

## Roadmap

This repository runs one grove per model plus a controller grove for the arbiter.
The arbiter exists because orchd does not yet cover the work directly, so the
shape of this repository tracks the state of that project.

What orchd does not do yet:

- `fell` removes the whole state directory of a grove, and no verb stops one
  service on its own. The `orchdi` platform contains `stop(services)`, and the
  CLI does not expose it.
- `RELOAD` is in the Orchfile grammar and unimplemented, so an edit needs `fell`
  and `grow`. Tracked as [orchd#54](https://github.com/adiled/orchd/issues/54).
- `RECREATE` is unimplemented. Tracked as
  [orchd#59](https://github.com/adiled/orchd/issues/59).
- `survey` reports a running service as inactive, which leaves the process id as
  the only reliable field. Tracked as
  [orchd#56](https://github.com/adiled/orchd/issues/56).

With a per-service stop and an in-place reload, the five models could share a
single grove. The arbiter would still choose which model runs, and the
grove-per-model split would disappear.

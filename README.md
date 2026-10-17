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

Start once, then point any OpenAI client at `http://127.0.0.1:8899/v1` and name a
model. That address is the only one a client needs to know. Models appear and
disappear as requests arrive, so there are no per-model commands to run.

## How it works

[orchd](https://github.com/adiled/orchd) starts and supervises processes from a
declarative file called an Orchfile. An Orchfile lists the services to run, the
command for each service, its environment, and a probe that reports whether the
service is ready. The Orchfile spec is documented under [Write an
Orchfile](https://github.com/adiled/orchd#write-an-orchfile), and the model that
gives it its vocabulary of groves, sow, plant and tend is described in [The
Orchard Model](https://github.com/adiled/orchd/blob/main/ORCHARD.md).

Each model here has its own Orchfile in `services/`. The arbiter reads that
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

The repository currently runs one grove per model plus a controller grove for the
arbiter. The intended end state is a single grove that needs no arbiter. Three
limitations in `orchd` 0.3.4 cause the split, and each one corresponds to a
missing capability.

| Limitation | Missing capability | Effect once it exists |
| --- | --- | --- |
| `fell` removes a whole state directory, and there is no way to stop one service | per-service stop in the `orchdi` platform, tracked by pidfile | models can share a single grove |
| an Orchfile is read once, so an edit needs `fell` and `grow` | adoption of an edited Orchfile, which reconciles it in place | one file can hold the roster and the selected model |
| nothing states that two services cannot run together | an `EXCLUSIVE <resource>` directive with `PREEMPT` | `orchd` performs the GPU swap itself |

With those in place the Orchfiles merge into a single file and the arbiter goes
away:

```dockerfile
SERVICE glm53server
RUN ... --port 8889 ...
EXCLUSIVE gpu
PREEMPT ds4server
```

A switch then becomes `orchd --platform orchdi grow glm53server`. The arbiter is a
hand-rolled version of this loop: a grove stands in for the service, `fell` stands
in for the stop, and the lock stands in for the exclusive slot.

# worker_app

Background worker kit for modern C++.

`worker_app` provides a minimal, deterministic background processing foundation built on top of `vix/app`.

It enables:

- Job handlers
- In-memory queue abstraction
- Retry policies
- Delayed jobs
- Interval scheduled tasks
- Deterministic worker loops

Header-only. Layered. Explicit.

## Download

https://vixcpp.com/registry/pkg/vix/worker_app

## Why worker_app?

Many systems require background processing for:

- Sending emails
- Processing events
- Running scheduled tasks
- Retrying failed operations
- Performing asynchronous work

Most C++ backends either:

- Hardcode background threads
- Mix job logic with application logic
- Reimplement retry systems
- Build ad-hoc worker loops
- Depend on heavy queue systems

`worker_app` provides:

- Deterministic worker ticks
- Pluggable queue backend
- Structured retry policies
- Interval task scheduling
- Clean separation of job logic

No mandatory queue backend.
No threadpool required.
No networking required.

Just a structured worker foundation.

## Dependency

`worker_app` depends on:

- `vix/app`

Architecture layering:

```
vix/app
  ↑
vix/worker_app
```

This ensures:

- Minimal runtime dependencies
- Clear lifecycle control
- Deterministic execution model
- Composable worker infrastructure

Dependencies are installed automatically via Vix Registry.

## Installation

### Using Vix Registry

```bash
vix add vix/worker_app
vix deps
```

### Manual

```bash
git clone https://github.com/vixcpp/worker_app.git
```

Add the `include/` directory and ensure dependencies are available.

## Core concepts

### Job envelope

A job is represented by `JobEnvelope`:

```cpp
JobEnvelope job;
job.id = "job-1";
job.type = "send_email";
job.payload = "{\"user\":123}";
```

The payload format is not enforced.

It can be JSON, msgpack, protobuf, or any serialized data.

### Job handlers

Register a handler for each job type:

```cpp
WorkerApp app;

app.on("send_email", [](const JobEnvelope& job, WorkerContext& ctx) {
    ctx.info("sending email");
    return JobResult::Success;
});
```

Handlers return:

- `JobResult::Success`
- `JobResult::Retry`
- `JobResult::Fail`

### Enqueue jobs

```cpp
JobEnvelope job;
job.type = "send_email";

app.enqueue(job);
```

Delayed jobs:

```cpp
app.enqueue_in(job, std::chrono::seconds(10));
```

### Worker tick

The worker can run in two ways.

Single deterministic tick:

```cpp
app.tick_once();
```

Continuous worker loop:

```cpp
app.run();
```

`run()` executes the worker loop until `stop()` is called.

## Retry policies

### Fixed retry

```cpp
FixedRetryPolicy retry(
    3,
    std::chrono::seconds(1)
);

app.set_retry_policy(&retry);
```

Retry sequence:

- attempt 1
- attempt 2
- attempt 3

### Exponential retry

```cpp
ExponentialRetryPolicy retry(
    5,
    std::chrono::milliseconds(500)
);
```

Retry delays:

- 500ms
- 1000ms
- 2000ms
- 4000ms
- ...

The delay can be capped.

## Scheduled tasks

Interval tasks run automatically during worker ticks:

```cpp
app.schedule_every(
    "cleanup",
    std::chrono::seconds(60),
    [](WorkerContext& ctx) {
        ctx.info("running cleanup task");
    }
);
```

This is deterministic interval scheduling.

It is not a cron parser.

## Worker context

Jobs receive a `WorkerContext`:

```cpp
struct WorkerContext
{
    std::string worker_id;
    std::uint64_t tick;
};
```

It also supports optional logging callbacks:

```cpp
app.set_logger(
    [](std::string_view m){ std::cout << "[info] " << m << "\n"; }
);
```

## Queue abstraction

`worker_app` uses a queue interface.

Default implementation:

- `InMemoryQueue`

You can implement your own backend:

```cpp
class RedisQueue : public JobQueue
{
public:
    void push(JobEnvelope env) override;
    void push_delayed(JobEnvelope env, WorkerClock::time_point run_at) override;
    std::optional<JobEnvelope> try_pop_ready() override;
};
```

This allows integration with:

- Redis
- SQS
- Kafka
- database-backed queues

without changing worker logic.

## Worker loop

`WorkerApp::run_loop()` performs:

1. Run scheduled tasks
2. Pop one ready job
3. Execute handler
4. Apply retry policy if needed
5. Sleep if idle

This keeps execution predictable and deterministic.

## Complexity

| Operation         | Complexity |
|------------------|------------|
| Handler lookup    | O(n) |
| Queue pop         | depends on backend |
| Retry policy      | O(1) |
| Scheduled tasks   | O(n) |
| Worker tick       | O(n) |

For small-to-medium worker sets this is deterministic and predictable.

## Design philosophy

`worker_app` focuses on:

- Deterministic worker execution
- Explicit job handling
- Minimal runtime assumptions
- Clear retry semantics
- Pluggable queue infrastructure

It does not aim to replace:

- Distributed job systems
- Full queue orchestration
- Production scheduler frameworks
- Workflow engines

Those belong to higher-level infrastructure layers.

## Tests

Run:

```bash
vix build
vix test
```

Tests verify:

- Job execution
- Retry behavior
- Scheduled tasks
- Queue integration

## License

MIT License\
Copyright (c) Gaspard Kirira


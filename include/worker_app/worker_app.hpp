/**
 * @file worker_app.hpp
 * @brief Background worker kit built on top of vix/app for jobs, queues, retries, and scheduled tasks.
 *
 * `worker_app` provides a deterministic foundation for background processing:
 *
 * - Job interface (execute with context)
 * - In-memory queue abstraction (pluggable backend)
 * - Retry policy (fixed / exponential backoff)
 * - Delayed jobs (run_at scheduling)
 * - Scheduled tasks (simple interval scheduling)
 * - Worker loop helpers (single-threaded, runtime-agnostic)
 *
 * This kit is intentionally lightweight:
 * - No mandatory queue backend (Redis, SQS, etc.) and no network IO
 * - No threading model forced on you
 * - No cron parser requirement
 *
 * You can bind it to:
 * - in-process memory queues (default)
 * - a persistent queue backend (custom adapter)
 * - a multi-worker runtime (your threadpool / processes)
 *
 * Requirements: C++17+
 * Header-only. Depends on `vix/app`.
 */

#ifndef VIX_WORKER_APP_WORKER_APP_HPP
#define VIX_WORKER_APP_WORKER_APP_HPP

#include <app/app.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vix::worker_app
{

  /**
   * @brief Clock helper for worker scheduling.
   */
  struct WorkerClock
  {
    using Clock = std::chrono::steady_clock;
    using time_point = Clock::time_point;

    static time_point now() noexcept { return Clock::now(); }
  };

  /**
   * @brief Generic job payload envelope.
   *
   * `type` is a user-defined identifier (e.g. "send_email", "index_user").
   * `payload` is an opaque string (JSON, msgpack, etc.).
   *
   * The worker kit does not force a serialization format.
   */
  struct JobEnvelope
  {
    std::string id;      // optional unique job id
    std::string type;    // job type
    std::string payload; // opaque
    std::uint32_t attempt = 0;
  };

  /**
   * @brief Execution result for a job run.
   */
  enum class JobResult
  {
    Success,
    Retry,
    Fail
  };

  /**
   * @brief Retry policy for failed jobs.
   *
   * The policy decides:
   * - whether the job should be retried
   * - when it should be retried (delay)
   */
  class RetryPolicy
  {
  public:
    virtual ~RetryPolicy() = default;

    /**
     * @brief Decide retry behavior.
     * @param env Job envelope (contains attempt count)
     * @param error_message A short error summary
     * @return delay until next attempt if retry is allowed, nullopt otherwise
     */
    virtual std::optional<std::chrono::milliseconds>
    next_delay(const JobEnvelope &env, std::string_view error_message) const = 0;
  };

  /**
   * @brief Fixed backoff retry policy.
   */
  class FixedRetryPolicy final : public RetryPolicy
  {
  public:
    FixedRetryPolicy() = default;

    FixedRetryPolicy(std::uint32_t max_attempts, std::chrono::milliseconds delay)
        : max_attempts_(max_attempts), delay_(delay)
    {
      if (max_attempts_ == 0)
        throw std::runtime_error("FixedRetryPolicy: max_attempts must be > 0");
      if (delay_.count() < 0)
        throw std::runtime_error("FixedRetryPolicy: delay must be >= 0ms");
    }

    std::optional<std::chrono::milliseconds>
    next_delay(const JobEnvelope &env, std::string_view) const override
    {
      if (env.attempt + 1 >= max_attempts_)
        return std::nullopt;
      return delay_;
    }

  private:
    std::uint32_t max_attempts_ = 3;
    std::chrono::milliseconds delay_{1000};
  };

  /**
   * @brief Exponential backoff retry policy with optional cap.
   *
   * delay = base * (2^attempt) capped by max_delay (if set).
   */
  class ExponentialRetryPolicy final : public RetryPolicy
  {
  public:
    ExponentialRetryPolicy() = default;

    ExponentialRetryPolicy(std::uint32_t max_attempts,
                           std::chrono::milliseconds base_delay,
                           std::optional<std::chrono::milliseconds> max_delay = std::chrono::milliseconds(30000))
        : max_attempts_(max_attempts), base_(base_delay), cap_(max_delay)
    {
      if (max_attempts_ == 0)
        throw std::runtime_error("ExponentialRetryPolicy: max_attempts must be > 0");
      if (base_.count() < 0)
        throw std::runtime_error("ExponentialRetryPolicy: base_delay must be >= 0ms");
      if (cap_ && cap_->count() < 0)
        throw std::runtime_error("ExponentialRetryPolicy: max_delay must be >= 0ms");
    }

    std::optional<std::chrono::milliseconds>
    next_delay(const JobEnvelope &env, std::string_view) const override
    {
      if (env.attempt + 1 >= max_attempts_)
        return std::nullopt;

      // attempt=0 => base * 1
      // attempt=1 => base * 2
      // attempt=2 => base * 4
      const std::uint32_t shift = env.attempt;
      std::uint64_t mult = 1ULL << (shift > 30 ? 30 : shift); // cap exponent to avoid overflow
      std::uint64_t ms = static_cast<std::uint64_t>(base_.count()) * mult;

      if (ms > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        ms = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

      auto d = std::chrono::milliseconds(static_cast<std::int64_t>(ms));
      if (cap_ && d > *cap_)
        d = *cap_;
      return d;
    }

  private:
    std::uint32_t max_attempts_ = 5;
    std::chrono::milliseconds base_{500};
    std::optional<std::chrono::milliseconds> cap_{std::chrono::milliseconds(30000)};
  };

  /**
   * @brief Worker execution context passed to jobs.
   */
  struct WorkerContext
  {
    std::string worker_id = "worker";
    std::uint64_t tick = 0;

    // User can attach a logger via callback (keeps this header-only and dependency-free).
    std::function<void(std::string_view)> log_info;
    std::function<void(std::string_view)> log_warn;
    std::function<void(std::string_view)> log_error;

    void info(std::string_view m) const
    {
      if (log_info)
        log_info(m);
    }

    void warn(std::string_view m) const
    {
      if (log_warn)
        log_warn(m);
    }

    void error(std::string_view m) const
    {
      if (log_error)
        log_error(m);
    }
  };

  /**
   * @brief Job handler signature.
   *
   * Implement your job logic by registering handlers by job type.
   */
  using JobHandler = std::function<JobResult(const JobEnvelope &, WorkerContext &)>;

  /**
   * @brief Queue interface for pulling and pushing jobs.
   *
   * Default implementation provided: InMemoryQueue.
   */
  class JobQueue
  {
  public:
    virtual ~JobQueue() = default;

    /**
     * @brief Push a job ready to execute immediately.
     */
    virtual void push(JobEnvelope env) = 0;

    /**
     * @brief Push a job scheduled for later execution.
     */
    virtual void push_delayed(JobEnvelope env, WorkerClock::time_point run_at) = 0;

    /**
     * @brief Try to pop one ready job.
     * @return JobEnvelope if available, nullopt otherwise
     */
    virtual std::optional<JobEnvelope> try_pop_ready() = 0;

    /**
     * @brief Next time when a delayed job becomes ready (if any).
     */
    virtual std::optional<WorkerClock::time_point> next_ready_time() const = 0;
  };

  /**
   * @brief Minimal in-memory queue with delayed jobs.
   *
   * Deterministic, single-process, single-threaded by default.
   */
  class InMemoryQueue final : public JobQueue
  {
  public:
    void push(JobEnvelope env) override
    {
      ready_.push_back(std::move(env));
    }

    void push_delayed(JobEnvelope env, WorkerClock::time_point run_at) override
    {
      delayed_.push_back(DelayedItem{run_at, std::move(env)});
    }

    std::optional<JobEnvelope> try_pop_ready() override
    {
      promote_ready_from_delayed();

      if (ready_.empty())
        return std::nullopt;

      JobEnvelope env = std::move(ready_.front());
      ready_.erase(ready_.begin());
      return env;
    }

    std::optional<WorkerClock::time_point> next_ready_time() const override
    {
      if (delayed_.empty())
        return std::nullopt;

      WorkerClock::time_point best = delayed_.front().run_at;
      for (const auto &d : delayed_)
      {
        if (d.run_at < best)
          best = d.run_at;
      }
      return best;
    }

  private:
    struct DelayedItem
    {
      WorkerClock::time_point run_at;
      JobEnvelope env;
    };

    void promote_ready_from_delayed()
    {
      const auto now = WorkerClock::now();

      // Move any due delayed jobs into ready queue.
      // Preserve insertion order among due jobs by scanning.
      std::vector<DelayedItem> remain;
      remain.reserve(delayed_.size());

      for (auto &d : delayed_)
      {
        if (d.run_at <= now)
          ready_.push_back(std::move(d.env));
        else
          remain.push_back(std::move(d));
      }

      delayed_ = std::move(remain);
    }

  private:
    std::vector<JobEnvelope> ready_;
    std::vector<DelayedItem> delayed_;
  };

  /**
   * @brief Simple scheduled task (interval based).
   *
   * This is not cron. It is deterministic interval scheduling.
   */
  struct ScheduledTask
  {
    std::string name;
    std::chrono::milliseconds interval{0};
    WorkerClock::time_point next_run{};
    std::function<void(WorkerContext &)> fn;
  };

  /**
   * @brief Background worker application built on top of vix/app.
   *
   * Provides:
   * - job registration
   * - deterministic tick (poll/execute)
   * - retry integration
   * - delayed jobs
   * - interval scheduled tasks
   *
   * Runtime-agnostic:
   * - call run() to run its loop
   * - or call tick_once() yourself in a custom runtime
   */
  class WorkerApp : public vix::app::Application
  {
  public:
    WorkerApp() = default;
    ~WorkerApp() override = default;

    WorkerApp(const WorkerApp &) = delete;
    WorkerApp &operator=(const WorkerApp &) = delete;

    WorkerApp(WorkerApp &&) = delete;
    WorkerApp &operator=(WorkerApp &&) = delete;

    /**
     * @brief Set job queue implementation.
     *
     * If not set, a default InMemoryQueue is used.
     * Caller owns the queue pointer.
     */
    void set_queue(JobQueue *q) noexcept
    {
      queue_ = q;
    }

    /**
     * @brief Set retry policy (optional).
     *
     * If not set, failed jobs are treated as final failures.
     * Caller owns the retry policy pointer.
     */
    void set_retry_policy(const RetryPolicy *p) noexcept
    {
      retry_ = p;
    }

    /**
     * @brief Set worker id for context.
     */
    void set_worker_id(std::string id)
    {
      ctx_.worker_id = std::move(id);
    }

    /**
     * @brief Set optional logging callbacks.
     */
    void set_logger(std::function<void(std::string_view)> info,
                    std::function<void(std::string_view)> warn = {},
                    std::function<void(std::string_view)> error = {})
    {
      ctx_.log_info = std::move(info);
      ctx_.log_warn = std::move(warn);
      ctx_.log_error = std::move(error);
    }

    /**
     * @brief Configure idle sleep used when no work was done.
     *
     * Default: 10ms.
     */
    void set_idle_sleep(std::chrono::milliseconds d) noexcept
    {
      if (d.count() < 0)
        d = std::chrono::milliseconds(0);
      idle_sleep_ = d;
    }

    /**
     * @brief Register a handler for a given job type.
     */
    void on(std::string type, JobHandler h)
    {
      if (type.empty())
        throw std::runtime_error("WorkerApp::on: job type must not be empty");
      if (!h)
        throw std::runtime_error("WorkerApp::on: handler must be set");

      handlers_.push_back(HandlerEntry{std::move(type), std::move(h)});
    }

    /**
     * @brief Enqueue a job for immediate execution.
     */
    void enqueue(JobEnvelope env)
    {
      ensure_queue();
      queue_->push(std::move(env));
    }

    /**
     * @brief Enqueue a job for delayed execution.
     */
    void enqueue_in(JobEnvelope env, std::chrono::milliseconds delay)
    {
      ensure_queue();
      if (delay.count() < 0)
        delay = std::chrono::milliseconds(0);

      queue_->push_delayed(std::move(env), WorkerClock::now() + delay);
    }

    /**
     * @brief Register an interval scheduled task.
     *
     * This is deterministic interval scheduling (not cron).
     */
    void schedule_every(std::string name,
                        std::chrono::milliseconds interval,
                        std::function<void(WorkerContext &)> fn)
    {
      if (name.empty())
        throw std::runtime_error("WorkerApp::schedule_every: name must not be empty");
      if (interval.count() <= 0)
        throw std::runtime_error("WorkerApp::schedule_every: interval must be > 0ms");
      if (!fn)
        throw std::runtime_error("WorkerApp::schedule_every: fn must be set");

      ScheduledTask t;
      t.name = std::move(name);
      t.interval = interval;
      t.next_run = WorkerClock::now() + interval;
      t.fn = std::move(fn);
      tasks_.push_back(std::move(t));
    }

    /**
     * @brief Execute one deterministic worker tick.
     *
     * It:
     * - runs due scheduled tasks
     * - pops at most one ready job and executes it
     *
     * @return true if some work was done, false otherwise
     */
    bool tick_once()
    {
      ensure_queue();

      ctx_.tick++;

      bool did_work = false;

      did_work |= run_due_tasks();

      auto env_opt = queue_->try_pop_ready();
      if (!env_opt)
        return did_work;

      execute_job(std::move(*env_opt));
      return true;
    }

  protected:
    /**
     * @brief Override Application::run_loop().
     *
     * Application::run() calls this.
     * Loop runs while is_running() is true (stop() sets it to false).
     */
    void run_loop() override
    {
      while (is_running())
      {
        const bool did = tick_once();
        if (!did)
          std::this_thread::sleep_for(idle_sleep_);
      }
    }

  private:
    struct HandlerEntry
    {
      std::string type;
      JobHandler fn;
    };

    void ensure_queue()
    {
      if (queue_)
        return;
      queue_ = &default_queue_;
    }

    JobHandler *find_handler(std::string_view type)
    {
      for (auto &h : handlers_)
      {
        if (h.type == type)
          return &h.fn;
      }
      return nullptr;
    }

    bool run_due_tasks()
    {
      bool did = false;
      const auto now = WorkerClock::now();

      for (auto &t : tasks_)
      {
        if (t.interval.count() <= 0)
          continue;

        if (now >= t.next_run)
        {
          did = true;

          ctx_.info(std::string("task: ") + t.name);

          try
          {
            t.fn(ctx_);
          }
          catch (const std::exception &e)
          {
            ctx_.warn(std::string("task failed: ") + t.name + " err=" + e.what());
          }
          catch (...)
          {
            ctx_.warn(std::string("task failed: ") + t.name + " err=unknown");
          }

          // Advance next_run by multiples of interval to avoid drift explosion.
          auto next = t.next_run;
          while (next <= now)
            next += t.interval;
          t.next_run = next;
        }
      }

      return did;
    }

    void execute_job(JobEnvelope env)
    {
      JobHandler *h = find_handler(env.type);
      if (!h || !(*h))
      {
        ctx_.error(std::string("no handler for job type: ") + env.type);
        return;
      }

      try
      {
        JobResult r = (*h)(env, ctx_);
        if (r == JobResult::Success)
          return;

        if (r == JobResult::Fail)
        {
          ctx_.warn(std::string("job failed: type=") + env.type);
          return;
        }

        // Retry requested
        retry_or_fail(std::move(env), "retry requested");
      }
      catch (const std::exception &e)
      {
        retry_or_fail(std::move(env), e.what());
      }
      catch (...)
      {
        retry_or_fail(std::move(env), "unknown error");
      }
    }

    void retry_or_fail(JobEnvelope env, std::string_view err)
    {
      if (!retry_)
      {
        ctx_.warn(std::string("job failed (no retry policy): type=") + env.type +
                  " err=" + std::string(err));
        return;
      }

      const auto delay = retry_->next_delay(env, err);
      if (!delay)
      {
        ctx_.warn(std::string("job failed (retries exhausted): type=") + env.type +
                  " err=" + std::string(err));
        return;
      }

      env.attempt += 1;

      ctx_.info(std::string("job retry: type=") + env.type +
                " attempt=" + std::to_string(env.attempt) +
                " in_ms=" + std::to_string(delay->count()));

      enqueue_in(std::move(env), *delay);
    }

  private:
    InMemoryQueue default_queue_{};
    JobQueue *queue_ = nullptr;

    const RetryPolicy *retry_ = nullptr;

    WorkerContext ctx_{};
    std::vector<HandlerEntry> handlers_{};
    std::vector<ScheduledTask> tasks_{};

    std::chrono::milliseconds idle_sleep_{10};
  };

} // namespace vix::worker_app

#endif // VIX_WORKER_APP_WORKER_APP_HPP

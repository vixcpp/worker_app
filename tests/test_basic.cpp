#include <worker_app/worker_app.hpp>

#include <cassert>
#include <iostream>

using namespace vix::worker_app;

void test_basic_job()
{
  WorkerApp app;

  bool called = false;

  app.on("hello", [&](const JobEnvelope &env, WorkerContext &)
         {
    called = true;
    assert(env.type == "hello");
    return JobResult::Success; });

  JobEnvelope env;
  env.type = "hello";
  env.payload = "{}";

  app.enqueue(env);

  bool did = app.tick_once();

  assert(did);
  assert(called);

  std::cout << "basic job ok\n";
}

void test_retry_policy()
{
  WorkerApp app;

  FixedRetryPolicy retry(2, std::chrono::milliseconds(0));
  app.set_retry_policy(&retry);

  int attempts = 0;

  app.on("retry_job", [&](const JobEnvelope &, WorkerContext &)
         {
    attempts++;
    return JobResult::Retry; });

  JobEnvelope env;
  env.type = "retry_job";

  app.enqueue(env);

  app.tick_once(); // first attempt
  app.tick_once(); // retry attempt

  assert(attempts >= 2);

  std::cout << "retry policy ok\n";
}

void test_scheduled_task()
{
  WorkerApp app;

  int runs = 0;

  app.schedule_every(
      "test_task",
      std::chrono::milliseconds(1),
      [&](WorkerContext &)
      {
        runs++;
      });

  app.tick_once();

  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  app.tick_once();

  assert(runs > 0);

  std::cout << "scheduled task ok\n";
}

int main()
{
  test_basic_job();
  test_retry_policy();
  test_scheduled_task();

  std::cout << "worker_app basic tests passed\n";
}

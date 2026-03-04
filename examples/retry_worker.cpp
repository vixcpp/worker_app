#include <worker_app/worker_app.hpp>

#include <iostream>

using namespace vix::worker_app;

int main()
{
  WorkerApp app;

  FixedRetryPolicy retry(3, std::chrono::milliseconds(100));

  app.set_retry_policy(&retry);

  int attempts = 0;

  app.on("unstable_job", [&](const JobEnvelope &, WorkerContext &)
         {
    attempts++;

    std::cout << "attempt: " << attempts << "\n";

    if (attempts < 3)
      return JobResult::Retry;

    return JobResult::Success; });

  JobEnvelope job;
  job.type = "unstable_job";

  app.enqueue(job);

  while (attempts < 3)
  {
    app.tick_once();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }

  return 0;
}

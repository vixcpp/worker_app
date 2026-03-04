#include <worker_app/worker_app.hpp>

#include <iostream>

using namespace vix::worker_app;

int main()
{
  WorkerApp app;

  app.set_logger(
      [](std::string_view m)
      { std::cout << "[info] " << m << "\n"; },
      [](std::string_view m)
      { std::cout << "[warn] " << m << "\n"; },
      [](std::string_view m)
      { std::cout << "[error] " << m << "\n"; });

  app.on("hello", [](const JobEnvelope &job, WorkerContext &)
         {
    std::cout << "job received: " << job.type << "\n";
    return JobResult::Success; });

  JobEnvelope job;
  job.type = "hello";
  job.payload = "{\"name\":\"vix\"}";

  app.enqueue(job);

  app.tick_once();

  return 0;
}

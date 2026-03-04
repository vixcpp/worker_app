#include <worker_app/worker_app.hpp>

#include <iostream>

using namespace vix::worker_app;

int main()
{
  WorkerApp app;

  app.set_logger(
      [](std::string_view m)
      { std::cout << "[info] " << m << "\n"; });

  app.schedule_every(
      "heartbeat",
      std::chrono::milliseconds(500),
      [](WorkerContext &ctx)
      {
        std::cout << "tick: " << ctx.tick << "\n";
      });

  app.run();

  return 0;
}

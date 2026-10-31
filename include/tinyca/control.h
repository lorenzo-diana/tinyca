// BSD 3-Clause License — see LICENSE at the repository root.
#pragma once

#include <atomic>
#include <thread>

namespace tinyca
{

class ITransport;
class Executor;

// Always-interruptible control thread (SPEC.md §9.1). start() blocks
// SIGINT/SIGTERM in the calling thread *before* spawning anything else, so
// every subsequently-created thread inherits the same mask (POSIX thread
// creation semantics) and only this class's own thread -- parked in
// sigwait() -- ever actually receives them. On receipt it cancels whichever
// of transport/executor is currently in flight and sets stop_requested().
//
// Call start() as the very first thing in main(), before constructing the
// worker thread that runs the agent loop.
class ControlThread
{
  public:
    ControlThread(ITransport *transport, Executor *executor);
    ~ControlThread();

    void start();
    void stop(); // clean shutdown; joins

    bool stop_requested() const;
    void reset(); // clears stop_requested() -- interactive mode, next task

  private:
    void run();

    ITransport *transport_;
    Executor *executor_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> shutting_down_{false};
    std::thread thread_;
};

} // namespace tinyca

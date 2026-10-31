// BSD 3-Clause License — see LICENSE at the repository root.
#include "tinyca/control.h"

#include <pthread.h>
#include <signal.h>

#include "tinyca/executor.h"
#include "tinyca/transport.h"

namespace tinyca
{

namespace
{
sigset_t control_signal_set()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    return set;
}
} // namespace

ControlThread::ControlThread(ITransport *transport, Executor *executor) : transport_(transport), executor_(executor)
{
}

ControlThread::~ControlThread()
{
    stop();
}

void ControlThread::start()
{
    sigset_t set = control_signal_set();
    // Must happen before the thread below (or any other thread) is created:
    // a new thread inherits the calling thread's signal mask at creation
    // time, so every thread in the process ends up blocking these signals,
    // leaving only this class's own sigwait() able to receive them.
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    thread_ = std::thread([this]() { run(); });
}

void ControlThread::run()
{
    sigset_t set = control_signal_set();
    while (!shutting_down_)
    {
        int sig = 0;
        if (sigwait(&set, &sig) != 0)
            continue;
        if (shutting_down_)
            break; // woken by stop()'s own SIGTERM, not a user interrupt
        stop_requested_ = true;
        if (executor_ != nullptr)
            executor_->interrupt();
        if (transport_ != nullptr)
            transport_->cancel();
    }
}

void ControlThread::stop()
{
    if (shutting_down_.exchange(true))
        return; // already stopped/stopping
    if (thread_.joinable())
    {
        pthread_kill(thread_.native_handle(), SIGTERM);
        thread_.join();
    }
}

bool ControlThread::stop_requested() const
{
    return stop_requested_;
}

void ControlThread::reset()
{
    stop_requested_ = false;
}

} // namespace tinyca

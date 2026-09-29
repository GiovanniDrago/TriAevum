#include "oot3d_native_render_command_stream.h"

#include <stdexcept>
#include <utility>

namespace Oot3dNativeGame {
namespace {

thread_local bool tInCommand = false;

struct CommandScope {
    CommandScope() { tInCommand = true; }
    ~CommandScope() { tInCommand = false; }
};

} // namespace

NativeRenderCommandStream::NativeRenderCommandStream(Mode mode,
                                                     size_t maxFramesInFlight)
    : mMode(mode),
      mMaxFramesInFlight(maxFramesInFlight == 0U ? 1U : maxFramesInFlight) {}

NativeRenderCommandStream::~NativeRenderCommandStream() {
    StopWorker();
}

void NativeRenderCommandStream::SetMode(Mode mode) {
    Drain();
    if (mode == mMode) return;
    if (mode == Mode::Inline) StopWorker();
    mMode = mode;
}

bool NativeRenderCommandStream::InCommand() const noexcept {
    return tInCommand;
}

uint64_t NativeRenderCommandStream::ExecutedCommands() const noexcept {
    std::lock_guard lock(mMutex);
    return mExecuted;
}

void NativeRenderCommandStream::Submit(Command command, bool endsFrame) {
    if (mMode == Mode::Inline) {
        {
            CommandScope scope;
            command();
        }
        std::lock_guard lock(mMutex);
        ++mSubmitted;
        ++mExecuted;
        return;
    }
    StartWorker();
    std::unique_lock lock(mMutex);
    RethrowPendingFailureLocked(lock);
    if (endsFrame) {
        mProgress.wait(lock, [&] {
            return mFailure != nullptr || mFramesQueued < mMaxFramesInFlight;
        });
        RethrowPendingFailureLocked(lock);
        ++mFramesQueued;
    }
    mQueue.push_back({std::move(command), endsFrame});
    ++mSubmitted;
    mWork.notify_one();
}

void NativeRenderCommandStream::Run(Command command) {
    if (mMode == Mode::Inline) {
        Submit(std::move(command));
        return;
    }
    Submit(std::move(command));
    std::unique_lock lock(mMutex);
    const uint64_t ticket = mSubmitted;
    mProgress.wait(lock, [&] {
        return mFailure != nullptr || mExecuted >= ticket;
    });
    RethrowPendingFailureLocked(lock);
}

void NativeRenderCommandStream::Drain() {
    if (mMode == Mode::Inline || !mWorker.joinable()) return;
    std::unique_lock lock(mMutex);
    mProgress.wait(lock, [&] {
        return mFailure != nullptr || (mQueue.empty() && !mBusy);
    });
    RethrowPendingFailureLocked(lock);
}

void NativeRenderCommandStream::RethrowPendingFailureLocked(
    std::unique_lock<std::mutex>& lock) {
    if (mFailure == nullptr) return;
    // Report the failure once; the stream is empty and usable afterwards.
    auto failure = std::exchange(mFailure, nullptr);
    mQueue.clear();
    mFramesQueued = 0U;
    mExecuted = mSubmitted;
    lock.unlock();
    std::rethrow_exception(failure);
}

void NativeRenderCommandStream::StartWorker() {
    if (mWorker.joinable()) return;
    mStop = false;
    mWorker = std::thread([this] { WorkerLoop(); });
}

void NativeRenderCommandStream::StopWorker() {
    if (!mWorker.joinable()) return;
    {
        std::lock_guard lock(mMutex);
        mStop = true;
    }
    mWork.notify_all();
    mWorker.join();
}

void NativeRenderCommandStream::WorkerLoop() {
    std::unique_lock lock(mMutex);
    for (;;) {
        mWork.wait(lock, [&] { return mStop || !mQueue.empty(); });
        if (mQueue.empty()) return;  // stopping with nothing left to run
        Entry entry = std::move(mQueue.front());
        mQueue.pop_front();
        mBusy = true;
        lock.unlock();
        std::exception_ptr failure;
        try {
            CommandScope scope;
            entry.Work();
        } catch (...) {
            failure = std::current_exception();
        }
        lock.lock();
        mBusy = false;
        if (entry.EndsFrame && mFramesQueued != 0U) --mFramesQueued;
        if (failure != nullptr) {
            mFailure = failure;
            mQueue.clear();
            mFramesQueued = 0U;
        } else {
            ++mExecuted;
        }
        mProgress.notify_all();
    }
}

} // namespace Oot3dNativeGame

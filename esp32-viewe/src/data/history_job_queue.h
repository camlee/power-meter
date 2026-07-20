#pragma once

#include <cstddef>
#include <cstdint>

namespace history_job_queue {

enum class State : uint8_t {
    Unknown,
    Queued,
    Running,
    Ready,
    Gone,
};

// A fixed-size, allocation-free FIFO for the history worker. The queue owns
// only cheap job descriptors; the service continues to own one set of result
// buffers. A completed job must be consumed or expired before the next job can
// start, so result buffers are never overwritten while a client can read them.
template <typename Job, size_t QueueCapacity, size_t FinishedCapacity>
class Queue {
public:
    static_assert(QueueCapacity > 0, "history job queue must have at least one slot");
    static_assert(FinishedCapacity > 0, "history job queue must remember finished jobs");

    uint32_t enqueue(Job job) {
        if (waitingCount_ == QueueCapacity) return 0;
        job.id = nextId();
        waiting_[waitingTail_] = job;
        waitingTail_ = (waitingTail_ + 1) % QueueCapacity;
        ++waitingCount_;
        return job.id;
    }

    bool beginNext(Job& job) {
        if (running_.id || ready_.id || waitingCount_ == 0) return false;
        running_ = waiting_[waitingHead_];
        waitingHead_ = (waitingHead_ + 1) % QueueCapacity;
        --waitingCount_;
        job = running_;
        return true;
    }

    bool complete(uint32_t jobId, uint32_t nowMs) {
        if (!jobId || running_.id != jobId || ready_.id) return false;
        if (runningCancelled_) {
            rememberFinished(jobId);
            running_ = Job{};
            runningCancelled_ = false;
            return false;
        }
        ready_ = running_;
        running_ = Job{};
        readySinceMs_ = nowMs;
        return true;
    }

    bool cancel(uint32_t jobId) {
        if (!jobId) return false;
        if (ready_.id == jobId) return consume(jobId);
        if (running_.id == jobId) {
            runningCancelled_ = true;
            return true;
        }
        for (size_t i = 0; i < waitingCount_; ++i) {
            const size_t index = (waitingHead_ + i) % QueueCapacity;
            if (waiting_[index].id != jobId) continue;
            for (size_t move = i; move + 1 < waitingCount_; ++move) {
                const size_t destination = (waitingHead_ + move) % QueueCapacity;
                const size_t source = (waitingHead_ + move + 1) % QueueCapacity;
                waiting_[destination] = waiting_[source];
            }
            waitingTail_ = (waitingTail_ + QueueCapacity - 1) % QueueCapacity;
            waiting_[waitingTail_] = Job{};
            --waitingCount_;
            rememberFinished(jobId);
            return true;
        }
        return false;
    }

    bool consume(uint32_t jobId) {
        if (!jobId || ready_.id != jobId) return false;
        rememberFinished(jobId);
        ready_ = Job{};
        readySinceMs_ = 0;
        return true;
    }

    bool expireReady(uint32_t nowMs, uint32_t ttlMs) {
        if (!ready_.id || static_cast<uint32_t>(nowMs - readySinceMs_) < ttlMs) return false;
        return consume(ready_.id);
    }

    State state(uint32_t jobId) const {
        if (!jobId) return State::Unknown;
        if (ready_.id == jobId) return State::Ready;
        if (running_.id == jobId) return State::Running;
        for (size_t i = 0, index = waitingHead_; i < waitingCount_; ++i) {
            if (waiting_[index].id == jobId) return State::Queued;
            index = (index + 1) % QueueCapacity;
        }
        for (size_t i = 0; i < finishedCount_; ++i) {
            if (finished_[i] == jobId) return State::Gone;
        }
        return State::Unknown;
    }

    const Job* ready() const { return ready_.id ? &ready_ : nullptr; }
    bool hasOutstanding() const { return waitingCount_ || running_.id || ready_.id; }
    size_t waitingCount() const { return waitingCount_; }
    uint32_t readySinceMs() const { return readySinceMs_; }

private:
    uint32_t nextId() {
        ++nextJobId_;
        if (!nextJobId_) ++nextJobId_;
        return nextJobId_;
    }

    void rememberFinished(uint32_t jobId) {
        finished_[finishedNext_] = jobId;
        finishedNext_ = (finishedNext_ + 1) % FinishedCapacity;
        if (finishedCount_ < FinishedCapacity) ++finishedCount_;
    }

    Job waiting_[QueueCapacity]{};
    uint32_t finished_[FinishedCapacity]{};
    Job running_{};
    Job ready_{};
    size_t waitingHead_ = 0;
    size_t waitingTail_ = 0;
    size_t waitingCount_ = 0;
    size_t finishedNext_ = 0;
    size_t finishedCount_ = 0;
    uint32_t nextJobId_ = 0;
    uint32_t readySinceMs_ = 0;
    bool runningCancelled_ = false;
};

} // namespace history_job_queue

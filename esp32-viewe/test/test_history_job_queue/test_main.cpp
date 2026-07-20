#include <unity.h>

#include "data/history_job_queue.h"

namespace {

struct TestJob {
    uint32_t id = 0;
    uint8_t value = 0;
};

using State = history_job_queue::State;

void testJobsCompleteInFifoOrderWithoutOverwritingReadyResult() {
    history_job_queue::Queue<TestJob, 3, 4> queue;
    const uint32_t first = queue.enqueue({0, 11});
    const uint32_t second = queue.enqueue({0, 22});
    TEST_ASSERT_NOT_EQUAL(0, first);
    TEST_ASSERT_NOT_EQUAL(0, second);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::Queued), static_cast<int>(queue.state(first)));

    TestJob job{};
    TEST_ASSERT_TRUE(queue.beginNext(job));
    TEST_ASSERT_EQUAL_UINT32(first, job.id);
    TEST_ASSERT_EQUAL_UINT8(11, job.value);
    TEST_ASSERT_TRUE(queue.complete(first, 100));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::Ready), static_cast<int>(queue.state(first)));
    TEST_ASSERT_FALSE(queue.beginNext(job));

    TEST_ASSERT_TRUE(queue.consume(first));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::Gone), static_cast<int>(queue.state(first)));
    TEST_ASSERT_TRUE(queue.beginNext(job));
    TEST_ASSERT_EQUAL_UINT32(second, job.id);
    TEST_ASSERT_EQUAL_UINT8(22, job.value);
}

void testReadyResultExpiresAndAllowsNextJobToRun() {
    history_job_queue::Queue<TestJob, 2, 4> queue;
    const uint32_t first = queue.enqueue({0, 1});
    const uint32_t second = queue.enqueue({0, 2});
    TestJob job{};
    TEST_ASSERT_TRUE(queue.beginNext(job));
    TEST_ASSERT_TRUE(queue.complete(first, 0xfffffff0U));
    TEST_ASSERT_FALSE(queue.expireReady(0x00000020U, 49));
    TEST_ASSERT_TRUE(queue.expireReady(0x00000021U, 49));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::Gone), static_cast<int>(queue.state(first)));
    TEST_ASSERT_TRUE(queue.beginNext(job));
    TEST_ASSERT_EQUAL_UINT32(second, job.id);
}

void testCancellationRemovesQueuedJobAndDiscardsRunningResult() {
    history_job_queue::Queue<TestJob, 3, 4> queue;
    const uint32_t running = queue.enqueue({0, 1});
    const uint32_t cancelledQueued = queue.enqueue({0, 2});
    const uint32_t remaining = queue.enqueue({0, 3});
    TestJob job{};
    TEST_ASSERT_TRUE(queue.beginNext(job));
    TEST_ASSERT_TRUE(queue.cancel(cancelledQueued));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::Gone),
                          static_cast<int>(queue.state(cancelledQueued)));
    TEST_ASSERT_TRUE(queue.cancel(running));
    TEST_ASSERT_FALSE(queue.complete(running, 10));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(State::Gone), static_cast<int>(queue.state(running)));
    TEST_ASSERT_TRUE(queue.beginNext(job));
    TEST_ASSERT_EQUAL_UINT32(remaining, job.id);
}

void testQueueCapacityIsBounded() {
    history_job_queue::Queue<TestJob, 2, 4> queue;
    TEST_ASSERT_NOT_EQUAL(0, queue.enqueue({0, 1}));
    TEST_ASSERT_NOT_EQUAL(0, queue.enqueue({0, 2}));
    TEST_ASSERT_EQUAL_UINT32(0, queue.enqueue({0, 3}));
    TEST_ASSERT_EQUAL_UINT32(2, queue.waitingCount());
}

} // namespace

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(testJobsCompleteInFifoOrderWithoutOverwritingReadyResult);
    RUN_TEST(testReadyResultExpiresAndAllowsNextJobToRun);
    RUN_TEST(testCancellationRemovesQueuedJobAndDiscardsRunningResult);
    RUN_TEST(testQueueCapacityIsBounded);
    return UNITY_END();
}

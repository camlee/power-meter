#include <unity.h>

#include "ui/home_axis.h"

void setUp() {}
void tearDown() {}

void testSmallScalesIncludeZero() {
    const auto positive = home_axis::scale(1.2f, 4.1f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, positive.minimum);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, positive.maximum);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, positive.step);

    const auto negative = home_axis::scale(-4.1f, -1.2f);
    TEST_ASSERT_EQUAL_FLOAT(-5.0f, negative.minimum);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, negative.maximum);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, negative.step);
}

void testLargeScalesUseNiceSteps() {
    const auto medium = home_axis::scale(0.0f, 20.0f);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, medium.step);

    const auto large = home_axis::scale(-83.0f, 12.0f);
    TEST_ASSERT_EQUAL_FLOAT(-100.0f, large.minimum);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, large.maximum);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, large.step);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(testSmallScalesIncludeZero);
    RUN_TEST(testLargeScalesUseNiceSteps);
    return UNITY_END();
}

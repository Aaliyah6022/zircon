// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fbl/algorithm.h>
#include <fbl/unique_ptr.h>
#include <perftest/results.h>
#include <unittest/unittest.h>

// Add terminator to a string buffer registered with fmemopen().
static bool FixUpFileBuffer(FILE* fp, char* buf, size_t buf_size) {
  ASSERT_FALSE(ferror(fp));
  size_t data_size = ftell(fp);
  ASSERT_LT(data_size, buf_size);
  buf[data_size] = '\0';
  ASSERT_EQ(fclose(fp), 0);
  return true;
}

static bool TestJsonOutput() {
  BEGIN_TEST;

  perftest::ResultsSet results;
  perftest::TestCaseResults* test_case =
      results.AddTestCase("results_test", "ExampleNullSyscall", "nanoseconds");
  // Fill out some example data.
  for (int val = 101; val <= 105; ++val) {
    test_case->AppendValue(val);
  }

  // Write the JSON output to a buffer in memory.
  char buf[1000];
  FILE* fp = fmemopen(buf, sizeof(buf), "w+");
  ASSERT_NONNULL(fp);
  results.WriteJSON(fp);
  ASSERT_TRUE(FixUpFileBuffer(fp, buf, sizeof(buf)));

  // Test the JSON output.
  const char* expected =
      R"JSON([{"label":"ExampleNullSyscall","test_suite":"results_test","unit":"nanoseconds","values":[101.000000,102.000000,103.000000,104.000000,105.000000]}])JSON";
  EXPECT_STR_EQ(expected, buf, "");

  END_TEST;
}

static bool TestSummaryStatistics() {
  BEGIN_TEST;

  perftest::ResultsSet results;
  perftest::TestCaseResults* test_case =
      results.AddTestCase("results_test", "ExampleNullSyscall", "nanoseconds");
  // Fill out some example data in a non-sorted order.
  test_case->AppendValue(200);
  test_case->AppendValue(6);
  test_case->AppendValue(100);
  test_case->AppendValue(110);

  perftest::SummaryStatistics stats = test_case->GetSummaryStatistics();
  EXPECT_EQ(stats.min, 6);
  EXPECT_EQ(stats.max, 200);
  EXPECT_EQ(stats.mean, 104);
  EXPECT_EQ(static_cast<int>(stats.std_dev), 68);
  // There is an even number of values, so the median is interpolated.
  EXPECT_EQ(stats.median, (100 + 110) / 2);

  test_case->AppendValue(300);
  stats = test_case->GetSummaryStatistics();
  // There is an odd number of values, so the median is not interpolated.
  EXPECT_EQ(stats.median, 110);

  END_TEST;
}

// Test escaping special characters in strings in JSON output.
static bool TestJsonStringEscaping() {
  BEGIN_TEST;

  char buf[1000];
  FILE* fp = fmemopen(buf, sizeof(buf), "w+");
  ASSERT_NONNULL(fp);
  perftest::WriteJSONString(fp, "foo \"bar\" \\ \n \xff");
  ASSERT_TRUE(FixUpFileBuffer(fp, buf, sizeof(buf)));

  const char* expected = "\"foo \\\"bar\\\" \\\\ \\u000a \\u00ff\"";
  EXPECT_STR_EQ(expected, buf, "");

  END_TEST;
}

BEGIN_TEST_CASE(perf_results_output_tests)
RUN_TEST(TestJsonOutput)
RUN_TEST(TestSummaryStatistics)
RUN_TEST(TestJsonStringEscaping)
END_TEST_CASE(perf_results_output_tests)

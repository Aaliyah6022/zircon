// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unittest/unittest.h>
//#include <log_test.h>

int main(int argc, char** argv) {
    return !unittest_run_all_tests(argc, argv);
}

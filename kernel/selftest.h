#pragma once

/* Runs kernel self-checks and reports SELFTEST PASS/FAIL to console+serial.
 * Returns 1 on success, 0 on failure. This is the automated test oracle
 * tools/run_tests.sh looks for. */
int selftest_run(void);

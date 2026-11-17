/*
 * ----------------------------------------------------------------------
 *
 * File: example.h
 *
 * Copyright (C) 2025 ETH Zurich and University of Bologna
 *
 * Based on the semester project of Ahmed Malanovic (ETH Zurich, 2025).
 *
 * ----------------------------------------------------------------------
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CRYPTO_API_EXAMPLE_H_
#define CRYPTO_API_EXAMPLE_H_

/** Run all tests, returns the number of failed checks. */
int run_tests(void);

/** Run the benchmarks and print the results. */
void run_benchmarks(void);

#endif /* CRYPTO_API_EXAMPLE_H_ */

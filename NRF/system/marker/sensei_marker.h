/*
 * SPDX-FileCopyrightText: 2026 ETH Zurich and University of Bologna
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:
 * - Philip Wiese <wiesep@iis.ee.ethz.ch>
 */

/**
 * @file sensei_marker.h
 * @brief Energy markers: two GPIO outputs for the digital inputs of a power analyzer (for example the PPK2).
 *
 * The pins are the devicetree aliases marker0 and marker1. A pin is high while at least one of its sources
 * is on, so overlapping jobs give one marker. The application defines what a source means.
 */

#ifndef SENSEI_MARKER_H_
#define SENSEI_MARKER_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSEI_MARKER_PINS 2
#define SENSEI_MARKER_SOURCES 32

/** Configure both pins as outputs, low. Pins that are not ready are skipped. */
void sensei_marker_init(void);

/** Turn a source of a pin on or off (pin 0 or 1, source 0 ... 31). The pin follows "any source on". */
void sensei_marker_set(unsigned pin, unsigned source, bool on);

#ifdef __cplusplus
}
#endif

#endif /* SENSEI_MARKER_H_ */

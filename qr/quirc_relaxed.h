/*
 * quirc_relaxed.h - a second build of quirc's identify.c with a looser
 * finder-pattern grouping test, linked next to the stock one.
 *
 * quirc groups three finder patterns into a code only when the distances
 * to the two neighbours, measured in the first pattern's own perspective,
 * agree within 20%. That perspective comes from the corners of one 7x7
 * module pattern, so a one-pixel corner error is magnified about eight
 * times at the far patterns, and on dense codes (version 10 and up) the
 * test rejects most real codes. The relaxed build accepts up to 60%; a
 * wrong grouping is still rejected later by the format and data error
 * correction. See README.md, host simulation.
 *
 * The build generates identify_relaxed.c from the vendored identify.c by
 * replacing the one constant and renaming the three public functions;
 * the vendored source stays unmodified.
 */
#ifndef QUIRC_RELAXED_H
#define QUIRC_RELAXED_H

#include "quirc.h"

uint8_t *quirc_begin_relaxed(struct quirc *q, int *w, int *h);
void quirc_end_relaxed(struct quirc *q);
void quirc_extract_relaxed(const struct quirc *q, int index, struct quirc_code *code);

#endif

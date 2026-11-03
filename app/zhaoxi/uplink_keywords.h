/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>
/* Personal, finite two-phrase recognizer, NOT general Chinese ASR. All structs
 * below are sizeable: allocate on the heap, not a small embedded task stack.
 * Real held-out recordings and unrelated negative phrases are mandatory. */
#define UPLINK_KW_RATE 16000u
#define UPLINK_KW_MAX_SAMPLES 64000u
#define UPLINK_KW_MAX_FRAMES 400u
#define UPLINK_KW_COEFFS 13u
#define UPLINK_KW_PER_CLASS 3u
enum uplink_kw_label {UPLINK_KW_UNKNOWN=0,UPLINK_KW_WIRED=1,UPLINK_KW_WIFI=2};
struct uplink_kw_features {
    uint32_t frames;
    float values[UPLINK_KW_MAX_FRAMES][UPLINK_KW_COEFFS];
};
struct uplink_kw_model {
    uint32_t count[2],calibrated,validated_mask;
    float radius[2],minimum_margin;
    struct uplink_kw_features templates[2][UPLINK_KW_PER_CLASS];
};
struct uplink_kw_result {
    int label; /* 0 means rejected/unknown, never a forced choice. */
    float scores[2],margin;
};
int uplink_kw_extract(const int16_t *pcm,size_t samples,struct uplink_kw_features *out);
/* Symmetric, prefix-weighted, constrained DTW distance; negative on bad input. */
float uplink_kw_distance(const struct uplink_kw_features *a,const struct uplink_kw_features *b);
int uplink_kw_add(struct uplink_kw_model *model,int label,const struct uplink_kw_features *features);
/* Requires three examples/class and refuses overlapping template classes. */
int uplink_kw_calibrate(struct uplink_kw_model *model);
/* Evaluate only for enrollment/diagnostics; does not require validation yet. */
int uplink_kw_evaluate(const struct uplink_kw_model *model,
    const struct uplink_kw_features *features,struct uplink_kw_result *out);
/* expected=1/2: independent held-out phrase; expected=0: unrelated negative.
 * A failed check clears readiness. Does not add the held-out sample to training.
 * At least one success of each kind (mask=7) is required for runtime classify. */
int uplink_kw_validate(struct uplink_kw_model *model,int expected,
    const struct uplink_kw_features *features,struct uplink_kw_result *out);
int uplink_kw_ready(const struct uplink_kw_model *model);
int uplink_kw_classify(const struct uplink_kw_model *model,
    const struct uplink_kw_features *features,struct uplink_kw_result *out);

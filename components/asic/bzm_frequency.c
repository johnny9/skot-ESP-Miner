#include "bzm_frequency.h"

#include <math.h>
#include <stddef.h>

enum
{
    BZM_REFERENCE_MHZ = 50,
    BZM_REFERENCE_DIVIDER = 2,
    BZM_POST1_DIVIDER = 1,
    BZM_POST2_DIVIDER = 1,
};

static const float BZM_FREQUENCY_EPSILON_MHZ = 0.001f;

static float clamp_frequency(float frequency_mhz, float minimum_mhz,
                             float maximum_mhz)
{
    if (frequency_mhz < minimum_mhz) return minimum_mhz;
    if (frequency_mhz > maximum_mhz) return maximum_mhz;
    return frequency_mhz;
}

bool bzm_frequency_request_is_valid(float requested_mhz)
{
    return isfinite(requested_mhz) &&
           requested_mhz >= BZM_FREQUENCY_TARGET_MIN_MHZ &&
           requested_mhz <= BZM_FREQUENCY_TARGET_MAX_MHZ;
}

bool bzm_frequency_resolve_target(float requested_mhz,
                                  bzm_frequency_target_t *target)
{
    if (!isfinite(requested_mhz) || target == NULL) return false;

    float clamped_mhz =
        clamp_frequency(requested_mhz, BZM_FREQUENCY_TARGET_MIN_MHZ,
                        BZM_FREQUENCY_TARGET_MAX_MHZ);
    float feedback =
        (float)BZM_REFERENCE_DIVIDER * (BZM_POST1_DIVIDER + 1) *
        (BZM_POST2_DIVIDER + 1) * clamped_mhz / BZM_REFERENCE_MHZ;

    /* Round normally while keeping exact half-divider ties at the lower value. */
    uint16_t feedback_divider = (uint16_t)feedback;
    if (feedback - (float)feedback_divider > 0.5f) ++feedback_divider;

    *target = (bzm_frequency_target_t){
        .requested_mhz = requested_mhz,
        .actual_mhz =
            (float)BZM_REFERENCE_MHZ * feedback_divider /
            ((float)BZM_REFERENCE_DIVIDER * (BZM_POST1_DIVIDER + 1) *
             (BZM_POST2_DIVIDER + 1)),
        .feedback_divider = feedback_divider,
        .postdiv_register =
            (1U << 12) | (BZM_POST2_DIVIDER << 9) |
            (BZM_POST1_DIVIDER << 6) | BZM_REFERENCE_DIVIDER,
    };
    return true;
}

float bzm_frequency_initial_mhz(float target_mhz)
{
    bzm_frequency_target_t resolved;
    if (!bzm_frequency_resolve_target(target_mhz, &resolved)) return NAN;

    float initial_mhz =
        clamp_frequency(resolved.actual_mhz - BZM_FREQUENCY_INITIAL_OFFSET_MHZ,
                        BZM_FREQUENCY_TARGET_MIN_MHZ,
                        BZM_FREQUENCY_INITIAL_MAX_MHZ);
    if (!bzm_frequency_resolve_target(initial_mhz, &resolved)) return NAN;
    return resolved.actual_mhz;
}

bool bzm_frequency_next_ramp_mhz(float current_mhz, float target_mhz,
                                 float *next_mhz)
{
    bzm_frequency_target_t resolved_target;
    if (next_mhz == NULL || !isfinite(current_mhz) ||
        !bzm_frequency_request_is_valid(target_mhz) ||
        !bzm_frequency_resolve_target(target_mhz, &resolved_target) ||
        current_mhz <
            BZM_FREQUENCY_POWER_ON_MHZ - BZM_FREQUENCY_EPSILON_MHZ) {
        return false;
    }

    target_mhz = resolved_target.actual_mhz;
    if (current_mhz >= target_mhz - BZM_FREQUENCY_EPSILON_MHZ) return false;

    float candidate_mhz;
    if (fabsf(current_mhz - BZM_FREQUENCY_POWER_ON_MHZ) <=
        BZM_FREQUENCY_EPSILON_MHZ) {
        candidate_mhz = bzm_frequency_initial_mhz(target_mhz);
        if (!isfinite(candidate_mhz) ||
            candidate_mhz <= current_mhz + BZM_FREQUENCY_EPSILON_MHZ) {
            candidate_mhz = current_mhz + BZM_FREQUENCY_RAMP_STEP_MHZ;
        }
    } else {
        candidate_mhz = current_mhz + BZM_FREQUENCY_RAMP_STEP_MHZ;
    }

    if (candidate_mhz > target_mhz) candidate_mhz = target_mhz;

    bzm_frequency_target_t resolved;
    if (!bzm_frequency_resolve_target(candidate_mhz, &resolved) ||
        resolved.actual_mhz <= current_mhz + BZM_FREQUENCY_EPSILON_MHZ) {
        return false;
    }
    *next_mhz = resolved.actual_mhz;
    return true;
}

bool bzm_frequency_next_live_ramp_mhz(float current_mhz, float target_mhz,
                                      float *next_mhz)
{
    bzm_frequency_target_t resolved_current;
    bzm_frequency_target_t resolved_target;
    if (next_mhz == NULL ||
        !bzm_frequency_request_is_valid(current_mhz) ||
        !bzm_frequency_request_is_valid(target_mhz) ||
        !bzm_frequency_resolve_target(current_mhz, &resolved_current) ||
        !bzm_frequency_resolve_target(target_mhz, &resolved_target) ||
        fabsf(resolved_current.actual_mhz - current_mhz) >=
            BZM_FREQUENCY_EPSILON_MHZ) {
        return false;
    }

    current_mhz = resolved_current.actual_mhz;
    target_mhz = resolved_target.actual_mhz;
    float delta_mhz = target_mhz - current_mhz;
    if (fabsf(delta_mhz) < BZM_FREQUENCY_EPSILON_MHZ) return false;

    float candidate_mhz =
        current_mhz +
        (delta_mhz > 0.0f ? BZM_FREQUENCY_RAMP_STEP_MHZ
                          : -BZM_FREQUENCY_RAMP_STEP_MHZ);
    if ((delta_mhz > 0.0f && candidate_mhz > target_mhz) ||
        (delta_mhz < 0.0f && candidate_mhz < target_mhz)) {
        candidate_mhz = target_mhz;
    }

    bzm_frequency_target_t resolved_next;
    if (!bzm_frequency_resolve_target(candidate_mhz, &resolved_next) ||
        fabsf(resolved_next.actual_mhz - current_mhz) <
            BZM_FREQUENCY_EPSILON_MHZ) {
        return false;
    }
    *next_mhz = resolved_next.actual_mhz;
    return true;
}

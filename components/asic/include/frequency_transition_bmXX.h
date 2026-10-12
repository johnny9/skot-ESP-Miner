#ifndef FREQUENCY_TRANSITION_H
#define FREQUENCY_TRANSITION_H

#include <stdbool.h>

#include "asic_context.h"

extern const char *FREQUENCY_TRANSITION_TAG;

/**
 * @brief Function pointer type for ASIC hash frequency setting functions
 * 
 * This type defines the signature for functions that set the hash frequency
 * for different ASIC types.
 * 
 * @param frequency The frequency to set in MHz
 */
typedef float (*set_hash_frequency_fn)(float frequency);

/**
 * @brief Transition the ASIC frequency to a target value
 * 
 * This function gradually adjusts the ASIC frequency to reach the target value,
 * stepping up or down in increments to ensure stability.
 * 
 * @param state Pointer to the asic_context_t structure
 * @param set_frequency_fn Function pointer to the appropriate ASIC's set_hash_frequency function
 */
void do_frequency_transition(asic_context_t * state, set_hash_frequency_fn set_frequency_fn);

#endif // FREQUENCY_TRANSITION_H

#ifndef ASIC_CONTEXT_INTERNAL_H
#define ASIC_CONTEXT_INTERNAL_H
#include "asic_context.h"
#include "asic_platform.h"
bool asic_context_claim(asic_context_t *context);
bool asic_context_is_active(const asic_context_t *context);
bool asic_context_has_owner(void);
const asic_platform_ops_t *asic_platform_default(void);
#endif

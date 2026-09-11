#ifndef MINING_ALLOCATOR_FIXTURE_H
#define MINING_ALLOCATOR_FIXTURE_H

#include <stddef.h>

/* Fail one allocation in the selected mining translation units. Zero disables
 * failure injection. Pool storage, libc strdup and ESP services are unaffected. */
void mining_allocator_fixture_reset(size_t failure_at);
void *mining_allocator_fixture_malloc(size_t size);
size_t mining_allocator_fixture_calls(void);
size_t mining_allocator_fixture_last_size(void);

#endif

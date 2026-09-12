/* ESP-IDF uses an isolated test copy; the host compiles the source directly. */
#ifdef ESP_PLATFORM
#include "bm1397_fixture_bindings.h"
#include "../bm1397.c"
#endif

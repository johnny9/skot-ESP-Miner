#ifndef ASIC_NATIVE_UNITY_H
#define ASIC_NATIVE_UNITY_H
#include "unity.h"
typedef struct asic_test_entry {
    void (*function)(void);
    const char *name, *file;
    int line;
    struct asic_test_entry *next;
} asic_test_entry_t;
void asic_test_register(asic_test_entry_t *entry);
#define ASIC_TEST_JOIN_(a, b) a##b
#define ASIC_TEST_JOIN(a, b) ASIC_TEST_JOIN_(a, b)
#define TEST_CASE(name, tags) ASIC_TEST_CASE(name, __LINE__)
#define ASIC_TEST_CASE(name, line) \
    static void ASIC_TEST_JOIN(test_, line)(void); \
    static void __attribute__((constructor)) ASIC_TEST_JOIN(register_, line)(void) { \
        static asic_test_entry_t entry = { ASIC_TEST_JOIN(test_, line), name, __FILE__, line, NULL }; \
        asic_test_register(&entry); \
    } \
    static void ASIC_TEST_JOIN(test_, line)(void)
#endif

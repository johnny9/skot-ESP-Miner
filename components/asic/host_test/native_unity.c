#include "native_unity.h"
static asic_test_entry_t *tests;
void asic_test_register(asic_test_entry_t *entry) { entry->next = tests; tests = entry; }
void setUp(void) {}
void tearDown(void) {}
int main(void)
{
    UnityBegin("asic native regressions");
    for (asic_test_entry_t *test = tests; test; test = test->next) {
        Unity.TestFile = test->file;
        UnityDefaultTestRun(test->function, test->name, test->line);
    }
    return UnityEnd();
}

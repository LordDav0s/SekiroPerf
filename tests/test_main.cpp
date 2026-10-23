// Host-side tests for the portable parts of the mod (signatures, FSR constants).

#include <cstdio>

#include "check.h"

int g_failures = 0;

void RunSigTests();
void RunFsrTests();

int main() {
    RunSigTests();
    RunFsrTests();
    if (g_failures == 0) std::printf("All tests passed\n");
    return g_failures == 0 ? 0 : 1;
}

#include "PPT.h"
#include "utils.h"


int main() {
    Struct addresses = {0};
    clock_t start_time = 0;
    clock_t end_time = 0;
    int result = 0;

    addresses = prepareForMapping();

    // CRUCIAL! In Tom's code this is only set through the menu in the main...
    // S = SetsLLC;

    start_time = clock();
    addresses = map_LLC(50, addresses);
    end_time = clock();

    result = CheckResult();

    printf("CheckResult finished with %d mistakes and the mapping took %f seconds.\n",
        result,
        ((double)end_time - (double)start_time)/CLOCKS_PER_SEC
    );

    return 0;
}
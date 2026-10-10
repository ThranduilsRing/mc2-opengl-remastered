#ifndef MC2_GAMEOS_UTILS_TIMING_H   // not TIMING_H: mclib/timing.h uses that guard, so whichever came second was silently skipped
#define MC2_GAMEOS_UTILS_TIMING_H

#include <stdint.h>

namespace timing {

void sleep(unsigned int nanosec);
void init();
uint64_t gettickcount();
uint64_t ticks2ms(uint64_t ticks);
uint64_t get_wall_time_ms();

};

#endif // TIMING_H

#ifndef __FILE_UTILS__
#define __FILE_UTILS__

#include "mc2_czstring.h"
#include <stdint.h>
#include <string>

namespace filesystem {

extern const char kPathSeparatorAsChar;
extern const char *const kPathSeparator;

uint64_t get_file_mod_time_ms(mc2::czstring filename);


std::string get_path(mc2::czstring fname);

}

#endif //__FILE_UTILS__


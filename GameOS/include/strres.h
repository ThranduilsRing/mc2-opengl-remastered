#ifndef STRRES_H
#define STRRES_H

#pragma once

using get_string_by_id_fptr = const char* (*)(unsigned int);

    using init_string_resources_fptr = void(*)(void);
    using free_string_resources_fptr = void(*)(void);

struct gos_StringRes {
    const char** strings;
    int num_strings;
    void* module;
    get_string_by_id_fptr getStringByIdFptr;
};

#endif // STRRES_H

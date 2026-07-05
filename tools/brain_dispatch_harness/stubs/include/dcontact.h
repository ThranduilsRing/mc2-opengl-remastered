#pragma once
#ifndef DCONTACT_H
#define DCONTACT_H
// HARNESS-STUB-REPAIR-1: dcontact.h stub (BRAIN-ENGAGE-1 deps).
// Defines DCONTACT_H so the engine code/dcontact.h is skipped.
// Values mirror code/dcontact.h exactly (MAX_CONTACTS_PER_SENSOR=200;
// ContactSortType enum order NONE, CV, DISTANCE).

#define MAX_CONTACTS_PER_SENSOR 200

typedef enum {
    CONTACT_SORT_NONE,
    CONTACT_SORT_CV,
    CONTACT_SORT_DISTANCE,
    NUM_CONTACT_SORTS
} ContactSortType;

#endif // DCONTACT_H

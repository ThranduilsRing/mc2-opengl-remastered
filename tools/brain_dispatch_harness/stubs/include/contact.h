#pragma once
#ifndef CONTACT_H
#define CONTACT_H
// HARNESS-STUB-REPAIR-1: contact.h stub (BRAIN-ENGAGE-1 deps).
// Defines CONTACT_H so the engine code/contact.h (SensorSystem — pulls mclib)
// is skipped. brain_special_dispatch.cpp includes contact.h but references no
// SensorSystem symbol directly (detection runs through Mover::getContacts),
// so an empty guard-only stub is sufficient.
#endif // CONTACT_H

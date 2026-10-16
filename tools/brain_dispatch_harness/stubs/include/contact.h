#pragma once
#ifndef CONTACT_H
#define CONTACT_H
// contact.h stub.
// Defines CONTACT_H so the engine code/contact.h (SensorSystem — pulls mclib)
// is skipped. brain_special_dispatch.cpp includes contact.h but references no
// SensorSystem symbol directly (detection runs through Mover::getContacts),
// so an empty guard-only stub is sufficient.
#endif // CONTACT_H

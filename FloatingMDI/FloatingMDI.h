#pragma once

#include "resource.h"

// Window class name for the FloatingMDIClient — a drop-in replacement for
// the system's MDICLIENT class. See FloatingMDI.cpp for the design.
extern const wchar_t* const kFloatingMDIClientClass;

// Register the FloatingMDIClient class. Call once at startup, before
// creating any frame that will host a FloatingMDIClient.
ATOM RegisterFloatingMDIClientClass(HINSTANCE hInstance);

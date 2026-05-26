// Centralized Windows SDK wrapper that applies project-wide macro hygiene
// before exposing Win32 declarations to public project headers.
#pragma once

#include "wh/core/compiler.hpp"

#if WH_OS_WINDOWS

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

// Win32 aliases `GetObject` to ANSI/Unicode variants, which collides with
// third-party APIs such as RapidJSON's `GetObject()` member.
#ifdef GetObject
#undef GetObject
#endif

#endif

// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - symbol visibility for the MAGE_Engine shared library.
//
//   MAGE_ENGINE_BUILD   defined while compiling MAGE_Engine itself  -> export
//   MAGE_ENGINE_SHARED  defined for consumers of the shared library -> import
//   neither             static build                                -> nothing

#if defined(_WIN32)
#if defined(MAGE_ENGINE_BUILD)
#define MAGE_API __declspec(dllexport)
#elif defined(MAGE_ENGINE_SHARED)
#define MAGE_API __declspec(dllimport)
#else
#define MAGE_API
#endif
#else
#define MAGE_API __attribute__((visibility("default")))
#endif

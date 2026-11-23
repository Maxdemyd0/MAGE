// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "renderer/GL.h"
#include "core/Log.h"

namespace mage::gl {

#define MAGE_GL_DEFINE(ret, name, args) PFN_gl##name gl##name = nullptr;
MAGE_GL_REQUIRED_FUNCTIONS(MAGE_GL_DEFINE)
MAGE_GL_OPTIONAL_FUNCTIONS(MAGE_GL_DEFINE)
#undef MAGE_GL_DEFINE

bool LoadFunctions(GLProcLoader loader) {
    bool ok = true;
#define MAGE_GL_LOAD_REQUIRED(ret, name, args)                                   \
    gl##name = reinterpret_cast<PFN_gl##name>(loader("gl" #name));               \
    if (!gl##name) {                                                             \
        Log::Error("OpenGL entry point missing: gl" #name);                      \
        ok = false;                                                              \
    }
#define MAGE_GL_LOAD_OPTIONAL(ret, name, args) \
    gl##name = reinterpret_cast<PFN_gl##name>(loader("gl" #name));
    MAGE_GL_REQUIRED_FUNCTIONS(MAGE_GL_LOAD_REQUIRED)
    MAGE_GL_OPTIONAL_FUNCTIONS(MAGE_GL_LOAD_OPTIONAL)
#undef MAGE_GL_LOAD_REQUIRED
#undef MAGE_GL_LOAD_OPTIONAL
    return ok;
}

} // namespace mage::gl

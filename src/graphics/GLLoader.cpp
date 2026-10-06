// =============================================================================
//  GLLoader.cpp
// =============================================================================
#include "graphics/GLLoader.hpp"

namespace ide::gl {

#define IDE_GL_DEFINE(ret, name, ...) PFN_##name name = nullptr;
IDE_GL_FUNCS(IDE_GL_DEFINE)
#undef IDE_GL_DEFINE

static bool g_loaded = false;

bool load(GetProcFn getProc, const char** missing) {
    bool ok = true;
#define IDE_GL_LOAD(ret, name, ...)                                         \
    name = reinterpret_cast<PFN_##name>(getProc("gl" #name));               \
    if (!name && ok) { ok = false; if (missing) *missing = "gl" #name; }
    IDE_GL_FUNCS(IDE_GL_LOAD)
#undef IDE_GL_LOAD
    g_loaded = ok;
    return ok;
}

bool loaded() { return g_loaded; }

} // namespace ide::gl

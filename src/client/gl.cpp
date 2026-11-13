#include "gl.h"

#include <SDL3/SDL.h>

namespace glfn {
#define BOYZ_GL_DEF(type, name) type name = nullptr;
BOYZ_GL_FUNCS(BOYZ_GL_DEF)
#undef BOYZ_GL_DEF

bool load() {
    bool ok = true;
#define BOYZ_GL_LOAD(type, name)                                          \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));          \
    if (!name) {                                                          \
        SDL_Log("missing GL function %s", #name);                         \
        ok = false;                                                       \
    }
    BOYZ_GL_FUNCS(BOYZ_GL_LOAD)
#undef BOYZ_GL_LOAD
    return ok;
}
}  // namespace glfn

#include "gl.h"

#include "platform.h"

namespace gl {
#define MODELER_GL_DEFINE(ret, name, params) PFN_##name name = nullptr;
MODELER_GL_FUNCTIONS(MODELER_GL_DEFINE)
#undef MODELER_GL_DEFINE

bool load(std::string& missing) {
    missing.clear();
#define MODELER_GL_LOAD(ret, name, params)                                  \
    name = reinterpret_cast<PFN_##name>(platform::getProcAddress("gl" #name)); \
    if (!name) missing += "gl" #name " ";
    MODELER_GL_FUNCTIONS(MODELER_GL_LOAD)
#undef MODELER_GL_LOAD
    return missing.empty();
}
}  // namespace gl

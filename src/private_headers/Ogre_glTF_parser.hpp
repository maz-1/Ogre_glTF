#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Ogre_glTF_gltfModel.hpp"

namespace Ogre_glTF {
namespace gltf {

bool parseFile(Model& dest, std::string& error, const std::string& path);
bool parseGlb(Model& dest, std::string& error, const uint8_t* data, size_t size,
              const std::string& baseDir);

} // namespace gltf
} // namespace Ogre_glTF

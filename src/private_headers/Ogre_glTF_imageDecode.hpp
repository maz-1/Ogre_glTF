#pragma once

namespace Ogre_glTF
{
namespace gltf
{

struct Model;

// Resolve encoded glTF images and populate Image::image with RGBA8 pixels.
void decodeImages(Model& model);

} // namespace gltf
} // namespace Ogre_glTF

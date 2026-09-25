#pragma once

#include <tiny_gltf_v3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Ogre_glTF
{
namespace gltf
{

struct Buffer
{
	std::string name;
	std::string uri;
	std::size_t byteLength = 0;
	std::vector<unsigned char> data;
};

struct BufferView
{
	std::string name;
	int buffer = -1;
	std::size_t byteOffset = 0;
	std::size_t byteLength = 0;
	std::size_t byteStride = 0;
};

struct Accessor
{
	std::string name;
	int bufferView = -1;
	std::size_t byteOffset = 0;
	std::size_t count = 0;
	int componentType = 0;
	int type = 0;
	bool normalized = false;
	bool isSparse = false;
	std::vector<double> minValues;
	std::vector<double> maxValues;

	int ByteStride(const BufferView& view) const;
};

struct Image
{
	std::string name;
	std::string uri;
	std::string mimeType;
	int bufferView = -1;
	int width = -1;
	int height = -1;
	int component = -1;
	int bits = -1;
	int pixel_type = -1;
	bool asIs = false;
	std::vector<unsigned char> image; // Filled by the plugin's image decoder.
};

struct Sampler
{
	std::string name;
	int minFilter = -1;
	int magFilter = -1;
	int wrapS = TG3_TEXTURE_WRAP_REPEAT;
	int wrapT = TG3_TEXTURE_WRAP_REPEAT;
};

struct Texture
{
	std::string name;
	int sampler = -1;
	int source = -1;
};

struct TextureInfo
{
	int index = -1;
	int texCoord = 0;
};

struct NormalTextureInfo : TextureInfo
{
	double scale = 1.0;
};

struct OcclusionTextureInfo : TextureInfo
{
	double strength = 1.0;
};

struct PbrMetallicRoughness
{
	std::array<double, 4> baseColorFactor {{1.0, 1.0, 1.0, 1.0}};
	TextureInfo baseColorTexture;
	double metallicFactor = 1.0;
	double roughnessFactor = 1.0;
	TextureInfo metallicRoughnessTexture;
};

struct Material
{
	std::string name;
	PbrMetallicRoughness pbrMetallicRoughness;
	NormalTextureInfo normalTexture;
	OcclusionTextureInfo occlusionTexture;
	TextureInfo emissiveTexture;
	std::array<double, 3> emissiveFactor {{0.0, 0.0, 0.0}};
	std::string alphaMode = "OPAQUE";
	double alphaCutoff = 0.5;
	bool doubleSided = false;
};

struct Primitive
{
	std::map<std::string, int> attributes;
	int material = -1;
	int indices = -1;
	int mode = TG3_MODE_TRIANGLES;
};

struct Mesh
{
	std::string name;
	std::vector<Primitive> primitives;
	std::vector<double> weights;
};

struct Node
{
	std::string name;
	int camera = -1;
	int skin = -1;
	int mesh = -1;
	std::vector<int> children;
	std::vector<double> rotation;
	std::vector<double> scale;
	std::vector<double> translation;
	std::vector<double> matrix; // Empty when the glTF node has no matrix.
	std::vector<double> weights;
};

struct Skin
{
	std::string name;
	int inverseBindMatrices = -1;
	int skeleton = -1;
	std::vector<int> joints;
};

struct AnimationChannel
{
	int sampler = -1;
	int target_node = -1;
	std::string target_path;
};

struct AnimationSampler
{
	int input = -1;
	int output = -1;
	std::string interpolation;
};

struct Animation
{
	std::string name;
	std::vector<AnimationChannel> channels;
	std::vector<AnimationSampler> samplers;
};

struct Scene
{
	std::string name;
	std::vector<int> nodes;
};

struct Camera
{
	std::string name;
};

struct Light
{
	std::string name;
};

struct Model
{
	std::vector<Accessor> accessors;
	std::vector<Animation> animations;
	std::vector<Buffer> buffers;
	std::vector<BufferView> bufferViews;
	std::vector<Material> materials;
	std::vector<Mesh> meshes;
	std::vector<Node> nodes;
	std::vector<Texture> textures;
	std::vector<Image> images;
	std::vector<Skin> skins;
	std::vector<Sampler> samplers;
	std::vector<Camera> cameras;
	std::vector<Scene> scenes;
	std::vector<Light> lights;
	int defaultScene = -1;
	std::string baseDir; // Assigned by the parser; used for external image URIs.
};

// Copies all fields consumed by the Ogre importers, so the tg3 arena may then be freed.
Model fromTinyGltf3(const tg3_model& raw);

} // namespace gltf
} // namespace Ogre_glTF

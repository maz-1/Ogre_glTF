#include "Ogre_glTF_gltfModel.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Ogre_glTF
{
namespace gltf
{
namespace
{

std::string copyString(tg3_str value)
{
	if(value.len != 0 && !value.data)
		throw std::invalid_argument("tinygltf v3 string has a length but no data");
	return value.len ? std::string(value.data, value.len) : std::string();
}

std::size_t checkedSize(std::uint64_t value)
{
	if(value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
		throw std::length_error("tinygltf v3 array exceeds addressable memory");
	return static_cast<std::size_t>(value);
}

template <typename Output, typename Input>
std::vector<Output> copyArray(const Input* data, std::size_t count)
{
	if(count != 0 && !data)
		throw std::invalid_argument("tinygltf v3 array has a length but no data");
	std::vector<Output> result;
	result.reserve(count);
	for(std::size_t i = 0; i < count; ++i)
		result.push_back(static_cast<Output>(data[i]));
	return result;
}

template <typename T>
void requireArray(const T* data, std::size_t count)
{
	if(count != 0 && !data)
		throw std::invalid_argument("tinygltf v3 model has a nonempty null array");
}

TextureInfo copyTextureInfo(const tg3_texture_info& source)
{
	TextureInfo result;
	result.index = source.index;
	result.texCoord = source.tex_coord;
	return result;
}

NormalTextureInfo copyTextureInfo(const tg3_normal_texture_info& source)
{
	NormalTextureInfo result;
	result.index = source.index;
	result.texCoord = source.tex_coord;
	result.scale = source.scale;
	return result;
}

OcclusionTextureInfo copyTextureInfo(const tg3_occlusion_texture_info& source)
{
	OcclusionTextureInfo result;
	result.index = source.index;
	result.texCoord = source.tex_coord;
	result.strength = source.strength;
	return result;
}

} // namespace

int Accessor::ByteStride(const BufferView& view) const
{
	if(view.byteStride != 0)
	{
		if(view.byteStride > static_cast<std::size_t>(std::numeric_limits<int>::max())) return -1;
		return static_cast<int>(view.byteStride);
	}
	const int componentSize = tg3_component_size(componentType);
	const int componentCount = tg3_num_components(type);
	if(componentSize < 0 || componentCount < 0) return -1;
	return componentSize * componentCount;
}

Model fromTinyGltf3(const tg3_model& raw)
{
	Model model;
	model.defaultScene = raw.default_scene;

	requireArray(raw.buffers, raw.buffers_count);
	model.buffers.reserve(raw.buffers_count);
	for(std::uint32_t i = 0; i < raw.buffers_count; ++i)
	{
		const auto& source = raw.buffers[i];
		Buffer buffer;
		buffer.name = copyString(source.name);
		buffer.uri = copyString(source.uri);
		buffer.byteLength = checkedSize(source.byte_length);
		buffer.data = copyArray<unsigned char>(source.data.data, checkedSize(source.data.count));
		model.buffers.push_back(std::move(buffer));
	}

	requireArray(raw.buffer_views, raw.buffer_views_count);
	model.bufferViews.reserve(raw.buffer_views_count);
	for(std::uint32_t i = 0; i < raw.buffer_views_count; ++i)
	{
		const auto& source = raw.buffer_views[i];
		BufferView view;
		view.name = copyString(source.name);
		view.buffer = source.buffer;
		view.byteOffset = checkedSize(source.byte_offset);
		view.byteLength = checkedSize(source.byte_length);
		view.byteStride = source.byte_stride;
		model.bufferViews.push_back(std::move(view));
	}

	requireArray(raw.accessors, raw.accessors_count);
	model.accessors.reserve(raw.accessors_count);
	for(std::uint32_t i = 0; i < raw.accessors_count; ++i)
	{
		const auto& source = raw.accessors[i];
		Accessor accessor;
		accessor.name = copyString(source.name);
		accessor.bufferView = source.buffer_view;
		accessor.byteOffset = checkedSize(source.byte_offset);
		accessor.count = checkedSize(source.count);
		accessor.componentType = source.component_type;
		accessor.type = source.type;
		accessor.normalized = source.normalized != 0;
		accessor.isSparse = source.sparse.is_sparse != 0;
		accessor.minValues = copyArray<double>(source.min_values, source.min_values_count);
		accessor.maxValues = copyArray<double>(source.max_values, source.max_values_count);
		model.accessors.push_back(std::move(accessor));
	}

	requireArray(raw.images, raw.images_count);
	model.images.reserve(raw.images_count);
	for(std::uint32_t i = 0; i < raw.images_count; ++i)
	{
		const auto& source = raw.images[i];
		Image image;
		image.name = copyString(source.name);
		image.uri = copyString(source.uri);
		image.mimeType = copyString(source.mime_type);
		image.bufferView = source.buffer_view;
		image.width = source.width;
		image.height = source.height;
		image.component = source.component;
		image.bits = source.bits;
		image.pixel_type = source.pixel_type;
		image.asIs = source.as_is != 0;
		model.images.push_back(std::move(image));
	}

	requireArray(raw.samplers, raw.samplers_count);
	model.samplers.reserve(raw.samplers_count);
	for(std::uint32_t i = 0; i < raw.samplers_count; ++i)
	{
		const auto& source = raw.samplers[i];
		Sampler sampler;
		sampler.name = copyString(source.name);
		sampler.minFilter = source.min_filter;
		sampler.magFilter = source.mag_filter;
		sampler.wrapS = source.wrap_s;
		sampler.wrapT = source.wrap_t;
		model.samplers.push_back(std::move(sampler));
	}

	requireArray(raw.textures, raw.textures_count);
	model.textures.reserve(raw.textures_count);
	for(std::uint32_t i = 0; i < raw.textures_count; ++i)
	{
		const auto& source = raw.textures[i];
		Texture texture;
		texture.name = copyString(source.name);
		texture.sampler = source.sampler;
		texture.source = source.source;
		model.textures.push_back(std::move(texture));
	}

	requireArray(raw.materials, raw.materials_count);
	model.materials.reserve(raw.materials_count);
	for(std::uint32_t i = 0; i < raw.materials_count; ++i)
	{
		const auto& source = raw.materials[i];
		const auto& pbr = source.pbr_metallic_roughness;
		Material material;
		material.name = copyString(source.name);
		std::copy(std::begin(source.emissive_factor), std::end(source.emissive_factor), material.emissiveFactor.begin());
		material.alphaMode = copyString(source.alpha_mode);
		material.alphaCutoff = source.alpha_cutoff;
		material.doubleSided = source.double_sided != 0;
		std::copy(std::begin(pbr.base_color_factor), std::end(pbr.base_color_factor), material.pbrMetallicRoughness.baseColorFactor.begin());
		material.pbrMetallicRoughness.baseColorTexture = copyTextureInfo(pbr.base_color_texture);
		material.pbrMetallicRoughness.metallicFactor = pbr.metallic_factor;
		material.pbrMetallicRoughness.roughnessFactor = pbr.roughness_factor;
		material.pbrMetallicRoughness.metallicRoughnessTexture = copyTextureInfo(pbr.metallic_roughness_texture);
		material.normalTexture = copyTextureInfo(source.normal_texture);
		material.occlusionTexture = copyTextureInfo(source.occlusion_texture);
		material.emissiveTexture = copyTextureInfo(source.emissive_texture);
		model.materials.push_back(std::move(material));
	}

	requireArray(raw.meshes, raw.meshes_count);
	model.meshes.reserve(raw.meshes_count);
	for(std::uint32_t i = 0; i < raw.meshes_count; ++i)
	{
		const auto& source = raw.meshes[i];
		Mesh mesh;
		mesh.name = copyString(source.name);
		mesh.weights = copyArray<double>(source.weights, source.weights_count);
		requireArray(source.primitives, source.primitives_count);
		mesh.primitives.reserve(source.primitives_count);
		for(std::uint32_t p = 0; p < source.primitives_count; ++p)
		{
			const auto& input = source.primitives[p];
			Primitive primitive;
			primitive.material = input.material;
			primitive.indices = input.indices;
			primitive.mode = input.mode < 0 ? TG3_MODE_TRIANGLES : input.mode;
			requireArray(input.attributes, input.attributes_count);
			for(std::uint32_t a = 0; a < input.attributes_count; ++a)
				primitive.attributes[copyString(input.attributes[a].key)] = input.attributes[a].value;
			mesh.primitives.push_back(std::move(primitive));
		}
		model.meshes.push_back(std::move(mesh));
	}

	requireArray(raw.nodes, raw.nodes_count);
	model.nodes.reserve(raw.nodes_count);
	for(std::uint32_t i = 0; i < raw.nodes_count; ++i)
	{
		const auto& source = raw.nodes[i];
		Node node;
		node.name = copyString(source.name);
		node.camera = source.camera;
		node.skin = source.skin;
		node.mesh = source.mesh;
		node.children = copyArray<int>(source.children, source.children_count);
		node.rotation = copyArray<double>(source.rotation, 4);
		node.scale = copyArray<double>(source.scale, 3);
		node.translation = copyArray<double>(source.translation, 3);
		if(source.has_matrix) node.matrix = copyArray<double>(source.matrix, 16);
		node.weights = copyArray<double>(source.weights, source.weights_count);
		model.nodes.push_back(std::move(node));
	}

	requireArray(raw.skins, raw.skins_count);
	model.skins.reserve(raw.skins_count);
	for(std::uint32_t i = 0; i < raw.skins_count; ++i)
	{
		const auto& source = raw.skins[i];
		Skin skin;
		skin.name = copyString(source.name);
		skin.inverseBindMatrices = source.inverse_bind_matrices;
		skin.skeleton = source.skeleton;
		skin.joints = copyArray<int>(source.joints, source.joints_count);
		model.skins.push_back(std::move(skin));
	}

	requireArray(raw.animations, raw.animations_count);
	model.animations.reserve(raw.animations_count);
	for(std::uint32_t i = 0; i < raw.animations_count; ++i)
	{
		const auto& source = raw.animations[i];
		Animation animation;
		animation.name = copyString(source.name);
		requireArray(source.channels, source.channels_count);
		animation.channels.reserve(source.channels_count);
		for(std::uint32_t c = 0; c < source.channels_count; ++c)
		{
			const auto& input = source.channels[c];
			AnimationChannel channel;
			channel.sampler = input.sampler;
			channel.target_node = input.target.node;
			channel.target_path = copyString(input.target.path);
			animation.channels.push_back(std::move(channel));
		}
		requireArray(source.samplers, source.samplers_count);
		animation.samplers.reserve(source.samplers_count);
		for(std::uint32_t s = 0; s < source.samplers_count; ++s)
		{
			const auto& input = source.samplers[s];
			AnimationSampler sampler;
			sampler.input = input.input;
			sampler.output = input.output;
			sampler.interpolation = copyString(input.interpolation);
			animation.samplers.push_back(std::move(sampler));
		}
		model.animations.push_back(std::move(animation));
	}

	requireArray(raw.scenes, raw.scenes_count);
	model.scenes.reserve(raw.scenes_count);
	for(std::uint32_t i = 0; i < raw.scenes_count; ++i)
	{
		const auto& source = raw.scenes[i];
		Scene scene;
		scene.name = copyString(source.name);
		scene.nodes = copyArray<int>(source.nodes, source.nodes_count);
		model.scenes.push_back(std::move(scene));
	}

	requireArray(raw.cameras, raw.cameras_count);
	model.cameras.reserve(raw.cameras_count);
	for(std::uint32_t i = 0; i < raw.cameras_count; ++i)
		model.cameras.push_back(Camera { copyString(raw.cameras[i].name) });

	requireArray(raw.lights, raw.lights_count);
	model.lights.reserve(raw.lights_count);
	for(std::uint32_t i = 0; i < raw.lights_count; ++i)
		model.lights.push_back(Light { copyString(raw.lights[i].name) });

	return model;
}

} // namespace gltf
} // namespace Ogre_glTF

#include "Ogre_glTF_materialLoader.hpp"
#include "Ogre_glTF_textureImporter.hpp"
#include "Ogre_glTF_common.hpp"
#include "Ogre_glTF.hpp"
#include <OgreHlmsPbsDatablock.h>
#include <OgreHlms.h>
#include <OgreHlmsManager.h>
#include <OgreLogManager.h>
#include "Ogre_glTF_internal_utils.hpp"

using namespace Ogre_glTF;

materialLoader::materialLoader(gltf::Model& input, textureImporter& textureInterface) :
	textureImporterRef { textureInterface }, 
	model { input } 
{}

void materialLoader::releaseCreatedDatablocksSince(size_t keepCount)
{
	auto* pbs = Ogre::Root::getSingleton().getHlmsManager()->getHlms(Ogre::HLMS_PBS);
	if(!pbs && createdDatablocks.size() > keepCount)
		throw InitError("Cannot release glTF materials after Ogre PBS has shut down");
	while(createdDatablocks.size() > keepCount) {
		const auto name = createdDatablocks.back();
		const Ogre::IdString id(name);
		if(pbs->getDatablock(id)) pbs->destroyDatablock(id);
		for(auto it = assignedDatablockNames.begin(); it != assignedDatablockNames.end();) {
			if(it->second == name) it = assignedDatablockNames.erase(it);
			else ++it;
		}
		createdDatablocks.pop_back();
	}
}

void materialLoader::setBaseColor(Ogre::HlmsPbsDatablock* block, Ogre::Vector3 color) const
{
	block->setDiffuse(color);
}

void materialLoader::setMetallicValue(Ogre::HlmsPbsDatablock* block, Ogre::Real value) const
{
	block->setMetalness(value);
}

void materialLoader::setRoughnesValue(Ogre::HlmsPbsDatablock* block, Ogre::Real value) const
{
	block->setRoughness(value);
}

void materialLoader::setEmissiveColor(Ogre::HlmsPbsDatablock* block, Ogre::Vector3 color) const
{
	block->setEmissive(color);
}

bool materialLoader::isTextureIndexValid(int value) const
{
	return !(value < 0);
}

void materialLoader::setBaseColorTexture(Ogre::HlmsPbsDatablock* block, int value) const
{
	if(!isTextureIndexValid(value)) return;
	auto texture = textureImporterRef.getTexture(value, Ogre::PbsTextureTypes::PBSM_DIFFUSE);
	if(texture)
		block->setTexture(Ogre::PbsTextureTypes::PBSM_DIFFUSE, texture);
}

void materialLoader::setMetalRoughTexture(Ogre::HlmsPbsDatablock* block, int gltfTextureID) const
{
	if(!isTextureIndexValid(gltfTextureID)) return;
	//Ogre cannot use combined metal rough textures. Metal is in the R channel, and rough in the G channel. 
	//It seems that the images are loaded as BGR by the libarry
	//R channel is channle 2 (from 0), G channel is 1.

	auto metalTexure = textureImporterRef.getTexture(gltfTextureID, Ogre::PBSM_METALLIC, Ogre::PixelFormatGpu::PFG_RGBA8_UNORM);
	auto roughTexure = textureImporterRef.getTexture(gltfTextureID, Ogre::PBSM_ROUGHNESS, Ogre::PixelFormatGpu::PFG_RGBA8_UNORM);
	
	if(metalTexure)
		block->setTexture(Ogre::PBSM_METALLIC, metalTexure);

	if(roughTexure)
		block->setTexture(Ogre::PBSM_ROUGHNESS, roughTexure);
}

void materialLoader::setNormalTexture(Ogre::HlmsPbsDatablock* block, int value) const
{
	if(!isTextureIndexValid(value)) return;
	auto texture = textureImporterRef.getTexture(value, Ogre::PbsTextureTypes::PBSM_NORMAL);
	//auto texture = textureImporterRef.getTexture(value);
	if(texture)
	{
		block->setTexture(Ogre::PbsTextureTypes::PBSM_NORMAL, texture);
	}
}

void materialLoader::setOcclusionTexture(Ogre::HlmsPbsDatablock* block, int value) const
{
	if(!isTextureIndexValid(value)) return;
	auto texture = textureImporterRef.getTexture(value, Ogre::PbsTextureTypes::PBSM_DIFFUSE);
	if(texture)
	{
		//OgreLog("occlusion texture from textureImporter : " + texture->getName());
		//OgreLog("Warning: Ogre doesn't supoort occlusion map in it's HLMS PBS implementation!");
		//block->setTexture(Ogre::PbsTextureTypes::PBSM_DIFFUSE, 0, texture);
	}
}

void materialLoader::setEmissiveTexture(Ogre::HlmsPbsDatablock* block, int value) const
{
	if(!isTextureIndexValid(value)) return;
	auto texture = textureImporterRef.getTexture(value, Ogre::PbsTextureTypes::PBSM_EMISSIVE);
	if(texture)
	{
		//OgreLog("emissive texture from textureImporter : " + texture->getName());
		block->setTexture(Ogre::PbsTextureTypes::PBSM_EMISSIVE, texture);
	}
}

void materialLoader::setAlphaMode(Ogre::HlmsPbsDatablock* block, const std::string& mode) const
{
	if(mode == "MASK")
	{
		block->setAlphaTest( Ogre::CMPF_GREATER_EQUAL );
	}
}

void materialLoader::setAlphaCutoff(Ogre::HlmsPbsDatablock* block, Ogre::Real value) const
{
	block->setAlphaTestThreshold(value);
}

Ogre::HlmsDatablock* materialLoader::getDatablock(size_t index) const
{
	OgreLog("Loading material...");
	if(index >= model.materials.size())
		throw LoadingError("glTF material index is out of range");
	auto HlmsPbs			 = static_cast<Ogre::HlmsPbs*>(Ogre::Root::getSingleton().getHlmsManager()->getHlms(Ogre::HlmsTypes::HLMS_PBS));
	const auto& material	 = model.materials[index];
	const auto baseName = "glTF_material_" + std::to_string(textureImporterRef.getImportId()) + "_" +
		std::to_string(index) + "_" + material.name;
	auto assigned = assignedDatablockNames.find(index);
	if(assigned != assignedDatablockNames.end()) {
		auto* existing = HlmsPbs->getDatablock(Ogre::IdString(assigned->second));
		if(existing && existing->getNameStr() && *existing->getNameStr() == assigned->second)
			return existing;
		assignedDatablockNames.erase(assigned);
	}

	Ogre::String name = baseName;
	for(size_t nonce = 0; HlmsPbs->getDatablock(Ogre::IdString(name)); ++nonce)
		name = baseName + "_collision_" + std::to_string(nonce);
	const Ogre::IdString nameId(name);
	assignedDatablockNames.emplace(index, name);

	Ogre::HlmsPbsDatablock* datablock = nullptr;
	try {
		datablock = static_cast<Ogre::HlmsPbsDatablock*>(HlmsPbs->createDatablock(
		nameId,
		name,
		Ogre::HlmsMacroblock {},
		Ogre::HlmsBlendblock {},
		Ogre::HlmsParamVec {}));
		createdDatablocks.push_back(name);
		datablock->setWorkflow(Ogre::HlmsPbsDatablock::Workflows::MetallicWorkflow);
		const auto& pbr = material.pbrMetallicRoughness;
		setBaseColor(datablock, Ogre::Vector3(
			static_cast<Ogre::Real>(pbr.baseColorFactor[0]),
			static_cast<Ogre::Real>(pbr.baseColorFactor[1]),
			static_cast<Ogre::Real>(pbr.baseColorFactor[2])));
		const auto alpha = static_cast<Ogre::Real>(pbr.baseColorFactor[3]);
		if(material.alphaMode == "BLEND")
			datablock->setTransparency(alpha, Ogre::HlmsPbsDatablock::Transparent);
		else if(material.alphaMode == "MASK")
			datablock->setTransparency(alpha, Ogre::HlmsPbsDatablock::None);
		else
			datablock->setTransparency(1.0f, Ogre::HlmsPbsDatablock::None, false);
		if(material.doubleSided) datablock->setTwoSidedLighting(true);
		setMetallicValue(datablock, static_cast<Ogre::Real>(pbr.metallicFactor));
		setRoughnesValue(datablock, static_cast<Ogre::Real>(pbr.roughnessFactor));
		setEmissiveColor(datablock, Ogre::Vector3(
			static_cast<Ogre::Real>(material.emissiveFactor[0]),
			static_cast<Ogre::Real>(material.emissiveFactor[1]),
			static_cast<Ogre::Real>(material.emissiveFactor[2])));
		setAlphaMode(datablock, material.alphaMode);
		setAlphaCutoff(datablock, static_cast<Ogre::Real>(material.alphaCutoff));
		setBaseColorTexture(datablock, pbr.baseColorTexture.index);
		setMetalRoughTexture(datablock, pbr.metallicRoughnessTexture.index);
		setNormalTexture(datablock, material.normalTexture.index);
		setEmissiveTexture(datablock, material.emissiveTexture.index);
	} catch(...) {
		if(!createdDatablocks.empty() && createdDatablocks.back() == name)
			createdDatablocks.pop_back();
		if(HlmsPbs->getDatablock(nameId)) HlmsPbs->destroyDatablock(nameId);
		assignedDatablockNames.erase(index);
		throw;
	}

	return datablock;
}

size_t materialLoader::getDatablockCount() const //todo this could use some refactoring. This information is actually fetched like, twice.
{
	if(model.meshes.empty()) return 0;
	if(model.defaultScene >= 0 && static_cast<size_t>(model.defaultScene) < model.scenes.size()) {
		for(const int nodeIndex : model.scenes[model.defaultScene].nodes) {
			if(nodeIndex >= 0 && static_cast<size_t>(nodeIndex) < model.nodes.size()) {
				const int meshIndex = model.nodes[nodeIndex].mesh;
				if(meshIndex >= 0 && static_cast<size_t>(meshIndex) < model.meshes.size())
					return model.meshes[meshIndex].primitives.size();
			}
		}
	}
	return model.meshes.front().primitives.size();
}

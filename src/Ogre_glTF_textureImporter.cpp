#include "Ogre_glTF_textureImporter.hpp"
#include "Ogre_glTF_common.hpp"
#include <cstring>
#include <OgreLogManager.h>
#include <OgreColourValue.h>
#include <OgreImage2.h>
#include <OgreRoot.h>

#include "OgreConfigFile.h"
#include "OgreTextureFilters.h"
#include "Ogre_glTF.hpp"
#include "OgrePrerequisites.h"


using namespace Ogre_glTF;

std::atomic<size_t> textureImporter::mNextId { 0 };

textureImporter::textureImporter(tinygltf::Model& input) : mId { mNextId.fetch_add(1, std::memory_order_relaxed) }, mModel { input } {
	const auto renderSystem	= Ogre::Root::getSingleton().getRenderSystem();
	mTextureManager = renderSystem->getTextureGpuManager();
}

void textureImporter::preparePixelBuffer(Ogre::uint32 componentOffset, const tinygltf::Image* sourceImage)
{
	mPixelBuffer.assign(sourceImage->image.size(), 0);
	for(size_t index = 0; index < mPixelBuffer.size(); index += 4)
		mPixelBuffer[index] = sourceImage->image[index + componentOffset];
}

Ogre::TextureGpu* textureImporter::getTexture(
	int glTFTextureIndex,
	Ogre::PbsTextureTypes texType,
	Ogre::PixelFormatGpu inputPixelFormat)
{

	if(glTFTextureIndex < 0 || static_cast<size_t>(glTFTextureIndex) >= mModel.textures.size())
		throw LoadingError("glTF texture index is out of range");
	const int imageIndex = mModel.textures[glTFTextureIndex].source;
	if(imageIndex < 0 || static_cast<size_t>(imageIndex) >= mModel.images.size())
		throw LoadingError("glTF texture has no valid image source");
	const auto& image = mModel.images[imageIndex];
	// Color maps use sRGB; normal, metalness and roughness data stay linear.
	const auto pixelFormat = (texType == Ogre::PBSM_DIFFUSE || texType == Ogre::PBSM_EMISSIVE)
		? Ogre::PixelFormatGpu::PFG_RGBA8_UNORM_SRGB : inputPixelFormat;
	if(image.width <= 0 || image.height <= 0 || image.bits != 8 || image.component != 4 ||
	   image.pixel_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE)
		throw LoadingError("Only 8-bit RGBA glTF images are supported by the texture importer");

	const auto sizeInBytes = Ogre::PixelFormatGpuUtils::calculateSizeBytes(
		image.width, image.height, 1, 1, pixelFormat, 1, 4);
	if(image.image.size() != sizeInBytes)
		throw LoadingError("Decoded glTF image size does not match its dimensions and pixel format");

	const Ogre::uchar* imageData = nullptr;
	Ogre::uint32 filters = Ogre::TextureFilter::TypeGenerateDefaultMipmaps;
	Ogre::String texTypeasString;

	switch(texType){
		case Ogre::PbsTextureTypes::PBSM_NORMAL: 
			filters        |= Ogre::TextureFilter::TypePrepareForNormalMapping;
			texTypeasString = "NORMAL";
			imageData		= image.image.data();
			break;
		case Ogre::PbsTextureTypes::PBSM_DIFFUSE: 
			texTypeasString = "DIFFUSE";
			imageData		= image.image.data();
			break;
		case Ogre::PbsTextureTypes::PBSM_EMISSIVE: 
			texTypeasString = "EMISSIVE";
			imageData		= image.image.data();
			break;
		case Ogre::PbsTextureTypes::PBSM_ROUGHNESS:
			filters |= Ogre::TextureFilter::TypeLeaveChannelR;
			texTypeasString = "ROUGHNESS";
			preparePixelBuffer(1, &image);
			imageData		= mPixelBuffer.data();
			break;
		case Ogre::PbsTextureTypes::PBSM_METALLIC:
			filters |= Ogre::TextureFilter::TypeLeaveChannelR;
			texTypeasString = "METALLIC";
			preparePixelBuffer(2, &image);
			imageData		= mPixelBuffer.data();
			break;
		default: throw LoadingError("Unsupported Ogre PBS texture type");
	}

	const auto name = "glTF_texture_" + std::to_string(mId) + "_" + image.name + "_" +
		texTypeasString + "_" + std::to_string(glTFTextureIndex) + "_" +
		std::to_string(static_cast<int>(pixelFormat));

	auto texture = mTextureManager->findTextureNoThrow(name);
	if(texture)
	{
		OgreLog("texture: '" + name + "' Already loaded in Ogre::TextureGpuManager");
		return texture;
	}
	OgreLog("Can't find texure '" + name + "'. Generating it from glTF");

	Ogre::Image2 ogreImage;
	ogreImage.createEmptyImage(image.width, image.height, 1, Ogre::TextureTypes::Type2D, pixelFormat);
	std::memcpy(ogreImage.getRawBuffer(), imageData, sizeInBytes);
	if(!ogreImage.generateMipmaps(false, Ogre::Image2::FILTER_GAUSSIAN_HIGH) &&
	   (image.width > 1 || image.height > 1))
		OgreLog("Could not generate mipmaps for glTF texture '" + name + "'; uploading the base level only");

	Ogre::TextureGpu* ogreTexture;
	ogreTexture = mTextureManager->createOrRetrieveTexture(
		name,
		Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::ManualTexture | Ogre::TextureFlags::AutomaticBatching,
		Ogre::TextureTypes::Type2D,
		Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME,
	    filters);

	ogreTexture->setResolution(ogreImage.getWidth(), ogreImage.getHeight());
	ogreTexture->setPixelFormat(ogreImage.getPixelFormat());
	ogreTexture->setNumMipmaps(ogreImage.getNumMipmaps());
	ogreTexture->scheduleTransitionTo(Ogre::GpuResidency::Resident);
	ogreImage.uploadTo(ogreTexture, 0, ogreImage.getNumMipmaps() - 1u);
	return ogreTexture;
}


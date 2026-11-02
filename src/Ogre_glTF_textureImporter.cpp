#include "Ogre_glTF_textureImporter.hpp"
#include "Ogre_glTF_common.hpp"
#include <cstring>
#include <utility>
#include <OgreLogManager.h>
#include <OgreColourValue.h>
#include <OgreImage2.h>
#include <OgreRoot.h>
#include <ScopeExit/ScopeExit.h>

#include "OgreConfigFile.h"
#include "OgreTextureFilters.h"
#include "Ogre_glTF.hpp"
#include "OgrePrerequisites.h"


using namespace Ogre_glTF;

std::atomic<size_t> textureImporter::mNextId { 0 };

textureImporter::textureImporter(gltf::Model& input) : mId { mNextId.fetch_add(1, std::memory_order_relaxed) }, mModel { input } {
	const auto renderSystem	= Ogre::Root::getSingleton().getRenderSystem();
	mTextureManager = renderSystem->getTextureGpuManager();
}

void textureImporter::enableTextureSharing()
{
	mTexturePool = texturePool::forManager(mTextureManager);
}

void textureImporter::releaseTextures()
{
	releaseTexturesSince(0, 0);
	mTexturePool.reset();
}

void textureImporter::releaseTexturesSince(size_t keepLeases, size_t keepCreated)
{
	mTextureLeases.resize(keepLeases);
	while(mCreatedTextures.size() > keepCreated) {
		mTextureManager->destroyTexture(mCreatedTextures.back());
		mCreatedTextures.pop_back();
	}
}

void textureImporter::preparePixelBuffer(Ogre::uint32 componentOffset, const gltf::Image* sourceImage)
{
	mPixelBuffer.assign(sourceImage->image.size(), 0);
	for(size_t index = 0; index < mPixelBuffer.size(); index += 4)
		mPixelBuffer[index] = sourceImage->image[index + componentOffset];
}

namespace
{
///glTF wrap modes use the OpenGL enumerants, Ogre uses its own TextureAddressingMode
Ogre::TextureAddressingMode translateWrapMode(int gltfWrapMode)
{
	switch(gltfWrapMode)
	{
		case TG3_TEXTURE_WRAP_CLAMP_TO_EDGE: return Ogre::TAM_CLAMP;
		case TG3_TEXTURE_WRAP_MIRRORED_REPEAT: return Ogre::TAM_MIRROR;
		case TG3_TEXTURE_WRAP_REPEAT:
		default: return Ogre::TAM_WRAP;
	}
}
}

Ogre::HlmsSamplerblock textureImporter::getSampler(int glTFTextureIndex) const
{
	const gltf::Sampler* sampler = nullptr;
	if(glTFTextureIndex >= 0 && static_cast<size_t>(glTFTextureIndex) < mModel.textures.size())
	{
		const int samplerIndex = mModel.textures[glTFTextureIndex].sampler;
		if(samplerIndex >= 0 && static_cast<size_t>(samplerIndex) < mModel.samplers.size())
			sampler = &mModel.samplers[samplerIndex];
	}

	//glTF defaults: REPEAT wrapping, LINEAR magnification and LINEAR_MIPMAP_LINEAR minification.
	//This matters: assets can store texture coordinates outside [0; 1] and rely on the wrapping,
	//while the default Ogre samplerblock clamps, which smears the texture along one axis.
	Ogre::HlmsSamplerblock params;
	params.mU = translateWrapMode(sampler ? sampler->wrapS : TG3_TEXTURE_WRAP_REPEAT);
	params.mV = translateWrapMode(sampler ? sampler->wrapT : TG3_TEXTURE_WRAP_REPEAT);
	params.mW = params.mV;

	//glTF filter enumerants: 9728 NEAREST, 9729 LINEAR, 9984..9987 the mipmapped variants
	const int magFilter = sampler ? sampler->magFilter : -1;
	params.mMagFilter = magFilter == 9728 ? Ogre::FO_POINT : Ogre::FO_LINEAR;

	switch(sampler ? sampler->minFilter : -1)
	{
		case 9728: params.mMinFilter = Ogre::FO_POINT; params.mMipFilter = Ogre::FO_NONE; break;
		case 9729: params.mMinFilter = Ogre::FO_LINEAR; params.mMipFilter = Ogre::FO_NONE; break;
		case 9984: params.mMinFilter = Ogre::FO_POINT; params.mMipFilter = Ogre::FO_POINT; break;
		case 9985: params.mMinFilter = Ogre::FO_LINEAR; params.mMipFilter = Ogre::FO_POINT; break;
		case 9986: params.mMinFilter = Ogre::FO_POINT; params.mMipFilter = Ogre::FO_LINEAR; break;
		case 9987:
		default: params.mMinFilter = Ogre::FO_LINEAR; params.mMipFilter = Ogre::FO_LINEAR; break;
	}

	return params;
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
	   image.pixel_type != TG3_COMPONENT_TYPE_UNSIGNED_BYTE)
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

	const Ogre::uint32 flags = Ogre::TextureFlags::ManualTexture | Ogre::TextureFlags::AutomaticBatching;
	const auto upload = [&](const Ogre::String& name) -> Ogre::TextureGpu* {
		Ogre::Image2 ogreImage;
		ogreImage.createEmptyImage(image.width, image.height, 1, Ogre::TextureTypes::Type2D, pixelFormat);
		std::memcpy(ogreImage.getRawBuffer(), imageData, sizeInBytes);
		if(!ogreImage.generateMipmaps(false, Ogre::Image2::FILTER_GAUSSIAN_HIGH) &&
		   (image.width > 1 || image.height > 1))
			OgreLog("Could not generate mipmaps for glTF texture '" + name + "'; uploading the base level only");

		Ogre::TextureGpu* ogreTexture = nullptr;
		bool uploaded = false;
		auto releaseOnFailure = [&] {
			if(ogreTexture && !uploaded) mTextureManager->destroyTexture(ogreTexture);
		};
		ScopeExit::ScopeExit<decltype(releaseOnFailure)> guard(std::move(releaseOnFailure));
		ogreTexture = mTextureManager->createOrRetrieveTexture(
			name,
			Ogre::GpuPageOutStrategy::Discard, flags,
			Ogre::TextureTypes::Type2D,
			Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME,
			filters);
		ogreTexture->setResolution(ogreImage.getWidth(), ogreImage.getHeight());
		ogreTexture->setPixelFormat(ogreImage.getPixelFormat());
		ogreTexture->setNumMipmaps(ogreImage.getNumMipmaps());
		ogreTexture->scheduleTransitionTo(Ogre::GpuResidency::Resident);
		ogreImage.uploadTo(ogreTexture, 0, ogreImage.getNumMipmaps() - 1u);
		uploaded = true;
		return ogreTexture;
	};

	if(mTexturePool) {
		texturePool::Descriptor descriptor {
			static_cast<Ogre::uint32>(image.width), static_cast<Ogre::uint32>(image.height),
			pixelFormat, texType, filters, flags,
			std::vector<std::uint8_t>(imageData, imageData + sizeInBytes)
		};
		auto lease = mTexturePool->acquire(std::move(descriptor), upload);
		mTextureLeases.push_back(lease);
		return lease->texture;
	}

	const auto name = "glTF_texture_" + std::to_string(mId) + "_" + image.name + "_" +
		texTypeasString + "_" + std::to_string(glTFTextureIndex) + "_" +
		std::to_string(static_cast<int>(pixelFormat));
	auto texture = mTextureManager->findTextureNoThrow(name);
	if(texture) return texture;
	texture = upload(name);
	try {
		mCreatedTextures.push_back(texture);
	} catch(...) {
		mTextureManager->destroyTexture(texture);
		throw;
	}
	return texture;
}


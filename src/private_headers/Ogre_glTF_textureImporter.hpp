#pragma once

#include "tiny_gltf.h"
#include <atomic>
#include <unordered_map>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreTextureBox.h>
#include <OgrePixelFormatGpu.h>
#include "OgreBitwise.h"
#include <OgreHlms.h>
#include <OgreHlmsPbs.h>


namespace Ogre_glTF
{

	///Import textures described in glTF into Ogre
	class textureImporter
	{
		///List of the loaded basic textures
		std::unordered_map<int, Ogre::TextureGpu *> mLoadedTextures;

		///Unique namespace for this model's textures and material datablocks
		static std::atomic<size_t> mNextId;
		const size_t mId;

		///Reference to the tinygltf
		tinygltf::Model& mModel;

		Ogre::TextureGpuManager* mTextureManager;

		std::vector<std::uint8_t> mPixelBuffer;


	public:
		///Construct the texture importer and assign its unique import identifier
		/// \param input reference to the model that we are loading
		textureImporter(tinygltf::Model& input);

		///Identifier shared by this model's Ogre textures and material datablocks
		size_t getImportId() const { return mId; }

		Ogre::TextureGpu* getTexture(
			int glTFTextureIndex, 
			Ogre::PbsTextureTypes texType, 
			Ogre::PixelFormatGpu inputPixelFormat=Ogre::PixelFormatGpu::PFG_RGBA8_UNORM);

		void preparePixelBuffer(Ogre::uint32 componentOffset, const tinygltf::Image* sourceImage);
	};
}

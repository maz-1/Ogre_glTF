#pragma once

#include <OgreTextureGpuManager.h>
#include <OgreHlmsPbs.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ogre_glTF
{
	///Shares identical uploaded textures between managed glTF assets using one Ogre texture manager.
	class texturePool
	{
	public:
		struct Descriptor
		{
			Ogre::uint32 width;
			Ogre::uint32 height;
			Ogre::PixelFormatGpu pixelFormat;
			Ogre::PbsTextureTypes textureType;
			Ogre::uint32 filters;
			Ogre::uint32 flags;
			std::vector<std::uint8_t> pixels;

			bool operator==(const Descriptor& other) const;
		};

		struct Entry
		{
			Ogre::TextureGpuManager* manager;
			Ogre::TextureGpu* texture = nullptr;
			Descriptor descriptor;

			Entry(Ogre::TextureGpuManager* owner, Descriptor value) : manager(owner), descriptor(std::move(value)) {}
			~Entry();
		};

		static std::shared_ptr<texturePool> forManager(Ogre::TextureGpuManager* manager);
		std::shared_ptr<Entry> acquire(
			Descriptor descriptor,
			const std::function<Ogre::TextureGpu*(const Ogre::String&)>& create);

	private:
		explicit texturePool(Ogre::TextureGpuManager* owner) : manager(owner) {}
		static std::uint64_t hash(const Descriptor& descriptor);

		Ogre::TextureGpuManager* manager;
		std::mutex threadMutex;
		std::thread::id ownerThread;
		std::size_t acquisitions = 0;
		std::unordered_map<std::uint64_t, std::vector<std::weak_ptr<Entry>>> entries;
	};
}

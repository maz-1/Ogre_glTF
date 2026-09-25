#include "Ogre_glTF_texturePool.hpp"
#include <OgreRoot.h>
#include <atomic>
#include <algorithm>
#include <stdexcept>

using namespace Ogre_glTF;

namespace
{
	std::mutex poolsMutex;
	std::unordered_map<Ogre::TextureGpuManager*, std::weak_ptr<texturePool>> pools;
	std::atomic<std::uint64_t> nextTextureName { 0 };

	void hashWord(std::uint64_t& value, std::uint64_t word)
	{
		for(unsigned i = 0; i < 8; ++i) {
			value ^= static_cast<std::uint8_t>(word);
			value *= UINT64_C(1099511628211);
			word >>= 8;
		}
	}
}

bool texturePool::Descriptor::operator==(const Descriptor& other) const
{
	return width == other.width && height == other.height &&
		pixelFormat == other.pixelFormat && textureType == other.textureType &&
		filters == other.filters && flags == other.flags && pixels == other.pixels;
}

texturePool::Entry::~Entry()
{
	// Ogre owns the texture; a managed asset must be released before its Root.
	if(texture && Ogre::Root::getSingletonPtr() && Ogre::Root::getSingleton().isInitialised() &&
	   Ogre::Root::getSingleton().getRenderSystem() &&
	   Ogre::Root::getSingleton().getRenderSystem()->getTextureGpuManager() == manager)
		manager->destroyTexture(texture);
}

std::shared_ptr<texturePool> texturePool::forManager(Ogre::TextureGpuManager* manager)
{
	std::lock_guard<std::mutex> lock(poolsMutex);
	for(auto it = pools.begin(); it != pools.end();) {
		if(it->second.expired()) it = pools.erase(it);
		else ++it;
	}
	auto& weak = pools[manager];
	auto shared = weak.lock();
	if(!shared) {
		shared = std::shared_ptr<texturePool>(new texturePool(manager));
		weak = shared;
	}
	return shared;
}

std::uint64_t texturePool::hash(const Descriptor& descriptor)
{
	std::uint64_t value = UINT64_C(14695981039346656037);
	hashWord(value, 1); // Version of the texture conversion and mipmap algorithm.
	hashWord(value, descriptor.width);
	hashWord(value, descriptor.height);
	hashWord(value, static_cast<std::uint64_t>(descriptor.pixelFormat));
	hashWord(value, static_cast<std::uint64_t>(descriptor.textureType));
	hashWord(value, descriptor.filters);
	hashWord(value, descriptor.flags);
	for(const std::uint8_t byte : descriptor.pixels) {
		value ^= byte;
		value *= UINT64_C(1099511628211);
	}
	return value;
}

std::shared_ptr<texturePool::Entry> texturePool::acquire(
	Descriptor descriptor,
	const std::function<Ogre::TextureGpu*(const Ogre::String&)>& create)
{
	{
		std::lock_guard<std::mutex> lock(threadMutex);
		const auto current = std::this_thread::get_id();
		if(ownerThread == std::thread::id()) ownerThread = current;
		else if(ownerThread != current)
			throw std::runtime_error("Managed glTF textures must be created on the Ogre rendering thread");
	}
	if(++acquisitions % 64 == 0) {
		for(auto it = entries.begin(); it != entries.end();) {
			auto& bucket = it->second;
			bucket.erase(std::remove_if(bucket.begin(), bucket.end(),
				[](const std::weak_ptr<Entry>& value) { return value.expired(); }), bucket.end());
			if(bucket.empty()) it = entries.erase(it);
			else ++it;
		}
	}
	const std::uint64_t key = hash(descriptor);
	auto& bucket = entries[key];
	for(auto it = bucket.begin(); it != bucket.end();) {
		auto candidate = it->lock();
		if(!candidate) {
			it = bucket.erase(it);
		} else {
			if(candidate->descriptor == descriptor)
				return candidate;
			++it;
		}
	}

	Ogre::String name;
	do {
		name = "glTF_shared_texture_" + std::to_string(nextTextureName.fetch_add(1));
	} while(manager->findTextureNoThrow(name));

	auto entry = std::make_shared<Entry>(manager, std::move(descriptor));
	entry->texture = create(name);
	bucket.push_back(entry);
	return entry;
}

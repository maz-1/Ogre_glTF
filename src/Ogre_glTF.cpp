#include <utility>
#include <cstdio>
#include <exception>

#include "Ogre_glTF.hpp"
#include "Ogre_glTF_modelConverter.hpp"
#include "Ogre_glTF_textureImporter.hpp"
#include "Ogre_glTF_materialLoader.hpp"
#include "Ogre_glTF_skeletonImporter.hpp"
#include "Ogre_glTF_parser.hpp"
#include "Ogre_glTF_imageDecode.hpp"
#include "Ogre_glTF_common.hpp"
#include "Ogre_glTF_OgreResource.hpp"
#include "Ogre_glTF_internal_utils.hpp"

#include <OgreItem.h>
#include <OgreMesh2.h>
#include <OgreMeshManager2.h>
#include <OgreOldSkeletonManager.h>
#include <OgreHlmsManager.h>
#include <Animation/OgreTagPoint2.h>
#include <Animation/OgreSkeletonInstance.h>

using namespace Ogre_glTF;

///Implementaiton of the adapter
struct loaderAdapter::impl
{
	///Constructor, initialize once all the objects inclosed in this class. They need a reference
	///to a model object (and sometimes more) given at construct time
	impl() : textureImp(model), materialLoad(model, textureImp),
		modelConv(model, textureImp.getImportId()), skeletonImp(model, textureImp.getImportId()) {}

	///Variable to check if everything is alright with the adapter
	bool valid = false;
	bool resourcesReleased = false;
	SceneInstance* activeScene = nullptr;
	bool ownsGlbResource = false;
	Ogre::String glbResourceName;
	struct ResourceCheckpoint {
		size_t materials = 0;
		size_t textureLeases = 0;
		size_t createdTextures = 0;
		size_t meshes = 0;
		size_t skeletons = 0;
	};

	///The model object that data will be loaded into and read from
	gltf::Model model;

	///Error reported by the parser or image decoder.
	std::string error = "";

	///Texture importer object : go through the texture array and load them into Ogre
	textureImporter textureImp;

	///Material loader : get the data from the material section of the glTF file and create an HlmsDatablock to use
	materialLoader materialLoad;

	///Model converter : load all the actual mesh data from the glTF file, and convert them into index and vertex buffer that can
	///be used to create an Ogre VAO (Vertex Array Object), then create a mesh for it
	modelConverter modelConv;

	///Skeleton importer : load skins from the glTF model, create equivalent OgreSkeleton objects
	skeletonImporter skeletonImp;

	ResourceCheckpoint checkpoint() const {
		return { materialLoad.getCreatedDatablocks().size(), textureImp.getLeaseCount(),
			textureImp.getCreatedTextureCount(), modelConv.getCreatedMeshes().size(),
			skeletonImp.getCreatedSkeletons().size() };
	}

	void rollbackSince(const ResourceCheckpoint& saved) {
		materialLoad.releaseCreatedDatablocksSince(saved.materials);
		textureImp.releaseTexturesSince(saved.textureLeases, saved.createdTextures);
		modelConv.releaseCreatedMeshesSince(saved.meshes);
		skeletonImp.releaseCreatedSkeletonsSince(saved.skeletons);
	}
};

loaderAdapter::loaderAdapter() : pimpl { std::make_unique<impl>() } { OgreLog("Created adapter object..."); }

loaderAdapter::~loaderAdapter() { OgreLog("Destructed adapter object..."); }

void loaderAdapter::loadMainScene(Ogre::SceneNode* parentNode, Ogre::SceneManager* smgr) const
{
	if(!isOk())
		return;

	//pimpl->textureImp.loadTextures();
	auto sceneIdx = pimpl->model.defaultScene >= 0 ? pimpl->model.defaultScene : 0;
	if(static_cast<size_t>(sceneIdx) >= pimpl->model.scenes.size())
		throw LoadingError("glTF has no valid scene to instantiate");
	const auto& scene = pimpl->model.scenes[sceneIdx];

	for(auto nodeIdx : scene.nodes)
	{
		auto* rootNode = getSceneNode(nodeIdx, parentNode, smgr);
		if(pimpl->activeScene && rootNode) pimpl->activeScene->roots.push_back(rootNode);
	}
}

Ogre::SceneNode* loaderAdapter::getFirstSceneNode(Ogre::SceneManager* smgr) const
{
	if(!isOk())
		return nullptr;
	if(pimpl->model.scenes.empty() || pimpl->model.scenes[0].nodes.empty())
		throw LoadingError("glTF has no first scene node");
	
	//pimpl->textureImp.loadTextures();
	return getSceneNode(pimpl->model.scenes[0].nodes[0], smgr->getRootSceneNode(), smgr);
}

Ogre::SceneNode* loaderAdapter::getSceneNode(size_t index, Ogre::SceneNode* parentSceneNode, Ogre::SceneManager* smgr) const
{
	if(pimpl->resourcesReleased) throw InitError("glTF adapter resources have been released");
	if(index >= pimpl->model.nodes.size()) throw LoadingError("glTF scene node index is out of range");

	const auto& node = pimpl->model.nodes[index];
	// Check if node is not a bone
	for(const auto& skin : pimpl->model.skins)
	{
		if(std::find(skin.joints.begin(), skin.joints.end(), index) != skin.joints.end())
		{
			return nullptr;
		}
	}

	auto sceneNode = parentSceneNode->createChildSceneNode();
	if(pimpl->activeScene) pimpl->activeScene->nodes.push_back(sceneNode);
	sceneNode->setName(node.name);
	
	if(!node.translation.empty())
		sceneNode->setPosition(
			Ogre::Real(node.translation[0]), 
			Ogre::Real(node.translation[1]), 
			Ogre::Real(node.translation[2]));

	if(!node.rotation.empty())
		sceneNode->setOrientation(
			Ogre::Real(node.rotation[3]),
			Ogre::Real(node.rotation[0]), 
			Ogre::Real(node.rotation[1]), 
			Ogre::Real(node.rotation[2]));

	if(!node.scale.empty())
		sceneNode->setScale(
			Ogre::Real(node.scale[0]), 
			Ogre::Real(node.scale[1]), 
			Ogre::Real(node.scale[2]));

	if(!node.matrix.empty())
	{
		std::array<Ogre::Real, 4 * 4> matrixArray { 0 };
		internal_utils::container_double_to_real(node.matrix, matrixArray);
		Ogre::Matrix4 matrix { matrixArray.data() };
		Ogre::Vector3 position;
		Ogre::Quaternion orientation;
		Ogre::Vector3 scale;
		matrix.transpose().decomposition(position, scale, orientation);
		sceneNode->setPosition(position);
		sceneNode->setOrientation(orientation);
		sceneNode->setScale(scale);
	}

	if(node.mesh >= 0)
	{
		auto ogreMesh = pimpl->modelConv.getOgreMesh(node.mesh);

		if(node.skin >= 0)
		{
			auto skeleton = this->pimpl->skeletonImp.getSkeleton(node.skin);
			if(skeleton)
			{
				ogreMesh->_notifySkeleton(skeleton);
			}
		}

		auto item		 = smgr->createItem(ogreMesh, Ogre::SCENE_DYNAMIC);
		if(pimpl->activeScene) pimpl->activeScene->items.push_back(item);
		const auto& mesh = pimpl->model.meshes[node.mesh];
		for(size_t i = 0; i < mesh.primitives.size(); ++i) 
		{ 
			auto subItem = item->getSubItem(i);
			if(mesh.primitives[i].material >= 0)
				subItem->setDatablock(getDatablock(mesh.primitives[i].material));
		}
		sceneNode->attachObject(item);

		// Add tag points
		auto skeletonInstance = item->getSkeletonInstance();
		if(skeletonInstance)
		{
			// Find all root bones. Collect all children of all nodes in the skin and then find
			// all nodes in the skin that are not in the set of children. Those are all nodes
			// that don't have a parent because they're not a child of any other node. 
			std::vector<int> rootBones;
			std::vector<int> allChildren;
			const auto& skin = pimpl->model.skins[node.skin];

			for(const auto& nodeIndex : skin.joints)
			{
				const auto& node = pimpl->model.nodes[nodeIndex];
				allChildren.insert(allChildren.end(), node.children.begin(), node.children.end());
			}

			for(const auto& nodeIndex : skin.joints)
			{
				if(std::find(allChildren.begin(), allChildren.end(), nodeIndex) == allChildren.end()) {
					rootBones.push_back(nodeIndex);
				}
			}

			for(int boneIndex : rootBones)
			{
				createTagPoints(boneIndex, skeletonInstance, smgr);
			}
		}
	}

	for(const auto& child : node.children)
	{
		getSceneNode(child, sceneNode, smgr);
	}

	return sceneNode;
}

void loaderAdapter::createTagPoints(int boneIndex, Ogre::SkeletonInstance* skeletonInstance, Ogre::SceneManager* smgr) const
{
	if(pimpl->resourcesReleased) throw InitError("glTF adapter resources have been released");
	if(boneIndex < 0 || static_cast<size_t>(boneIndex) >= pimpl->model.nodes.size())
		throw LoadingError("glTF bone node index is out of range");
	const auto& boneNode = pimpl->model.nodes[boneIndex];

	for(auto child : boneNode.children)
	{
		const auto& childNode = pimpl->model.nodes[child];

		if(childNode.mesh >= 0)
		{
			auto tagPoint = smgr->createTagPoint();
			if(pimpl->activeScene) pimpl->activeScene->nodes.push_back(tagPoint);
			tagPoint->setName(childNode.name);

			Ogre::Vector3 position;
			Ogre::Quaternion orientation;
			Ogre::Vector3 scale(1);
			
			if(!childNode.translation.empty())
				position = Ogre::Vector3(
					Ogre::Real(childNode.translation[0]), 
					Ogre::Real(childNode.translation[1]), 
					Ogre::Real(childNode.translation[2]));

			if(!childNode.rotation.empty())
				orientation = Ogre::Quaternion(
					Ogre::Real(childNode.rotation[3]), 
					Ogre::Real(childNode.rotation[0]),
					Ogre::Real(childNode.rotation[1]), 
					Ogre::Real(childNode.rotation[2]));

			if(!childNode.scale.empty())
				scale = Ogre::Vector3(
					Ogre::Real(childNode.scale[0]), 
					Ogre::Real(childNode.scale[1]), 
					Ogre::Real(childNode.scale[2]));

			if(!childNode.matrix.empty())
			{
				std::array<Ogre::Real, 4 * 4> matrixArray { 0 };
				internal_utils::container_double_to_real(childNode.matrix, matrixArray);
				Ogre::Matrix4 matrix { matrixArray.data() };
				matrix.transpose().decomposition(position, scale, orientation);
			}

			tagPoint->setPosition(position);
			tagPoint->setOrientation(orientation);
			tagPoint->setScale(scale);

			auto ogreMesh = pimpl->modelConv.getOgreMesh(childNode.mesh);

			if(childNode.skin >= 0)
			{
				auto skeleton = this->pimpl->skeletonImp.getSkeleton(childNode.skin);
				if(skeleton)
				{
					ogreMesh->_notifySkeleton(skeleton);
				}
			}

			auto item		 = smgr->createItem(ogreMesh, Ogre::SCENE_DYNAMIC);
			if(pimpl->activeScene) pimpl->activeScene->items.push_back(item);
			const auto& mesh = pimpl->model.meshes[childNode.mesh];
			for(size_t i = 0; i < mesh.primitives.size(); ++i) 
			{ 
				auto subItem = item->getSubItem(i);
				if(mesh.primitives[i].material >= 0)
					subItem->setDatablock(getDatablock(mesh.primitives[i].material));
			}
			tagPoint->attachObject(item);

			auto parentBone = skeletonInstance->getBone(boneNode.name);
			parentBone->addTagPoint(tagPoint);

			for(const auto& childOfChild : childNode.children)
			{
				getSceneNode(childOfChild, tagPoint, smgr);
			}
		}
		else
		{
			createTagPoints(child, skeletonInstance, smgr);
		}
	}
}

Ogre::HlmsDatablock* loaderAdapter::getDatablock(size_t index) const
{
	if(pimpl->resourcesReleased) throw InitError("glTF adapter resources have been released");
	return pimpl->materialLoad.getDatablock(index);
}

size_t loaderAdapter::getDatablockCount() { return pimpl->materialLoad.getDatablockCount(); }

loaderAdapter::loaderAdapter(loaderAdapter&& other) noexcept : pimpl { std::move(other.pimpl) },
	adapterName { std::move(other.adapterName) }
{

	OgreLog("Moved adapter object...");
}

loaderAdapter& loaderAdapter::operator=(loaderAdapter&& other) noexcept
{
	pimpl = std::move(other.pimpl);
	adapterName = std::move(other.adapterName);
	return *this;
}

bool loaderAdapter::isOk() const { return pimpl && pimpl->valid; }

std::string loaderAdapter::getLastError() const { return pimpl ? pimpl->error : std::string(); }

void loaderAdapter::releaseResources()
{
	if(!pimpl || pimpl->resourcesReleased) return;
	if(pimpl->activeScene) throw InitError("Cannot release a glTF asset during scene creation");
	if(!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingleton().isInitialised() ||
	   !Ogre::Root::getSingleton().getHlmsManager() ||
	   !Ogre::Root::getSingleton().getRenderSystem())
		throw InitError("Release glTF resources before Ogre::Root shuts down");

	pimpl->rollbackSince({});
	pimpl->textureImp.releaseTextures();
	if(pimpl->ownsGlbResource && GlbFileManager::getSingletonPtr()) {
		auto& glbManager = GlbFileManager::getSingleton();
		if(glbManager.resourceExists(pimpl->glbResourceName))
			glbManager.remove(pimpl->glbResourceName);
	}
	pimpl->resourcesReleased = true;
	pimpl->valid = false;
}

SceneInstance::~SceneInstance()
{
	try { reset(); }
	catch(const std::exception& error) {
		std::fprintf(stderr, "Ogre_glTF scene release failed: %s\n", error.what());
	}
	catch(...) { std::fputs("Ogre_glTF scene release failed\n", stderr); }
}

SceneInstance::SceneInstance(SceneInstance&& other) noexcept :
	adapter(std::move(other.adapter)), sceneManager(other.sceneManager),
	roots(std::move(other.roots)), nodes(std::move(other.nodes)), items(std::move(other.items))
{
	other.sceneManager = nullptr;
	other.roots.clear();
	other.nodes.clear();
	other.items.clear();
}

SceneInstance& SceneInstance::operator=(SceneInstance&& other)
{
	if(this != &other) {
		reset();
		adapter = std::move(other.adapter);
		sceneManager = other.sceneManager;
		roots = std::move(other.roots);
		nodes = std::move(other.nodes);
		items = std::move(other.items);
		other.sceneManager = nullptr;
		other.roots.clear();
		other.nodes.clear();
		other.items.clear();
	}
	return *this;
}

void SceneInstance::reset()
{
	if(sceneManager) {
		// Destroying Items releases their SkeletonInstances and detaches bone TagPoints.
		// The TagPoint scene nodes can then be destroyed with the other nodes.
		while(!items.empty()) {
			sceneManager->destroyItem(items.back());
			items.pop_back();
		}
		while(!nodes.empty()) {
			sceneManager->destroySceneNode(nodes.back());
			nodes.pop_back();
		}
	}
	items.clear();
	nodes.clear();
	roots.clear();
	sceneManager = nullptr;
	adapter.reset();
}

SceneInstance ManagedAsset::instantiate(Ogre::SceneNode* parentNode, Ogre::SceneManager* smgr) const
{
	if(!adapter || !adapter->isOk()) throw LoadingError("Cannot instantiate an invalid managed glTF asset");
	if(!parentNode || !smgr) throw LoadingError("Managed glTF scene needs a parent node and scene manager");
	if(adapter->pimpl->activeScene) throw InitError("Concurrent scene creation from one glTF asset is unsupported");

	SceneInstance scene;
	scene.adapter = adapter;
	scene.sceneManager = smgr;
	const auto checkpoint = adapter->pimpl->checkpoint();
	adapter->pimpl->activeScene = &scene;
	try {
		adapter->loadMainScene(parentNode, smgr);
	} catch(...) {
		adapter->pimpl->activeScene = nullptr;
		scene.reset();
		adapter->pimpl->rollbackSince(checkpoint);
		throw;
	}
	adapter->pimpl->activeScene = nullptr;
	return scene;
}

///Implementation of the glTF loader. Exist as a pImpl inside the glTFLoader class
struct glTFLoader::glTFLoaderImpl
{
	glTFLoaderImpl() { OgreLog("initialized TinyGLTF v3 C parser"); }

	///Load the content of a file into an adapter object
	bool loadInto(loaderAdapter& adapter, const std::string& path)
	{
		if(!gltf::parseFile(adapter.pimpl->model, adapter.pimpl->error, path)) return false;
		try { gltf::decodeImages(adapter.pimpl->model); }
		catch(const std::exception& e) { adapter.pimpl->error = e.what(); return false; }
		return true;
	}

	bool loadGlb(loaderAdapter& adapter, GlbFilePtr file)
	{
		if(!gltf::parseGlb(adapter.pimpl->model, adapter.pimpl->error,
			file->getData(), file->getSize(), ".")) return false;
		try { gltf::decodeImages(adapter.pimpl->model); }
		catch(const std::exception& e) { adapter.pimpl->error = e.what(); return false; }
		return true;
	}
};

glTFLoader::glTFLoader() : loaderImpl { std::make_unique<glTFLoaderImpl>() }
{
	if(Ogre::Root::getSingletonPtr() == nullptr) throw RootNotInitializedYet("Please create an Ogre::Root instance before initializing the glTF library!");

	if(!Ogre_glTF::GlbFileManager::getSingletonPtr()) new GlbFileManager;

	OgreLog("glTFLoader created!");
}

loaderAdapter glTFLoader::loadFromFileSystem(const std::string& path) const
{
	OgreLog("loading file " + path);
	loaderAdapter adapter;
	adapter.adapterName = path;
	adapter.pimpl->valid = loaderImpl->loadInto(adapter, path);
	if(adapter.pimpl->valid) adapter.pimpl->modelConv.debugDump();
	return adapter;
}

namespace
{
	std::shared_ptr<loaderAdapter> manageAdapter(loaderAdapter&& adapter)
	{
		return std::shared_ptr<loaderAdapter>(new loaderAdapter(std::move(adapter)),
			[](loaderAdapter* value) noexcept {
				try {
					value->releaseResources();
				} catch(const std::exception& error) {
					std::fprintf(stderr, "Ogre_glTF resource release failed: %s\n", error.what());
				} catch(...) {
					std::fputs("Ogre_glTF resource release failed\n", stderr);
				}
				delete value;
			});
	}
}

ManagedAsset glTFLoader::loadManagedFromFileSystem(const std::string& path) const
{
	auto adapter = loadFromFileSystem(path);
	if(!adapter.isOk()) throw LoadingError("Could not load glTF file: " + adapter.getLastError());
	adapter.pimpl->textureImp.enableTextureSharing();
	return ManagedAsset(manageAdapter(std::move(adapter)));
}

loaderAdapter glTFLoader::loadGlbResource(const std::string& name) const
{
	OgreLog("Loading GLB from resource manager " + name);
	auto& glbManager = GlbFileManager::getSingleton();
	auto glbFile	 = glbManager.load(name, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);

	loaderAdapter adapter;
	if(glbFile)
	{
		adapter.pimpl->valid = loaderImpl->loadGlb(adapter, glbFile);
	}

	if(adapter.pimpl->valid) adapter.pimpl->modelConv.debugDump();
	return adapter;
}

ManagedAsset glTFLoader::loadManagedGlbResource(const std::string& name) const
{
	auto& glbManager = GlbFileManager::getSingleton();
	const bool existed = glbManager.resourceExists(name);
	try {
		auto adapter = loadGlbResource(name);
		if(!adapter.isOk())
			throw LoadingError("Could not load GLB resource: " + adapter.getLastError());
		adapter.pimpl->ownsGlbResource = !existed;
		adapter.pimpl->glbResourceName = name;
		adapter.pimpl->textureImp.enableTextureSharing();
		return ManagedAsset(manageAdapter(std::move(adapter)));
	} catch(...) {
		if(!existed && glbManager.resourceExists(name)) glbManager.remove(name);
		throw;
	}
}

glTFLoader::glTFLoader(glTFLoader&& other) noexcept : loaderImpl(std::move(other.loaderImpl)) {}

glTFLoader& glTFLoader::operator=(glTFLoader&& other) noexcept
{
	loaderImpl = std::move(other.loaderImpl);
	return *this;
}

glTFLoader::~glTFLoader() = default;

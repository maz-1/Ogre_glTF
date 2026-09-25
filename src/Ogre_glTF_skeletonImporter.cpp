#include "Ogre_glTF_skeletonImporter.hpp"
#include "Ogre_glTF_common.hpp"
#include "Ogre_glTF_internal_utils.hpp"
#include <OgreOldSkeletonManager.h>
#include <OgreSkeleton.h>
#include <OgreOldBone.h>
#include <OgreLogManager.h>
#include <OgreKeyFrame.h>
#include "Ogre_glTF.hpp"
#include <algorithm>
#include <limits>

using namespace Ogre_glTF;

namespace
{
struct AccessorData
{
	const gltf::Accessor& accessor;
	const unsigned char* data;
	std::size_t stride;
};

AccessorData resolveAccessor(const gltf::Model& model, int accessorIndex)
{
	if(accessorIndex < 0 || static_cast<std::size_t>(accessorIndex) >= model.accessors.size())
		throw LoadingError("glTF animation or skin accessor index is out of range");
	const auto& accessor = model.accessors[accessorIndex];
	if(accessor.isSparse)
		throw LoadingError("Sparse glTF animation and skin accessors are not supported");
	if(accessor.bufferView < 0 || static_cast<std::size_t>(accessor.bufferView) >= model.bufferViews.size())
		throw LoadingError("glTF animation or skin accessor has no valid bufferView");
	const auto& view = model.bufferViews[accessor.bufferView];
	if(view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= model.buffers.size())
		throw LoadingError("glTF bufferView has no valid buffer");
	const auto& buffer = model.buffers[view.buffer];
	const int componentSize = tg3_component_size(accessor.componentType);
	const int componentCount = tg3_num_components(accessor.type);
	const int stride = accessor.ByteStride(view);
	if(componentSize <= 0 || componentCount <= 0 || stride <= 0 ||
	   static_cast<std::size_t>(stride) < static_cast<std::size_t>(componentSize) * componentCount)
		throw LoadingError("glTF animation or skin accessor has an invalid component type or stride");
	if(view.byteOffset > buffer.data.size() || view.byteLength > buffer.data.size() - view.byteOffset ||
	   accessor.byteOffset > view.byteLength)
		throw LoadingError("glTF animation or skin accessor byte range is outside its buffer");
	const auto elementSize = static_cast<std::size_t>(componentSize) * componentCount;
	const auto available = view.byteLength - accessor.byteOffset;
	if(accessor.count > 0 &&
	   (elementSize > available ||
	    accessor.count - 1 > (available - elementSize) / static_cast<std::size_t>(stride)))
		throw LoadingError("glTF animation or skin accessor elements exceed their bufferView");
	return { accessor,
		buffer.data.empty() ? nullptr : buffer.data.data() + view.byteOffset + accessor.byteOffset,
		static_cast<std::size_t>(stride) };
}

void requireFrame(const gltf::Accessor& accessor, int frameID)
{
	if(frameID < 0 || static_cast<std::size_t>(frameID) >= accessor.count ||
	   accessor.count > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		throw LoadingError("glTF animation frame index is out of range");
}
}

void skeletonImporter::addChidren(const std::vector<int>& childs, Ogre::v1::OldBone* parent)
{

	for(auto child : childs)
	{
		const auto& node = model.nodes[child];
		
		if(node.mesh >= 0)
		{
			// child is a mesh
			continue;
		}

		auto bone = skeleton->getBone(nodeToJointMap[child]);
		if(!bone) { throw InitError("could not get bone " + std::to_string(bone->getHandle())); }

		parent->addChild(bone);

		auto bindMatrix = bindMatrices[nodeToJointMap[child]];

		Ogre::Vector3 translation, scale;
		Ogre::Quaternion rotation;

		bindMatrix.decomposition(translation, scale, rotation);

		bone->setPosition(parent->convertWorldToLocalPosition(translation));
		bone->setOrientation(parent->convertWorldToLocalOrientation(rotation));
		bone->setScale(parent->_getDerivedScale() / scale);

		addChidren(model.nodes[child].children, bone);
	}
}

void skeletonImporter::loadBoneHierarchy(int boneIndex)
{
	const auto& node = model.nodes[boneIndex];
	Ogre::v1::OldBone* rootBone = skeleton->getBone(nodeToJointMap[boneIndex]);

	std::array<Ogre::Real, 3> translation = { 0, 0, 0 };
	std::array<Ogre::Real, 3> scale = { 1, 1, 1 };
	std::array<Ogre::Real, 4> rotation = { 0, 0, 0, 1 };
	if(!node.translation.empty())
		internal_utils::container_double_to_real(node.translation, translation);
	if(!node.scale.empty()) 
		internal_utils::container_double_to_real(node.scale, scale);
	if(!node.rotation.empty())
		internal_utils::container_double_to_real(node.rotation, rotation);

	Ogre::Vector3 trans  = Ogre::Vector3 { translation.data() };
	Ogre::Quaternion rot = Ogre::Quaternion { rotation[3], rotation[0], rotation[1], rotation[2] };
	Ogre::Vector3 sc	 = Ogre::Vector3 { scale.data() };

	if(sc.isZeroLength()) sc = Ogre::Vector3::UNIT_SCALE;

	rootBone->setPosition(trans);
	rootBone->setOrientation(rot);
	rootBone->setScale(sc);

	std::stringstream rootBoneXformLog;
	rootBoneXformLog << "rootBone " << trans << " " << rot;
	OgreLog(rootBoneXformLog);

	addChidren(node.children, rootBone);
}

skeletonImporter::skeletonImporter(gltf::Model& input, size_t importId) : model { input }, importId { importId } {}

void skeletonImporter::releaseCreatedSkeletonsSince(size_t keepCount)
{
	skeleton = Ogre::v1::SkeletonPtr();
	auto* skeletonManager = Ogre::v1::OldSkeletonManager::getSingletonPtr();
	while(createdSkeletons.size() > keepCount)
	{
		const auto& created = createdSkeletons.back();
		if(created && skeletonManager && skeletonManager->resourceExists(created->getName()))
			skeletonManager->remove(created->getName());
		createdSkeletons.pop_back();
	}
	bindMatrices.clear();
	nodeToJointMap.clear();
}

void skeletonImporter::loadTimepointFromSamplerToKeyFrame(int bone, int frameID, int& count, keyFrame& animationFrame, gltf::AnimationSampler& sampler)
{
	const auto source = resolveAccessor(model, sampler.input);
	const auto& input = source.accessor;
	requireFrame(input, frameID);
	count					 = static_cast<int>(input.count);
	if(input.type != TG3_TYPE_SCALAR)
		throw LoadingError("glTF animation time accessor must be scalar");
	const auto* frameData = source.data + static_cast<std::size_t>(frameID) * source.stride;
	float data = 0.0f;
	if(input.componentType == TG3_COMPONENT_TYPE_FLOAT) { memcpy(&data, frameData, sizeof(data)); }
	else if(input.componentType == TG3_COMPONENT_TYPE_DOUBLE)
	{
		double value = 0.0;
		memcpy(&value, frameData, sizeof(value));
		data = static_cast<float>(value);
	}
	else throw LoadingError("Unsupported glTF animation time component type");

	if(animationFrame.timePoint < 0)
		animationFrame.timePoint = data;
	else if(animationFrame.timePoint != data)
	{
		throw FileIOError("Mismatch of timecode while loading an animation keyframe for bone joint " + std::to_string(bone)
								 + "\n"
								   "read from file : "
								 + std::to_string(data) + " while animationFrame recorded " + std::to_string(animationFrame.timePoint));
	}
}

void skeletonImporter::loadVector3FromSampler(int frameID, int& count, gltf::AnimationSampler& sampler, Ogre::Vector3& vector)
{
	const auto source = resolveAccessor(model, sampler.output);
	const auto& output = source.accessor;
	requireFrame(output, frameID);
	count					 = static_cast<int>(output.count);
	if(output.type != TG3_TYPE_VEC3)
		throw LoadingError("glTF animation vector accessor must be VEC3");
	const auto* frameData = source.data + static_cast<std::size_t>(frameID) * source.stride;
	if(output.componentType == TG3_COMPONENT_TYPE_FLOAT)
	{
		std::array<float, 3> values {};
		memcpy(values.data(), frameData, sizeof(values));
		vector = Ogre::Vector3(values.data());
	}
	else if(output.componentType == TG3_COMPONENT_TYPE_DOUBLE) //need double to float conversion
	{
		std::array<Ogre::Real, 3> vectFloat {};
		std::array<double, 3> vectDouble {};

		memcpy(vectDouble.data(), frameData, sizeof(vectDouble));
		internal_utils::container_double_to_real(vectDouble, vectFloat);

		vector = Ogre::Vector3(vectFloat.data());
	}
	else throw LoadingError("Unsupported glTF animation vector component type");
}

void skeletonImporter::loadQuatFromSampler(int frameID, int& count, gltf::AnimationSampler& sampler, Ogre::Quaternion& quat) const
{
	const auto source = resolveAccessor(model, sampler.output);
	const auto& output = source.accessor;
	requireFrame(output, frameID);
	count					 = static_cast<int>(output.count);
	if(output.type != TG3_TYPE_VEC4)
		throw LoadingError("glTF animation rotation accessor must be VEC4");
	const auto* frameData = source.data + static_cast<std::size_t>(frameID) * source.stride;

	if(output.componentType == TG3_COMPONENT_TYPE_FLOAT)
	{
		std::array<float, 4> values {};
		memcpy(values.data(), frameData, sizeof(values));
		quat = Ogre::Quaternion(values[3], values[0], values[1], values[2]);
	}
	else if(output.componentType == TG3_COMPONENT_TYPE_DOUBLE) //need double to float conversion
	{
		std::array<Ogre::Real, 4> vectFloat {};
		std::array<double, 4> vectDouble {};

		memcpy(vectDouble.data(), frameData, sizeof(vectDouble));
		internal_utils::container_double_to_real(vectDouble, vectFloat);

		quat = Ogre::Quaternion(vectFloat[3], vectFloat[0], vectFloat[1], vectFloat[2]);
	}
	else throw LoadingError("Unsupported glTF animation rotation component type");
}

void skeletonImporter::detectAnimationChannel(const channelList& channels,
											  gltf::AnimationChannel*& translation,
											  gltf::AnimationChannel*& rotation,
											  gltf::AnimationChannel*& scale,
											  gltf::AnimationChannel*& weights) const
{
	const auto translationIt
		= std::find_if(channels.begin(), channels.end(), [](const gltf::AnimationChannel& c) { return c.target_path == "translation"; });
	if(translationIt != channels.end()) translation = &(*translationIt).get();

	const auto rotationIt = std::find_if(channels.begin(), channels.end(), [](const gltf::AnimationChannel& c) { return c.target_path == "rotation"; });
	if(rotationIt != channels.end()) rotation = &(*rotationIt).get();

	const auto scaleIt = std::find_if(channels.begin(), channels.end(), [](const gltf::AnimationChannel& c) { return c.target_path == "scale"; });
	if(scaleIt != channels.end()) scale = &(*scaleIt).get();

	const auto weightsIt = std::find_if(channels.begin(), channels.end(), [](const gltf::AnimationChannel& c) { return c.target_path == "weights"; });
	if(weightsIt != channels.end()) weights = &(*weightsIt).get();
}

void skeletonImporter::loadKeyFrameDataFromSampler(const gltf::Animation& animation,
												   int bone,
												   gltf::AnimationChannel* translation,
												   gltf::AnimationChannel* rotation,
												   gltf::AnimationChannel* scale,
												   gltf::AnimationChannel* weights,
												   int frameID,
												   int& count,
												   keyFrame& animationFrame)
{
	if(translation)
	{
		auto sampler = animation.samplers[translation->sampler];
		loadTimepointFromSamplerToKeyFrame(bone, frameID, count, animationFrame, sampler);
		loadVector3FromSampler(frameID, count, sampler, animationFrame.position);
	}
	if(rotation)
	{
		auto sampler = animation.samplers[rotation->sampler];
		loadTimepointFromSamplerToKeyFrame(bone, frameID, count, animationFrame, sampler);
		loadQuatFromSampler(frameID, count, sampler, animationFrame.rotation);
	}
	if(scale)
	{
		auto sampler = animation.samplers[scale->sampler];
		loadTimepointFromSamplerToKeyFrame(bone, frameID, count, animationFrame, sampler);
		loadVector3FromSampler(frameID, count, sampler, animationFrame.scale);
	}
	if(weights)
	{
		auto sampler = animation.samplers[weights->sampler];
		loadTimepointFromSamplerToKeyFrame(bone, frameID, count, animationFrame, sampler);
		//TODO load the scalar... but well, we don't do anything with that in a skeletal animation, so... do nothing
	}
}

void skeletonImporter::loadKeyFrames(const gltf::Animation& animation,
									 int bone,
									 keyFrameList& keyFrames,
									 gltf::AnimationChannel* translation,
									 gltf::AnimationChannel* rotation,
									 gltf::AnimationChannel* scale,
									 gltf::AnimationChannel* weights)
{
	bool endOfTimeLine = false;
	int frameID		   = 0;
	int count		   = 0;
	while(!endOfTimeLine)
	{
		keyFrame animationFrame;

		loadKeyFrameDataFromSampler(animation, bone, translation, rotation, scale, weights, frameID, count, animationFrame);

		keyFrames.push_back(animationFrame);
		++frameID;
		if(frameID >= count) endOfTimeLine = true;
	}
}

void skeletonImporter::loadSkeletonAnimations(const gltf::Skin skin, const std::string& skeletonName)
{
	//List all the animations that own at least one channel that target one of the bones of our skeleton
	OgreLog("Searching for animations for skeleton " + skeleton->getName());
	std::vector<std::reference_wrapper<gltf::Animation>> animations;
	for(auto& animation : model.animations)
	{
		for(const auto& channel : animation.channels)
		{
			if(std::find(skin.joints.begin(), skin.joints.end(), channel.target_node) != skin.joints.end())
			{
				//animation is targeting our skeleton, just save that information
				animations.emplace_back(animation);
				break;
			}
		}
	}

	std::unordered_map<tinygltfJointNodeIndex, keyFrameList> boneIndexedKeyFrames;

	const auto getAnimationLength = [](const keyFrameList& l) {
		if(l.empty()) return 0.0f;
		return l.back().timePoint;
	};

	std::unordered_map<tinygltfJointNodeIndex, channelList> boneRawAnimationChannels;

	if(!animations.empty())
	{
		int i = 0;
		for(auto animation_rw : animations)
		{
			//Get animation
			auto animation			  = animation_rw.get();
			std::string animationName = animation.name;
			if(animation.name.empty()) animationName = skeletonName + "Animation" + std::to_string(i++);

			OgreLog("parsing channels for animation " + animationName);

			float maxLen = 0;
			for(auto& channel : animation.channels)
			{
				const auto joint					   = nodeToJointMap[channel.target_node];
				const auto& boneRawAnimationChannelIt = boneRawAnimationChannels.find(joint);
				if(boneRawAnimationChannelIt == boneRawAnimationChannels.end()) boneRawAnimationChannels[joint];

				boneRawAnimationChannels[joint].push_back(channel);
			}

			//from here, bones -> channel map has been built;
			for(auto& boneChannels : boneRawAnimationChannels)
			{
				auto bone	 = boneChannels.first;
				auto channels = boneChannels.second;

				keyFrameList keyFrames;

				gltf::AnimationChannel* translation = nullptr;
				gltf::AnimationChannel* rotation	= nullptr;
				gltf::AnimationChannel* scale		= nullptr;
				gltf::AnimationChannel* weights		= nullptr;

				detectAnimationChannel(channels, translation, rotation, scale, weights);
				loadKeyFrames(animation, bone, keyFrames, translation, rotation, scale, weights);

				//here, we have a list of all key frames for one bone
				boneIndexedKeyFrames[bone] = keyFrames;
				maxLen					   = getAnimationLength(keyFrames);
			}

			//Create animation
			auto ogreAnimation = skeleton->createAnimation(animationName, maxLen);
			ogreAnimation->setInterpolationMode(Ogre::v1::Animation::InterpolationMode::IM_LINEAR);

			//For each bone's list of keyframes
			for(auto& keyFrameForBone : boneIndexedKeyFrames)
			{
				//Get the bone index
				const auto boneIndex	 = keyFrameForBone.first;
				const auto ogreBoneIndex = boneIndex;

				//Add a node to the animation track
				auto nodeAnimTrack = ogreAnimation->createOldNodeTrack(ogreBoneIndex);
				auto bone		   = skeleton->getBone(boneIndex);

				//for each keyframe
				for(auto& keyFrame : keyFrameForBone.second)
				{
					//Add a transform to apply
					Ogre::v1::TransformKeyFrame* transformKeyFrame = nodeAnimTrack->createNodeKeyFrame(keyFrame.timePoint);

					//Set the data
					transformKeyFrame->setRotation(bone->getOrientation().Inverse() * keyFrame.rotation);
					transformKeyFrame->setTranslate(bone->getPosition() - keyFrame.position);
					transformKeyFrame->setScale(keyFrame.scale / bone->getScale());
				}
			}

			//Need to use the keyframe 0 as base keyframe
		}
	}
}

void recurse(const gltf::Model& m, int node, std::vector<int>& output)
{
	output.push_back(node);
	for(auto child : m.nodes[node].children) { recurse(m, child, output); }
}

std::vector<int> traversal(const gltf::Model& m, int node)
{
	std::vector<int> o;
	recurse(m, node, o);
	return o;
}

Ogre::v1::SkeletonPtr skeletonImporter::getSkeleton(size_t index)
{
	if(index >= model.skins.size()) throw LoadingError("glTF skin index out of range: " + std::to_string(index));
	const auto& skin = model.skins[index];

	const std::string skeletonName = "glTF_skeleton_" + std::to_string(importId) + "_" + std::to_string(index);
	OgreLog("Loading skin " + skin.name + " as " + skeletonName);

	//Get skeleton
	skeleton = Ogre::v1::OldSkeletonManager::getSingleton().getByName(skeletonName);
	if(skeleton)
	{
		//OgreLog("Found in the skeleton manager");
		return skeleton;
	}

	//Create new skeleton
	createdSkeletons.reserve(createdSkeletons.size() + 1u);
	const size_t createdCount = createdSkeletons.size();
	try
	{
	skeleton = Ogre::v1::OldSkeletonManager::getSingleton().create(skeletonName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

	if(!skeleton) throw InitError("Couldn't create skeletion for skin" + skeletonName);
	createdSkeletons.push_back(skeleton);
	bindMatrices.clear();
	nodeToJointMap.clear();

	//OgreLog("skin.skeleton = " + std::to_string(skin.skeleton));
	//OgreLog("first joint : " + std::to_string(skin.joints.front()));
	if(skin.inverseBindMatrices == -1)
	{
		bindMatrices.assign(skin.joints.size(), Ogre::Matrix4::IDENTITY);
	}
	else
	{
		const auto source = resolveAccessor(model, skin.inverseBindMatrices);
		const auto& inverseBindMatricesAccessor = source.accessor;
		if(inverseBindMatricesAccessor.count < skin.joints.size() ||
		   inverseBindMatricesAccessor.type != TG3_TYPE_MAT4 ||
		   (inverseBindMatricesAccessor.componentType != TG3_COMPONENT_TYPE_FLOAT &&
		    inverseBindMatricesAccessor.componentType != TG3_COMPONENT_TYPE_DOUBLE))
			throw LoadingError("glTF inverse bind matrices must contain a floating-point MAT4 for every joint");

		std::array<Ogre::Real, 4 * 4> floatMatrix {};

		for(std::size_t i = 0; i < skin.joints.size(); ++i)
		{
			const unsigned char* frameData = source.data + i * source.stride;
			if(inverseBindMatricesAccessor.componentType == TG3_COMPONENT_TYPE_FLOAT)
			{
				std::array<float, 4 * 4> sourceMatrix {};
				memcpy(sourceMatrix.data(), frameData, sizeof(sourceMatrix));
				std::transform(sourceMatrix.begin(), sourceMatrix.end(), floatMatrix.begin(),
					[](float value) { return static_cast<Ogre::Real>(value); });
			}
			else if(inverseBindMatricesAccessor.componentType == TG3_COMPONENT_TYPE_DOUBLE)
			{
				//Needs to do Double -> Float conversion
				std::array<double, 4 * 4> doubleMatrix {};
				memcpy(doubleMatrix.data(), frameData, sizeof(doubleMatrix));
				internal_utils::container_double_to_real(doubleMatrix, floatMatrix);
			}

			Ogre::Matrix4 inverseBindMatrixTransposed = Ogre::Matrix4(floatMatrix[0],
													 floatMatrix[1],
													 floatMatrix[2],
													 floatMatrix[3],
													 floatMatrix[4],
													 floatMatrix[5],
													 floatMatrix[6],
													 floatMatrix[7],
													 floatMatrix[8],
													 floatMatrix[9],
													 floatMatrix[10],
													 floatMatrix[11],
													 floatMatrix[12],
													 floatMatrix[13],
													 floatMatrix[14],
													 floatMatrix[15]);

			assert(inverseBindMatrixTransposed.transpose().isAffine());
			bindMatrices.push_back(inverseBindMatrixTransposed.transpose().inverseAffine());
		}
	}

	std::vector<int> rootBones;
	std::vector<int> allChildren;
	for(const auto& nodeIndex : skin.joints)
	{
		const auto& node = model.nodes[nodeIndex];
		allChildren.insert(allChildren.end(), node.children.begin(), node.children.end());
	}

	//Build the "node to joint map". In the vertex buffer, property "JOINT_0" refer to the joints that affect a particular vertex of the skined mesh.
	//To refer to theses joints, it refer to the index of the node in the skin.joints array.
	//We need to be able to get the index for each of theses joints in the array easilly, so we are builind a dictionarry to be able to reverse-search them
	for(int i = 0; i < skin.joints.size(); ++i)
	{
		//Get the index in the "node" array in the glTF's JSON
		const auto jointNode = skin.joints[i];

		//Record in the dictionary the joint node-> index
		nodeToJointMap[jointNode] = i;

		//Get the name (if possible)
		const auto name = model.nodes[jointNode].name;

		//Create bone with index "i"
		auto bone = skeleton->createBone(!name.empty() ? name : skeletonName + std::to_string(i), i);

		if(std::find(allChildren.begin(), allChildren.end(), jointNode) == allChildren.end()) {
			rootBones.push_back(jointNode);
		}
	}

	for(int boneIndex : rootBones)
	{
		loadBoneHierarchy(boneIndex);
	}
	skeleton->setBindingPose();
	loadSkeletonAnimations(skin, skeletonName);

	return skeleton;
	}
	catch(...)
	{
		releaseCreatedSkeletonsSince(createdCount);
		throw;
	}
}

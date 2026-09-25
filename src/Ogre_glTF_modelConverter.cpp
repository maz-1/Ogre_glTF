#include "Ogre_glTF_modelConverter.hpp"
#include "Ogre_glTF_common.hpp"
#include <OgreMesh2.h>
#include <OgreMeshManager2.h>
#include <OgreSubMesh2.h>
#include "Ogre_glTF_internal_utils.hpp"


using namespace Ogre_glTF;

namespace
{
struct AccessorData
{
	const gltf::Accessor& accessor;
	const gltf::BufferView& view;
	gltf::Buffer& buffer;
	std::size_t byteOffset;
	std::size_t byteStride;
};

AccessorData resolveAccessor(gltf::Model& model, int accessorIndex)
{
	if(accessorIndex < 0 || static_cast<std::size_t>(accessorIndex) >= model.accessors.size())
		throw LoadingError("glTF accessor index is out of range");
	const auto& accessor = model.accessors[accessorIndex];
	if(accessor.isSparse)
		throw LoadingError("Sparse glTF mesh accessors are not supported");
	if(accessor.bufferView < 0 || static_cast<std::size_t>(accessor.bufferView) >= model.bufferViews.size())
		throw LoadingError("glTF accessor has no valid bufferView");
	const auto& view = model.bufferViews[accessor.bufferView];
	if(view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= model.buffers.size())
		throw LoadingError("glTF bufferView has no valid buffer");
	auto& buffer = model.buffers[view.buffer];
	const int componentSize = tg3_component_size(accessor.componentType);
	const int componentCount = tg3_num_components(accessor.type);
	const int stride = accessor.ByteStride(view);
	if(componentSize <= 0 || componentCount <= 0 || stride <= 0 ||
	   static_cast<std::size_t>(stride) < static_cast<std::size_t>(componentSize) * componentCount)
		throw LoadingError("glTF accessor has an invalid component type or stride");
	if(view.byteOffset > buffer.data.size() || view.byteLength > buffer.data.size() - view.byteOffset ||
	   accessor.byteOffset > view.byteLength)
		throw LoadingError("glTF accessor byte range is outside its buffer");
	const auto elementSize = static_cast<std::size_t>(componentSize) * componentCount;
	const auto available = view.byteLength - accessor.byteOffset;
	if(accessor.count > 0 &&
	   (elementSize > available ||
	    accessor.count - 1 > (available - elementSize) / static_cast<std::size_t>(stride)))
		throw LoadingError("glTF accessor elements exceed their bufferView");
	return { accessor, view, buffer, view.byteOffset + accessor.byteOffset,
		static_cast<std::size_t>(stride) };
}
}

size_t vertexBufferPart::getPartStride() const { return buffer->elementSize() * perVertex; }

modelConverter::modelConverter(gltf::Model& input, size_t importId) : model { input }, importId { importId } {}

void modelConverter::releaseCreatedMeshesSince(size_t keepCount)
{
	auto* meshManager = Ogre::MeshManager::getSingletonPtr();
	while(createdMeshes.size() > keepCount)
	{
		const auto& mesh = createdMeshes.back();
		if(mesh && meshManager && meshManager->resourceExists(mesh->getName())) meshManager->remove(mesh->getName());
		createdMeshes.pop_back();
	}
}

Ogre::VertexBufferPackedVec modelConverter::constructVertexBuffer(const std::vector<vertexBufferPart>& parts) const
{
	Ogre::VertexElement2Vec vertexElements;

	size_t stride { 0 }, strideInElements { 0 };
	size_t vertexCount { 0 }, previousVertexCount { 0 };

	for(const auto& part : parts)
	{
		vertexElements.emplace_back(part.type, part.semantic);
		strideInElements += part.perVertex;
		stride += part.buffer->elementSize() * part.perVertex;
		vertexCount = part.vertexCount;

		//Sanity check
		if(previousVertexCount != 0)
		{
			if(vertexCount != previousVertexCount) throw LoadingError("Part of vertex buffer for the same primitive have different vertex counts!");
		}
		else
			previousVertexCount = vertexCount;
	}

	OgreLog("There will be " + std::to_string(vertexCount) + " vertices with a stride of " + std::to_string(stride) + " bytes");

	geometryBuffer<float> finalBuffer(vertexCount * strideInElements);
	size_t bytesWrittenInCurrentStride { 0 };
	for(size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
	{
		bytesWrittenInCurrentStride = 0;
		for(const auto& part : parts)
		{
			memcpy(finalBuffer.dataAddress() + (bytesWrittenInCurrentStride + vertexIndex * stride),
				   (part.buffer->dataAddress() + (vertexIndex * part.getPartStride())),
				   part.getPartStride());
			bytesWrittenInCurrentStride += part.getPartStride();
		}
	}

	Ogre::VertexBufferPackedVec vec;
	auto vertexBuffer = getVaoManager()->createVertexBuffer(vertexElements, vertexCount, Ogre::BT_IMMUTABLE, finalBuffer.data(), false);

	vec.push_back(vertexBuffer);
	return vec;
}

Ogre::MeshPtr modelConverter::getOgreMesh(const Ogre::String& name)
{
	if(name.empty())
	{
		if(!model.meshes.empty()) {
			return getOgreMesh(0);
		}
		else
		{
			return Ogre::MeshPtr();
		}
	}

	for(size_t meshIdx = 0; meshIdx < model.meshes.size(); ++meshIdx) {
		const auto& mesh = model.meshes[meshIdx];
		if(!mesh.name.empty() && mesh.name == name) {
			return getOgreMesh(meshIdx);
		}
	}
	
	return Ogre::MeshPtr();
}

Ogre::MeshPtr modelConverter::getOgreMesh(size_t meshIdx)
{
	if(meshIdx >= model.meshes.size()) throw LoadingError("glTF mesh index out of range: " + std::to_string(meshIdx));

	Ogre::Aabb boundingBox;
	auto& mesh = model.meshes[meshIdx];
	OgreLog("Found mesh " + mesh.name + " in glTF file");
	const Ogre::String resourceName = "glTF_mesh_" + std::to_string(importId) + "_" + std::to_string(meshIdx);

	auto ogreMesh = Ogre::MeshManager::getSingleton().getByName(resourceName);
	if(ogreMesh)
	{
		OgreLog("Found mesh " + resourceName + " in Ogre::MeshManager(v2)");
		return ogreMesh;
	}

	OgreLog("Loading mesh from glTF file");
	OgreLog("mesh has " + std::to_string(mesh.primitives.size()) + " primitives");
	createdMeshes.reserve(createdMeshes.size() + 1u);
	const size_t createdCount = createdMeshes.size();
	try
	{
	ogreMesh = Ogre::MeshManager::getSingleton().createManual(resourceName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
	createdMeshes.push_back(ogreMesh);
	OgreLog("Created mesh on v2 MeshManager");

	for(const auto& primitive : mesh.primitives)
	{
		auto subMesh = ogreMesh->createSubMesh();
		OgreLog("Created one submesh");
		const auto indexBuffer = extractIndexBuffer(primitive.indices);

		std::vector<vertexBufferPart> parts;
		//OgreLog("\tprimitive has : " + std::to_string(primitive.attributes.size()) + " atributes");
		for(const auto& atribute : primitive.attributes)
		{
			OgreLog("\t " + atribute.first);
			parts.push_back(std::move(extractVertexBuffer(atribute, boundingBox)));
		}

		//Get (if they exist) the blend weights and bone index parts of our vertex array object content
		const auto blendIndicesIt = std::find_if(std::begin(parts), std::end(parts), [](const vertexBufferPart& vertexBufferPart) {
			return (vertexBufferPart.semantic == Ogre::VertexElementSemantic::VES_BLEND_INDICES);
		});

		const auto blendWeightsIt = std::find_if(std::begin(parts), std::end(parts), [](const vertexBufferPart& vertexBufferPart) {
			return (vertexBufferPart.semantic == Ogre::VertexElementSemantic::VES_BLEND_WEIGHTS);
		});

		const auto vertexBuffers = constructVertexBuffer(parts);
		auto vao				 = getVaoManager()->createVertexArrayObject(vertexBuffers, indexBuffer, [&]() -> Ogre::OperationType {
			switch(primitive.mode)
			{
				case TG3_MODE_LINE: OgreLog("Line List"); return Ogre::OT_LINE_LIST;
				case TG3_MODE_LINE_LOOP: OgreLog("Line Loop"); return Ogre::OT_LINE_STRIP;
				case TG3_MODE_POINTS: OgreLog("Points"); return Ogre::OT_POINT_LIST;
				case TG3_MODE_TRIANGLES: OgreLog("Triangle List"); return Ogre::OT_TRIANGLE_LIST;
				case TG3_MODE_TRIANGLE_FAN: OgreLog("Trinagle Fan"); return Ogre::OT_TRIANGLE_FAN;
				case TG3_MODE_TRIANGLE_STRIP: OgreLog("Triangle Strip"); return Ogre::OT_TRIANGLE_STRIP;
				default: OgreLog("Unknown"); throw LoadingError("Can't understand primitive mode!");
			};
		}());

		subMesh->mVao[Ogre::VpNormal].push_back(vao);
		subMesh->mVao[Ogre::VpShadow].push_back(vao);

		if(blendIndicesIt != std::end(parts) && blendWeightsIt != std::end(parts))
		{
			//subMesh->_buildBoneAssignmentsFromVertexData();

			//Get the vertexBufferParts from the two iterators
			//OgreLog("The vertex buffer contains blend weights and indices information!");
			vertexBufferPart& blendIndices = *blendIndicesIt;
			vertexBufferPart& blendWeights = *blendWeightsIt;

			//Debug sanity check, both should be equals
			//OgreLog("Vertex count blendIndex : " + std::to_string(blendIndices.vertexCount));
			//OgreLog("Vertex count blendWeight: " + std::to_string(blendWeights.vertexCount));
			//OgreLog("Vertex element count blendIndex : " + std::to_string(blendIndices.perVertex));
			//OgreLog("Vertex element count blendWeight: " + std::to_string(blendWeights.perVertex));

			//Allocate 2 small arrays to store the bone idexes. (They should be of lenght "4")
			std::vector<Ogre::ushort> vertexBoneIndex(blendIndices.perVertex);
			std::vector<Ogre::Real> vertexBlend(blendWeights.perVertex);

			//Add the attahcments for each bones
			for(Ogre::uint32 vertexIndex = 0; vertexIndex < blendIndices.vertexCount; ++vertexIndex)
			{
				//Fetch the for bone indexes from the buffer
				memcpy(vertexBoneIndex.data(),
					   blendIndices.buffer->dataAddress() + (blendIndices.getPartStride() * vertexIndex),
					   blendIndices.perVertex * sizeof(Ogre::ushort));

				//Fetch the for weights from the buffer
				memcpy(vertexBlend.data(),
					   blendWeights.buffer->dataAddress() + (blendWeights.getPartStride() * vertexIndex),
					   blendWeights.perVertex * sizeof(Ogre::Real));

				//Add the bone assignments to the submesh
				for(size_t i = 0; i < blendIndices.perVertex; ++i)
				{
					auto vba = Ogre::VertexBoneAssignment(vertexIndex, vertexBoneIndex[i], vertexBlend[i]);

					//OgreLog("VertexBoneAssignment: " + std::to_string(i) + " over " + std::to_string(blendIndices.perVertex));
					//OgreLog(std::to_string(vba.vertexIndex));
					//OgreLog(std::to_string(vba.boneIndex));
					//OgreLog(std::to_string(vba.weight));

					subMesh->addBoneAssignment(vba);
				}
			}

			//subMesh->_buildBoneIndexMap();
			subMesh->_compileBoneAssignments();
		}
	}

	ogreMesh->_setBounds(boundingBox, true);
	//OgreLog("Setting 'bounding sphere radius' from bounds : " + std::to_string(boundingBox.getRadius()));

	return ogreMesh;
	}
	catch(...)
	{
		releaseCreatedMeshesSince(createdCount);
		throw;
	}
}

void modelConverter::debugDump() const
{
	std::stringstream gltfContentDump;
	gltfContentDump << "This glTF model has:\n"
					<< model.accessors.size() << " accessors\n"
					<< model.animations.size() << " animations\n"
					<< model.buffers.size() << " buffers\n"
					<< model.bufferViews.size() << " bufferViews\n"
					<< model.materials.size() << " materials\n"
					<< model.meshes.size() << " meshes\n"
					<< model.nodes.size() << " nodes\n"
					<< model.textures.size() << " textures\n"
					<< model.images.size() << " images\n"
					<< model.skins.size() << " skins\n"
					<< model.samplers.size() << " samplers\n"
					<< model.cameras.size() << " cameras\n"
					<< model.scenes.size() << " scenes\n"
					<< model.lights.size() << " lights\n";

	OgreLog(gltfContentDump);
}

bool modelConverter::hasSkins() const { return !model.skins.empty(); }

Ogre::VaoManager* modelConverter::getVaoManager()
{
	//Our class shouldn't be able to exist if Ogre hasn't been initalized with a valid render system. This call should allways succeed.
	return Ogre::Root::getSingletonPtr()->getRenderSystem()->getVaoManager();
}

Ogre::IndexBufferPacked* modelConverter::extractIndexBuffer(int accessorID) const
{
	OgreLog("Extracting index buffer");
	if(accessorID == -1) return nullptr;
	const auto source = resolveAccessor(model, accessorID);
	const auto& accessor = source.accessor;
	auto& buffer = source.buffer;
	const auto byteStride = source.byteStride;
	const auto indexCount  = accessor.count;
	Ogre::IndexBufferPacked::IndexType type;

	auto convertTo16Bit { false };
	switch(accessor.componentType)
	{
		default: throw LoadingError("Unrecognized index data format");
		case TG3_COMPONENT_TYPE_BYTE:
		case TG3_COMPONENT_TYPE_UNSIGNED_BYTE: convertTo16Bit = true;
		case TG3_COMPONENT_TYPE_SHORT:
		case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
		{
			type			= Ogre::IndexBufferPacked::IT_16BIT;
			auto geomBuffer = geometryBuffer<Ogre::uint16>(indexCount);
			if(convertTo16Bit)
				loadIndexBuffer(geomBuffer.data(), buffer.data.data(), indexCount, source.byteOffset, byteStride);
			else
				loadIndexBuffer(geomBuffer.data(),
								reinterpret_cast<Ogre::uint16*>(buffer.data.data()),
								indexCount,
								source.byteOffset,
								byteStride);
			return getVaoManager()->createIndexBuffer(type, indexCount, Ogre::BT_IMMUTABLE, geomBuffer.dataAddress(), false);
		}
		case TG3_COMPONENT_TYPE_INT:;
		case TG3_COMPONENT_TYPE_UNSIGNED_INT:
		{
			type			= Ogre::IndexBufferPacked::IT_32BIT;
			auto geomBuffer = geometryBuffer<Ogre::uint32>(indexCount);
			loadIndexBuffer(
				geomBuffer.data(), reinterpret_cast<Ogre::uint32*>(buffer.data.data()), indexCount, source.byteOffset, byteStride);
			return getVaoManager()->createIndexBuffer(type, indexCount, Ogre::BT_IMMUTABLE, geomBuffer.dataAddress(), false);
		}
	}
}

size_t modelConverter::getVertexBufferElementsPerVertexCount(int type)
{
	switch(type)
	{
		case TG3_TYPE_VEC2: return 2;
		case TG3_TYPE_VEC3: return 3;
		case TG3_TYPE_VEC4: return 4;
		default: return 0;
	}
}

Ogre::VertexElementSemantic modelConverter::getVertexElementScemantic(const std::string& type)
{
	OgreLog("type: " + type);
	if(type == "POSITION") return Ogre::VES_POSITION;
	if(type == "NORMAL") return Ogre::VES_NORMAL;
	if(type == "TANGENT") return Ogre::VES_TANGENT;
	if(type == "TEXCOORD_0") return Ogre::VES_TEXTURE_COORDINATES;
	if(type == "TEXCOORD_1") return Ogre::VES_TEXTURE_COORDINATES;
	if(type == "COLOR_0") return Ogre::VES_DIFFUSE;
	if(type == "JOINTS_0") return Ogre::VES_BLEND_INDICES;
	if(type == "WEIGHTS_0") return Ogre::VES_BLEND_WEIGHTS;
	return Ogre::VES_COUNT; //Returning this means returning "invalid" here
}

vertexBufferPart modelConverter::extractVertexBuffer(const std::pair<std::string, int>& attribute, Ogre::Aabb& boundingBox) const
{
	const auto elementScemantic			= getVertexElementScemantic(attribute.first);
	const auto source                   = resolveAccessor(model, attribute.second);
	const auto& accessor                = source.accessor;
	const auto& bufferView              = source.view;
	const auto& buffer                  = source.buffer;
	const auto vertexBufferByteLen		= bufferView.byteLength;
	const auto numberOfElementPerVertex = getVertexBufferElementsPerVertexCount(accessor.type);
	const auto elementOffsetInBuffer	= source.byteOffset;
	size_t bufferLenghtInBufferBasicType { 0 };
	if(numberOfElementPerVertex == 0)
		throw LoadingError("Unsupported glTF mesh accessor element type");

	std::unique_ptr<geometryBuffer_base> geomBuffer { nullptr };
	Ogre::VertexElementType elementType {};

	switch(accessor.componentType)
	{
		case TG3_COMPONENT_TYPE_DOUBLE: throw LoadingError("Double precision not implemented!");
		case TG3_COMPONENT_TYPE_FLOAT:
			bufferLenghtInBufferBasicType = (vertexBufferByteLen / sizeof(float));
			geomBuffer					  = std::make_unique<geometryBuffer<float>>(bufferLenghtInBufferBasicType);
			if(numberOfElementPerVertex == 2) elementType = Ogre::VET_FLOAT2;
			if(numberOfElementPerVertex == 3) elementType = Ogre::VET_FLOAT3;
			if(numberOfElementPerVertex == 4) elementType = Ogre::VET_FLOAT4;
			break;
		case TG3_COMPONENT_TYPE_UNSIGNED_SHORT:
			bufferLenghtInBufferBasicType = (vertexBufferByteLen / sizeof(unsigned short));
			geomBuffer					  = std::make_unique<geometryBuffer<unsigned short>>(bufferLenghtInBufferBasicType);
			if(numberOfElementPerVertex == 2) elementType = Ogre::VET_USHORT2;
			if(numberOfElementPerVertex == 4) elementType = Ogre::VET_USHORT4;
			break;
		default: throw LoadingError("Unrecognized vertex buffer coponent type");
	}

	//if(bufferView.byteStride == 0)
	//	OgreLog("Vertex buffer is 'tightly packed'");

	const auto byteStride				  = source.byteStride;
	const auto vertexCount				  = accessor.count;
	const auto vertexElementLenghtInBytes = numberOfElementPerVertex * geomBuffer->elementSize();

	//OgreLog("A vertex element on this buffer is " + std::to_string(vertexElementLenghtInBytes) + " bytes long");
	for(size_t vertexIndex = 0; vertexIndex < vertexCount; vertexIndex++)
	{
		const auto destOffset   = vertexIndex * vertexElementLenghtInBytes;
		const auto sourceOffset = elementOffsetInBuffer + vertexIndex * byteStride;

		memcpy((geomBuffer->dataAddress() + destOffset), (buffer.data.data() + sourceOffset), vertexElementLenghtInBytes);
	}

	//Update the bounding sizes once, when vertex positions has been read.
	if(elementScemantic == Ogre::VES_POSITION)
	{
		//Convert to float and load into Ogre::Vector3 objects
		std::array<Ogre::Real, 3> floatVector {};
		internal_utils::container_double_to_real(accessor.minValues, floatVector);
		const Ogre::Vector3 minBounds { floatVector.data() };
		internal_utils::container_double_to_real(accessor.maxValues, floatVector);
		const Ogre::Vector3 maxBounds { floatVector.data() };

		OgreLog("Updating bounding box size: ");
		OgreLog("Setting Min size: " + std::to_string(minBounds.x) + " " + std::to_string(minBounds.y) + " " + std::to_string(minBounds.z));
		OgreLog("Setting Max size: " + std::to_string(maxBounds.x) + " " + std::to_string(maxBounds.y) + " " + std::to_string(maxBounds.z));
		boundingBox.merge(Ogre::Aabb::newFromExtents(minBounds, maxBounds));
	}

	//geometryBuffer->_debugContentToLog();
	return { std::move(geomBuffer), elementType, elementScemantic, vertexCount, numberOfElementPerVertex };
}

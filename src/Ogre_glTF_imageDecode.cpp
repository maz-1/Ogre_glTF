#include "Ogre_glTF_imageDecode.hpp"

#include "Ogre_glTF_gltfModel.hpp"
#include "Ogre_glTF.hpp"

#include <ScopeExit/ScopeExit.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../thirdParty/tinygltf/attic/stb_image.h"

namespace Ogre_glTF
{
namespace gltf
{
namespace
{

constexpr std::size_t maxDecodedImageBytes = std::size_t(512) * 1024 * 1024;

[[noreturn]] void imageError(std::size_t index, const std::string& reason)
{
	throw LoadingError("glTF image[" + std::to_string(index) + "]: " + reason);
}

int hexDigit(unsigned char value)
{
	if(value >= '0' && value <= '9') return value - '0';
	if(value >= 'a' && value <= 'f') return value - 'a' + 10;
	if(value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

std::vector<unsigned char> percentDecode(const std::string& encoded, std::size_t index)
{
	std::vector<unsigned char> result;
	result.reserve(encoded.size());
	for(std::size_t pos = 0; pos < encoded.size(); ++pos) {
		const auto value = static_cast<unsigned char>(encoded[pos]);
		if(value != '%') {
			result.push_back(value);
			continue;
		}
		if(pos + 2 >= encoded.size()) imageError(index, "incomplete URI escape");
		const int high = hexDigit(static_cast<unsigned char>(encoded[pos + 1]));
		const int low = hexDigit(static_cast<unsigned char>(encoded[pos + 2]));
		if(high < 0 || low < 0) imageError(index, "invalid URI escape");
		result.push_back(static_cast<unsigned char>((high << 4) | low));
		pos += 2;
	}
	return result;
}

int base64Digit(unsigned char value)
{
	if(value >= 'A' && value <= 'Z') return value - 'A';
	if(value >= 'a' && value <= 'z') return value - 'a' + 26;
	if(value >= '0' && value <= '9') return value - '0' + 52;
	if(value == '+') return 62;
	if(value == '/') return 63;
	return -1;
}

std::vector<unsigned char> base64Decode(const std::vector<unsigned char>& encoded, std::size_t index)
{
	std::vector<unsigned char> result;
	result.reserve(encoded.size() / 4 * 3 + 3);
	std::uint32_t pending = 0;
	int pendingBits = 0;
	std::size_t digits = 0;
	std::size_t padding = 0;
	for(const auto value : encoded) {
		if(value == '=') {
			++padding;
			continue;
		}
		const int digit = base64Digit(value);
		if(digit < 0 || padding != 0) imageError(index, "invalid base64 image URI");
		pending = (pending << 6) | static_cast<std::uint32_t>(digit);
		pendingBits += 6;
		++digits;
		if(pendingBits >= 8) {
			pendingBits -= 8;
			result.push_back(static_cast<unsigned char>((pending >> pendingBits) & 0xff));
			pending &= (std::uint32_t(1) << pendingBits) - 1;
		}
	}
	if(digits % 4 == 1 || padding > 2 ||
	   (padding != 0 && ((digits + padding) % 4 != 0 ||
	                     (padding == 1 && digits % 4 != 3) ||
	                     (padding == 2 && digits % 4 != 2))) ||
	   pending != 0)
		imageError(index, "invalid base64 image URI length or padding");
	return result;
}

bool asciiEqualIgnoreCase(const std::string& left, const char* right)
{
	std::size_t pos = 0;
	for(; pos < left.size() && right[pos] != '\0'; ++pos) {
		if(std::tolower(static_cast<unsigned char>(left[pos])) != right[pos]) return false;
	}
	return pos == left.size() && right[pos] == '\0';
}

bool isDataUri(const std::string& uri)
{
	return uri.size() >= 5 && asciiEqualIgnoreCase(uri.substr(0, 5), "data:");
}

std::vector<unsigned char> readDataUri(const std::string& uri, std::size_t index)
{
	const auto comma = uri.find(',', 5);
	if(comma == std::string::npos) imageError(index, "data URI has no payload separator");
	const auto metadata = uri.substr(5, comma - 5);
	const auto separator = metadata.rfind(';');
	const bool base64 = separator != std::string::npos &&
	                    asciiEqualIgnoreCase(metadata.substr(separator + 1), "base64");
	const auto payload = percentDecode(uri.substr(comma + 1), index);
	return base64 ? base64Decode(payload, index) : payload;
}

std::string relativeImagePath(const std::string& uri, std::size_t index)
{
	const auto bytes = percentDecode(uri, index);
	if(bytes.empty()) imageError(index, "empty external image URI");
	std::string path(bytes.begin(), bytes.end());
	if(path.front() == '/' || path.front() == '\\' ||
	   path.find(':') != std::string::npos || path.find('\0') != std::string::npos ||
	   path.find('?') != std::string::npos || path.find('#') != std::string::npos)
		imageError(index, "image URI must be a relative file path");
	std::replace(path.begin(), path.end(), '\\', '/');
	std::string normalized;
	for(std::size_t start = 0; start < path.size();) {
		const auto end = path.find('/', start);
		const auto segment = path.substr(start, end == std::string::npos ? end : end - start);
		if(segment == "..") imageError(index, "image URI escapes its base directory");
		if(!segment.empty() && segment != ".") {
			if(!normalized.empty()) normalized += '/';
			normalized += segment;
		}
		if(end == std::string::npos) break;
		start = end + 1;
	}
	if(normalized.empty()) imageError(index, "empty external image URI");
	return normalized;
}

std::vector<unsigned char> readExternalImage(const Model& model, const std::string& uri,
	                                           std::size_t index)
{
	const auto relative = relativeImagePath(uri, index);
	const auto path = model.baseDir.empty() ? relative :
		model.baseDir + ((model.baseDir.back() == '/' || model.baseDir.back() == '\\') ? "" : "/") + relative;
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if(!input) imageError(index, "cannot open external image: " + path);
	const auto fileSize = static_cast<std::streamoff>(input.tellg());
	if(fileSize <= 0 || fileSize > std::numeric_limits<int>::max())
		imageError(index, "external image has an invalid or unsupported size: " + path);
	std::vector<unsigned char> bytes(static_cast<std::size_t>(fileSize));
	input.seekg(0, std::ios::beg);
	input.read(reinterpret_cast<char*>(bytes.data()), fileSize);
	if(!input) imageError(index, "cannot read external image: " + path);
	return bytes;
}

std::vector<unsigned char> readBufferViewImage(const Model& model, int viewIndex,
	                                             std::size_t imageIndex)
{
	if(viewIndex < 0 || static_cast<std::size_t>(viewIndex) >= model.bufferViews.size())
		imageError(imageIndex, "bufferView index is out of range");
	const auto& view = model.bufferViews[static_cast<std::size_t>(viewIndex)];
	if(view.buffer < 0 || static_cast<std::size_t>(view.buffer) >= model.buffers.size())
		imageError(imageIndex, "bufferView refers to an invalid buffer");
	const auto& data = model.buffers[static_cast<std::size_t>(view.buffer)].data;
	if(view.byteOffset > data.size() || view.byteLength == 0 ||
	   view.byteLength > data.size() - view.byteOffset ||
	   view.byteLength > static_cast<std::size_t>(std::numeric_limits<int>::max()))
		imageError(imageIndex, "bufferView image byte range is invalid");
	return std::vector<unsigned char>(data.begin() + view.byteOffset,
	                                  data.begin() + view.byteOffset + view.byteLength);
}

std::vector<unsigned char> encodedImage(const Model& model, const Image& image,
	                                     std::size_t index)
{
	if(image.bufferView >= 0) {
		if(!image.uri.empty()) imageError(index, "both bufferView and URI are present");
		return readBufferViewImage(model, image.bufferView, index);
	}
	if(image.uri.empty()) imageError(index, "neither bufferView nor URI is present");
	return isDataUri(image.uri) ? readDataUri(image.uri, index) :
	                              readExternalImage(model, image.uri, index);
}

} // namespace

void decodeImages(Model& model)
{
	for(std::size_t index = 0; index < model.images.size(); ++index) {
		auto& image = model.images[index];
		const auto bytes = encodedImage(model, image, index);
		if(bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
			imageError(index, "encoded image is empty or too large");

		int width = 0;
		int height = 0;
		int channels = 0;
		const int byteCount = static_cast<int>(bytes.size());
		if(!stbi_info_from_memory(bytes.data(), byteCount, &width, &height, &channels))
			imageError(index, "unsupported or corrupt image data");
		if(width <= 0 || height <= 0 ||
		   static_cast<std::size_t>(width) > maxDecodedImageBytes / 4 / static_cast<std::size_t>(height))
			imageError(index, "decoded image dimensions are invalid or too large");

		int decodedWidth = 0;
		int decodedHeight = 0;
		int decodedChannels = 0;
		auto* pixels = stbi_load_from_memory(bytes.data(), byteCount,
			&decodedWidth, &decodedHeight, &decodedChannels, 4);
		if(!pixels) {
			const char* detail = stbi_failure_reason();
			imageError(index, detail ? std::string("image decoding failed: ") + detail :
			                          "image decoding failed");
		}
		auto freePixels = [&]() noexcept { stbi_image_free(pixels); };
		ScopeExit::ScopeExit<decltype(freePixels)> pixelsGuard(std::move(freePixels));
		if(decodedWidth != width || decodedHeight != height)
			imageError(index, "decoded image dimensions changed unexpectedly");
		const auto pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
		std::vector<unsigned char> rgba(pixels, pixels + pixelCount);
		image.image = std::move(rgba);
		image.width = width;
		image.height = height;
		image.component = 4;
		image.bits = 8;
		image.pixel_type = TG3_COMPONENT_TYPE_UNSIGNED_BYTE;
		image.asIs = false;
	}
}

} // namespace gltf
} // namespace Ogre_glTF

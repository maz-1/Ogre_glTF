#include "Ogre_glTF_parser.hpp"

#include <ScopeExit/ScopeExit.h>
#include <tiny_gltf_v3.h>

#include <cstdlib>
#include <exception>
#include <fstream>
#include <limits>
#include <utility>

namespace Ogre_glTF {
namespace gltf {
namespace {

std::string parseError(const tg3_error_stack& errors, tg3_error_code code)
{
    std::string message;
    for(uint32_t i = 0; i < tg3_errors_count(&errors); ++i)
    {
        const tg3_error_entry* entry = tg3_errors_get(&errors, i);
        if(!entry || entry->severity != TG3_SEVERITY_ERROR || !entry->message)
            continue;
        if(!message.empty()) message += "; ";
        message += entry->message;
    }
    if(message.empty())
        message = "tinygltf v3 parse failed (error " + std::to_string(static_cast<int>(code)) + ")";
    return message;
}

std::string directoryOf(const std::string& path)
{
    const auto separator = path.find_last_of("/\\");
    if(separator == std::string::npos) return ".";
    if(separator == 0 || (separator == 2 && path[1] == ':'))
        return path.substr(0, separator + 1);
    return path.substr(0, separator);
}

struct FileContext
{
    std::string rootPath;
    std::string parserBaseDir;
    std::string readBaseDir;
    bool rootReadPending = false;
};

int hexDigit(char ch)
{
    if(ch >= '0' && ch <= '9') return ch - '0';
    if(ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if(ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool decodeSafeRelativeUri(const std::string& encoded, std::string& decoded)
{
    decoded.clear();
    decoded.reserve(encoded.size());
    for(size_t i = 0; i < encoded.size(); ++i)
    {
        if(encoded[i] != '%')
        {
            decoded.push_back(encoded[i]);
            continue;
        }
        if(i + 2 >= encoded.size()) return false;
        const int high = hexDigit(encoded[i + 1]);
        const int low = hexDigit(encoded[i + 2]);
        if(high < 0 || low < 0) return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }

    if(decoded.empty() || decoded.front() == '/' || decoded.front() == '\\') return false;
    size_t segmentStart = 0;
    while(segmentStart < decoded.size())
    {
        const size_t segmentEnd = decoded.find_first_of("/\\", segmentStart);
        const size_t end = segmentEnd == std::string::npos ? decoded.size() : segmentEnd;
        const std::string segment = decoded.substr(segmentStart, end - segmentStart);
        if(segment.empty() || segment == ".." || segment.find(':') != std::string::npos ||
           segment.find('\0') != std::string::npos)
            return false;
        if(segmentEnd == std::string::npos) break;
        segmentStart = segmentEnd + 1;
    }
    return true;
}

std::string parserDirectoryOf(const std::string& path)
{
    const auto separator = path.find_last_of("/\\");
    return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

bool externalFilePath(const std::string& parserPath, const FileContext& context,
                      std::string& resolved)
{
    std::string encodedUri;
    if(context.parserBaseDir.empty())
        encodedUri = parserPath;
    else
    {
        std::string prefix = context.parserBaseDir;
        if(prefix.back() != '/' && prefix.back() != '\\') prefix += '/';
        if(parserPath.compare(0, prefix.size(), prefix) != 0) return false;
        encodedUri = parserPath.substr(prefix.size());
    }

    std::string decodedUri;
    if(!decodeSafeRelativeUri(encodedUri, decodedUri)) return false;
    resolved = context.readBaseDir.empty() ? std::string(".") : context.readBaseDir;
    if(resolved.back() != '/' && resolved.back() != '\\') resolved += '/';
    resolved += decodedUri;
    return true;
}

int32_t readFile(uint8_t** outData, uint64_t* outSize, const char* path,
                 uint32_t pathLen, void* userData)
{
    if(!outData || !outSize || !path || !userData) return 0;
    *outData = nullptr;
    *outSize = 0;

    try
    {
        auto& context = *static_cast<FileContext*>(userData);
        const std::string parserPath(path, pathLen);
        std::string filePath;
        if(context.rootReadPending)
        {
            context.rootReadPending = false;
            if(parserPath != context.rootPath || parserPath.find('\0') != std::string::npos)
                return 0;
            filePath = parserPath;
        }
        else if(!externalFilePath(parserPath, context, filePath))
            return 0;

        std::ifstream stream(filePath, std::ios::binary | std::ios::ate);
        if(!stream) return 0;
        const std::streamoff length = stream.tellg();
        if(length < 0 || static_cast<uint64_t>(length) > std::numeric_limits<size_t>::max() ||
           static_cast<uint64_t>(length) > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
            return 0;
        const size_t byteCount = static_cast<size_t>(length);
        auto* bytes = static_cast<uint8_t*>(std::malloc(byteCount == 0 ? 1 : byteCount));
        if(!bytes) return 0;
        bool transferred = false;
        auto releaseBytes = [&]() { if(!transferred) std::free(bytes); };
        ScopeExit::ScopeExit<decltype(releaseBytes)> bytesGuard(std::move(releaseBytes));
        stream.seekg(0);
        if(byteCount != 0)
            stream.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(byteCount));
        if(!stream) return 0;
        *outData = bytes;
        *outSize = static_cast<uint64_t>(byteCount);
        transferred = true;
        return 1;
    }
    catch(...)
    {
        return 0;
    }
}

void freeFile(uint8_t* data, uint64_t, void*)
{
    std::free(data);
}

void configureFileAccess(tg3_parse_options& options, FileContext& context)
{
    options.fs.read_file = readFile;
    options.fs.free_file = freeFile;
    options.fs.user_data = &context;
}

template<class Parse>
bool parseModel(Model& dest, std::string& error, const std::string& baseDir, Parse parse)
{
    error.clear();

    tg3_error_stack errors {};
    tg3_error_stack_init(&errors);
    auto freeErrors = [&errors]() { tg3_error_stack_free(&errors); };
    ScopeExit::ScopeExit<decltype(freeErrors)> errorsGuard(std::move(freeErrors));

    tg3_model raw {};
    auto freeModel = [&raw]() { tg3_model_free(&raw); };
    ScopeExit::ScopeExit<decltype(freeModel)> modelGuard(std::move(freeModel));

    const tg3_error_code code = parse(raw, errors);
    if(code != TG3_OK)
    {
        // Error text can be owned by raw's arena, so copy it before modelGuard runs.
        error = parseError(errors, code);
        return false;
    }

    // v3 can report success for an external buffer whose URI was too long to
    // resolve, leaving its data empty. Importers must not index that buffer.
    if(raw.buffers_count != 0 && !raw.buffers)
    {
        error = "glTF parser returned buffers without storage";
        return false;
    }
    for(uint32_t i = 0; i < raw.buffers_count; ++i)
    {
        const auto& buffer = raw.buffers[i];
        if(buffer.byte_length > buffer.data.count ||
           (buffer.data.count != 0 && !buffer.data.data))
        {
            error = "glTF buffer[" + std::to_string(i) + "] has missing or incomplete data";
            return false;
        }
    }

    try
    {
        Model converted = fromTinyGltf3(raw);
        converted.baseDir = baseDir;
        dest = std::move(converted);
    }
    catch(const std::exception& exception)
    {
        error = std::string("glTF conversion failed: ") + exception.what();
        return false;
    }
    catch(...)
    {
        error = "glTF conversion failed";
        return false;
    }
    return true;
}

} // namespace

bool parseFile(Model& dest, std::string& error, const std::string& path)
{
    if(path.empty() || path.size() > std::numeric_limits<uint32_t>::max() ||
       path.find('\0') != std::string::npos)
    {
        error = "Invalid glTF file path";
        return false;
    }
    FileContext context { path, parserDirectoryOf(path), directoryOf(path), true };
    return parseModel(dest, error, context.readBaseDir, [&path, &context](tg3_model& raw, tg3_error_stack& errors) {
        tg3_parse_options options;
        tg3_parse_options_init(&options);
        configureFileAccess(options, context);
        return tg3_parse_file(&raw, &errors, path.c_str(), static_cast<uint32_t>(path.size()), &options);
    });
}

bool parseGlb(Model& dest, std::string& error, const uint8_t* data, size_t size,
              const std::string& baseDir)
{
    if(!data || !size || baseDir.size() > std::numeric_limits<uint32_t>::max() ||
       baseDir.find('\0') != std::string::npos)
    {
        error = "Invalid GLB input";
        return false;
    }
    FileContext context { {}, baseDir, baseDir, false };
    return parseModel(dest, error, baseDir, [data, size, &baseDir, &context](tg3_model& raw, tg3_error_stack& errors) {
        tg3_parse_options options;
        tg3_parse_options_init(&options);
        configureFileAccess(options, context);
        return tg3_parse_glb(&raw, &errors, data, static_cast<uint64_t>(size),
                             baseDir.c_str(), static_cast<uint32_t>(baseDir.size()), &options);
    });
}

} // namespace gltf
} // namespace Ogre_glTF

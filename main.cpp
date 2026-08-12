#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <process.h>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kLcgMultiplier = 0x6C078965u;
constexpr uint32_t kLcgIncrement = 0x00003039u;
constexpr uint32_t kLcgMultiplierInverse = 0x9638806Du;
constexpr size_t kMaxRawEntrySize = 128u * 1024u * 1024u;
constexpr int kTotalPages = 109;
constexpr wchar_t kWindowClass[] = L"Nobu16DlcBookExtractorWindow";
constexpr wchar_t kWindowTitle[] = L"NOBU16 DLC 북 이미지 추출기";
constexpr UINT kMessageProgress = WM_APP + 1;
constexpr UINT kMessageDone = WM_APP + 2;

constexpr int kIdGamePath = 101;
constexpr int kIdGameBrowse = 102;
constexpr int kIdOutputPath = 103;
constexpr int kIdOutputBrowse = 104;
constexpr int kIdOverwrite = 105;
constexpr int kIdStart = 106;
constexpr int kIdProgress = 107;
constexpr int kIdStatus = 108;
constexpr int kIdLog = 109;
constexpr int kIdOpenOutput = 110;

const std::array<uint8_t, 4> kLinkMagic{'L', 'I', 'N', 'K'};
const std::array<uint8_t, 8> kG1tMagic{'G', 'T', '1', 'G', '0', '6', '0', '0'};

struct AppError {
    std::wstring message;
};

[[noreturn]] void fail(const std::wstring& message) {
    throw AppError{message};
}

std::wstring widenUtf8(const std::string& value) {
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return L"알 수 없는 시스템 오류";
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring hexHresult(HRESULT hr) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
           << static_cast<uint32_t>(hr);
    return stream.str();
}

void checkHr(HRESULT hr, const wchar_t* operation) {
    if (FAILED(hr)) fail(std::wstring(operation) + L" 실패 (" + hexHresult(hr) + L")");
}

template <typename T>
void releaseCom(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

struct ArchiveSpec {
    const wchar_t* relativePath;
    size_t expectedEntries;
};

constexpr ArchiveSpec kPkArchive{L"DLC_PK\\Common\\abstpk.n16", 52};
constexpr ArchiveSpec kBaseArchive{L"DLC\\Common\\abst.n16", 57};

struct BookSpec {
    const wchar_t* title;
    bool usesPkArchive;
    size_t firstEntry;
    size_t pageCount;
};

constexpr std::array<BookSpec, 3> kBooks{{
    {L"무장 아트북", true, 0, 29},
    {L"40주년 메모리얼북", true, 29, 23},
    {L"시부사와 코우 비전 공략 데이터 & 무장 아트북", false, 0, 57},
}};

struct LinkEntry {
    uint64_t offset;
    size_t storedSize;
};

struct Archive {
    fs::path path;
    uint32_t seed;
    std::vector<LinkEntry> entries;
};

struct Texture {
    size_t width;
    size_t height;
    const uint8_t* payload;
    size_t payloadSize;
};

struct CroppedImage {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> pixels;
};

struct ExtractResult {
    int pages;
    uint64_t digest;
    double elapsedSeconds;
};

using ProgressCallback = void (*)(void*, int, const std::wstring&);

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8u) |
           (static_cast<uint32_t>(data[2]) << 16u) | (static_cast<uint32_t>(data[3]) << 24u);
}

uint64_t readU64(const uint8_t* data) {
    uint64_t result = 0;
    for (unsigned index = 0; index < 8; ++index) result |= static_cast<uint64_t>(data[index]) << (index * 8u);
    return result;
}

uint32_t lcgStep(uint32_t state) {
    return state * kLcgMultiplier + kLcgIncrement;
}

uint8_t keyByte(uint32_t state) {
    return static_cast<uint8_t>((state >> 24u) ^ (state >> 16u));
}

uint32_t advanceLcg(uint32_t state, uint64_t steps) {
    uint32_t resultMultiplier = 1;
    uint32_t resultIncrement = 0;
    uint32_t currentMultiplier = kLcgMultiplier;
    uint32_t currentIncrement = kLcgIncrement;
    while (steps != 0) {
        if ((steps & 1u) != 0) {
            resultIncrement = currentMultiplier * resultIncrement + currentIncrement;
            resultMultiplier = currentMultiplier * resultMultiplier;
        }
        currentIncrement = currentMultiplier * currentIncrement + currentIncrement;
        currentMultiplier = currentMultiplier * currentMultiplier;
        steps >>= 1u;
    }
    return resultMultiplier * state + resultIncrement;
}

void cryptAt(uint8_t* data, size_t size, uint32_t seed, uint64_t offset) {
    uint32_t state = advanceLcg(seed, offset);
    for (size_t index = 0; index < size; ++index) {
        state = lcgStep(state);
        data[index] ^= keyByte(state);
    }
}

std::vector<uint32_t> recoverSeeds(const uint8_t* encryptedMagic) {
    std::array<uint8_t, 4> wanted{};
    for (size_t index = 0; index < wanted.size(); ++index) wanted[index] = encryptedMagic[index] ^ kLinkMagic[index];
    std::vector<uint32_t> result;
    for (uint32_t byte2 = 0; byte2 <= 255; ++byte2) {
        const uint32_t byte3 = byte2 ^ wanted[0];
        for (uint32_t low = 0; low <= 0xFFFFu; ++low) {
            const uint32_t state1 = (byte3 << 24u) | (byte2 << 16u) | low;
            const uint32_t state2 = lcgStep(state1);
            if (keyByte(state2) != wanted[1]) continue;
            const uint32_t state3 = lcgStep(state2);
            if (keyByte(state3) != wanted[2]) continue;
            const uint32_t state4 = lcgStep(state3);
            if (keyByte(state4) != wanted[3]) continue;
            result.push_back((state1 - kLcgIncrement) * kLcgMultiplierInverse);
        }
    }
    return result;
}

size_t alignUp(size_t value, size_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) fail(L"잘못된 정렬 단위입니다.");
    if (value > SIZE_MAX - alignment + 1) fail(L"정렬 크기 오버플로입니다.");
    return (value + alignment - 1) & ~(alignment - 1);
}

std::vector<uint8_t> readFileRange(const fs::path& path, uint64_t offset, size_t size) {
    std::ifstream file(path, std::ios::binary);
    if (!file) fail(L"파일을 열 수 없습니다: " + path.wstring());
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) fail(L"파일 위치 이동에 실패했습니다: " + path.wstring());
    std::vector<uint8_t> result(size);
    if (size != 0) file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size));
    if (file.gcount() != static_cast<std::streamsize>(size)) fail(L"파일 데이터가 잘렸습니다: " + path.wstring());
    return result;
}

bool parseLinkHeader(const std::vector<uint8_t>& header, size_t expectedEntries, uint64_t fileSize,
                     std::vector<LinkEntry>& result) {
    if (header.size() < 32 || !std::equal(kLinkMagic.begin(), kLinkMagic.end(), header.begin())) return false;
    const size_t count = readU32(header.data() + 4);
    const uint32_t version = readU32(header.data() + 8);
    const size_t reserved = readU32(header.data() + 12);
    if (count != expectedEntries || version != 32 || reserved != count) return false;
    if (count > (SIZE_MAX - 32) / 8) return false;
    const size_t tableEnd = 32 + count * 8;
    const size_t alignedTableEnd = alignUp(tableEnd, 32);
    if (header.size() < alignedTableEnd || readU32(header.data() + 16) != alignedTableEnd ||
        readU32(header.data() + 20) != 0 || readU32(header.data() + 24) != 0 || readU32(header.data() + 28) != 0)
        return false;

    std::vector<LinkEntry> parsed;
    parsed.reserve(count);
    uint64_t previousEnd = alignedTableEnd;
    for (size_t index = 0; index < count; ++index) {
        const uint8_t* row = header.data() + 32 + index * 8;
        const uint64_t offset = readU32(row);
        const size_t storedSize = readU32(row + 4);
        if (storedSize < 24 || offset < previousEnd || offset > fileSize || storedSize > fileSize - offset) return false;
        previousEnd = offset + storedSize;
        parsed.push_back({offset, storedSize});
    }
    result = std::move(parsed);
    return true;
}

Archive openArchive(const fs::path& gameRoot, const ArchiveSpec& spec) {
    const fs::path path = gameRoot / spec.relativePath;
    std::error_code ec;
    const uint64_t fileSize = fs::file_size(path, ec);
    if (ec) fail(L"DLC 아카이브 크기를 읽을 수 없습니다: " + path.wstring());
    const size_t headerSize = alignUp(32 + spec.expectedEntries * 8, 32);
    const std::vector<uint8_t> encryptedHeader = readFileRange(path, 0, headerSize);
    const std::vector<uint32_t> candidates = recoverSeeds(encryptedHeader.data());
    std::vector<Archive> validated;
    for (uint32_t seed : candidates) {
        std::vector<uint8_t> header = encryptedHeader;
        cryptAt(header.data(), header.size(), seed, 0);
        std::vector<LinkEntry> entries;
        if (parseLinkHeader(header, spec.expectedEntries, fileSize, entries))
            validated.push_back({path, seed, std::move(entries)});
    }
    if (validated.size() != 1)
        fail(L"DLC 복호화 키를 유일하게 판별하지 못했습니다: " + path.wstring());
    return std::move(validated.front());
}

std::vector<uint8_t> lz4Decompress(const uint8_t* source, size_t sourceSize, size_t expectedSize) {
    std::vector<uint8_t> output;
    output.reserve(expectedSize);
    size_t cursor = 0;
    while (cursor < sourceSize) {
        const uint8_t token = source[cursor++];
        size_t literalLength = token >> 4u;
        if (literalLength == 15) {
            for (;;) {
                if (cursor >= sourceSize) fail(L"LZ4 리터럴 길이가 잘렸습니다.");
                const size_t extra = source[cursor++];
                if (literalLength > SIZE_MAX - extra) fail(L"LZ4 길이 오버플로입니다.");
                literalLength += extra;
                if (extra != 255) break;
            }
        }
        if (literalLength > sourceSize - cursor || literalLength > expectedSize - output.size())
            fail(L"LZ4 리터럴 범위가 잘못되었습니다.");
        output.insert(output.end(), source + cursor, source + cursor + literalLength);
        cursor += literalLength;
        if (cursor == sourceSize) break;
        if (sourceSize - cursor < 2) fail(L"LZ4 매치 오프셋이 잘렸습니다.");
        const size_t offset = static_cast<size_t>(source[cursor]) | (static_cast<size_t>(source[cursor + 1]) << 8u);
        cursor += 2;
        if (offset == 0 || offset > output.size()) fail(L"LZ4 매치 오프셋이 잘못되었습니다.");
        size_t matchLength = (token & 0x0Fu) + 4u;
        if ((token & 0x0Fu) == 15) {
            for (;;) {
                if (cursor >= sourceSize) fail(L"LZ4 매치 길이가 잘렸습니다.");
                const size_t extra = source[cursor++];
                if (matchLength > SIZE_MAX - extra) fail(L"LZ4 길이 오버플로입니다.");
                matchLength += extra;
                if (extra != 255) break;
            }
        }
        if (matchLength > expectedSize - output.size()) fail(L"LZ4 출력이 선언 크기를 초과합니다.");
        for (size_t index = 0; index < matchLength; ++index) output.push_back(output[output.size() - offset]);
    }
    if (output.size() != expectedSize) fail(L"LZ4 출력 크기가 선언값과 다릅니다.");
    return output;
}

std::vector<uint8_t> readEntry(const Archive& archive, size_t index) {
    if (index >= archive.entries.size()) fail(L"요청한 LINK 엔트리가 없습니다.");
    const LinkEntry& entry = archive.entries[index];
    std::vector<uint8_t> stored = readFileRange(archive.path, entry.offset, entry.storedSize);
    cryptAt(stored.data(), stored.size(), archive.seed, entry.offset);
    const uint64_t rawSize64 = readU64(stored.data() + 8);
    const uint64_t compressedSize64 = readU64(stored.data() + 16);
    if (rawSize64 == 0 || rawSize64 > kMaxRawEntrySize || compressedSize64 == 0 ||
        compressedSize64 != stored.size() - 24)
        fail(L"DLC 압축 래퍼 크기가 잘못되었습니다.");
    return lz4Decompress(stored.data() + 24, static_cast<size_t>(compressedSize64), static_cast<size_t>(rawSize64));
}

Texture parseTexture(const std::vector<uint8_t>& raw) {
    if (raw.size() < 40 || !std::equal(kG1tMagic.begin(), kG1tMagic.end(), raw.begin()))
        fail(L"압축 해제 결과가 GT1G0600 이미지가 아닙니다.");
    const size_t declaredSize = readU32(raw.data() + 8);
    const size_t directoryOffset = readU32(raw.data() + 12);
    const size_t textureCount = readU32(raw.data() + 16);
    if (declaredSize != raw.size() || textureCount != 1 || directoryOffset < 32 || directoryOffset > raw.size() - 4)
        fail(L"지원하지 않는 G1T 헤더입니다.");
    const size_t relativeOffset = readU32(raw.data() + directoryOffset);
    if (relativeOffset > raw.size() - directoryOffset) fail(L"G1T 텍스처 오프셋이 잘못되었습니다.");
    const size_t start = directoryOffset + relativeOffset;
    if (start > raw.size() - 8) fail(L"G1T 텍스처 헤더가 잘렸습니다.");
    const uint8_t* header = raw.data() + start;
    if (header[1] != 0x5B) fail(L"지원하지 않는 G1T 형식입니다. BC3(0x5B)가 필요합니다.");
    const uint8_t dimensions = header[2];
    const size_t width = size_t{1} << (dimensions & 0x0F);
    const size_t height = size_t{1} << (dimensions >> 4u);
    size_t payloadOffset = start + 8;
    if (header[7] != 0) {
        if (start > raw.size() - 12) fail(L"G1T 확장 헤더가 잘렸습니다.");
        const size_t extraLength = readU32(raw.data() + start + 8);
        if (extraLength < 4 || extraLength > raw.size() - payloadOffset) fail(L"G1T 확장 헤더 길이가 잘못되었습니다.");
        payloadOffset += extraLength;
    }
    const size_t blocksX = (width + 3) / 4;
    const size_t blocksY = (height + 3) / 4;
    if (blocksX > SIZE_MAX / blocksY || blocksX * blocksY > SIZE_MAX / 16) fail(L"G1T 크기 오버플로입니다.");
    const size_t payloadSize = blocksX * blocksY * 16;
    if (payloadOffset > raw.size() || payloadSize > raw.size() - payloadOffset) fail(L"G1T BC3 데이터가 잘렸습니다.");
    return {width, height, raw.data() + payloadOffset, payloadSize};
}

std::array<uint8_t, 3> expand565(uint16_t value) {
    const uint32_t red = (value >> 11u) & 0x1Fu;
    const uint32_t green = (value >> 5u) & 0x3Fu;
    const uint32_t blue = value & 0x1Fu;
    return {static_cast<uint8_t>((red * 255 + 15) / 31), static_cast<uint8_t>((green * 255 + 31) / 63),
            static_cast<uint8_t>((blue * 255 + 15) / 31)};
}

std::array<uint8_t, 8> alphaPalette(uint8_t alpha0, uint8_t alpha1) {
    std::array<uint8_t, 8> palette{};
    palette[0] = alpha0;
    palette[1] = alpha1;
    if (alpha0 > alpha1) {
        for (size_t step = 1; step <= 6; ++step)
            palette[step + 1] = static_cast<uint8_t>(((7 - step) * alpha0 + step * alpha1 + 3) / 7);
    } else {
        for (size_t step = 1; step <= 4; ++step)
            palette[step + 1] = static_cast<uint8_t>(((5 - step) * alpha0 + step * alpha1 + 2) / 5);
        palette[6] = 0;
        palette[7] = 255;
    }
    return palette;
}

std::array<std::array<uint8_t, 4>, 16> decodeBc3Block(const uint8_t* block) {
    const auto alphas = alphaPalette(block[0], block[1]);
    uint64_t alphaBits = 0;
    for (size_t index = 0; index < 6; ++index) alphaBits |= static_cast<uint64_t>(block[2 + index]) << (index * 8u);
    const auto first = expand565(static_cast<uint16_t>(readU32(block + 8) & 0xFFFFu));
    const auto second = expand565(static_cast<uint16_t>(readU32(block + 10) & 0xFFFFu));
    std::array<uint8_t, 3> third{};
    std::array<uint8_t, 3> fourth{};
    for (size_t channel = 0; channel < 3; ++channel) {
        third[channel] = static_cast<uint8_t>((2u * first[channel] + second[channel] + 1u) / 3u);
        fourth[channel] = static_cast<uint8_t>((first[channel] + 2u * second[channel] + 1u) / 3u);
    }
    const std::array<std::array<uint8_t, 3>, 4> colors{first, second, third, fourth};
    const uint32_t colorBits = readU32(block + 12);
    std::array<std::array<uint8_t, 4>, 16> result{};
    for (size_t pixel = 0; pixel < 16; ++pixel) {
        const auto& color = colors[(colorBits >> (pixel * 2u)) & 3u];
        result[pixel] = {color[0], color[1], color[2], alphas[(alphaBits >> (pixel * 3u)) & 7u]};
    }
    return result;
}

CroppedImage decodeCropped(const Texture& texture) {
    const size_t blocksX = (texture.width + 3) / 4;
    const size_t blocksY = (texture.height + 3) / 4;
    size_t minX = texture.width;
    size_t minY = texture.height;
    size_t maxX = 0;
    size_t maxY = 0;
    bool found = false;
    for (size_t blockY = 0; blockY < blocksY; ++blockY) {
        for (size_t blockX = 0; blockX < blocksX; ++blockX) {
            const uint8_t* block = texture.payload + (blockY * blocksX + blockX) * 16;
            if (block[0] == 0 && block[1] == 0 && std::all_of(block + 2, block + 8, [](uint8_t value) { return value == 0; }))
                continue;
            const auto alphas = alphaPalette(block[0], block[1]);
            uint64_t alphaBits = 0;
            for (size_t index = 0; index < 6; ++index) alphaBits |= static_cast<uint64_t>(block[2 + index]) << (index * 8u);
            for (size_t pixel = 0; pixel < 16; ++pixel) {
                const size_t x = blockX * 4 + pixel % 4;
                const size_t y = blockY * 4 + pixel / 4;
                if (x >= texture.width || y >= texture.height) continue;
                if (alphas[(alphaBits >> (pixel * 3u)) & 7u] != 0) {
                    found = true;
                    minX = std::min(minX, x);
                    minY = std::min(minY, y);
                    maxX = std::max(maxX, x);
                    maxY = std::max(maxY, y);
                }
            }
        }
    }
    if (!found) fail(L"이미지가 완전히 투명합니다.");
    const size_t cropWidth = maxX - minX + 1;
    const size_t cropHeight = maxY - minY + 1;
    if (cropWidth > UINT32_MAX || cropHeight > UINT32_MAX || cropWidth > SIZE_MAX / cropHeight ||
        cropWidth * cropHeight > SIZE_MAX / 4)
        fail(L"PNG 크기 오버플로입니다.");
    std::vector<uint8_t> pixels(cropWidth * cropHeight * 4);
    for (size_t blockY = minY / 4; blockY <= maxY / 4; ++blockY) {
        for (size_t blockX = minX / 4; blockX <= maxX / 4; ++blockX) {
            const auto decoded = decodeBc3Block(texture.payload + (blockY * blocksX + blockX) * 16);
            for (size_t pixel = 0; pixel < 16; ++pixel) {
                const size_t sourceX = blockX * 4 + pixel % 4;
                const size_t sourceY = blockY * 4 + pixel / 4;
                if (sourceX < minX || sourceX > maxX || sourceY < minY || sourceY > maxY) continue;
                const size_t target = ((sourceY - minY) * cropWidth + sourceX - minX) * 4;
                std::copy(decoded[pixel].begin(), decoded[pixel].end(), pixels.begin() + static_cast<ptrdiff_t>(target));
            }
        }
    }
    return {static_cast<uint32_t>(cropWidth), static_cast<uint32_t>(cropHeight), std::move(pixels)};
}

IWICImagingFactory* createWicFactory() {
    IWICImagingFactory* factory = nullptr;
    checkHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            L"Windows 이미지 코덱 초기화");
    return factory;
}

void writePng(IWICImagingFactory* factory, const fs::path& target, const CroppedImage& image, bool overwrite) {
    if (!overwrite && fs::exists(target)) fail(L"파일이 이미 있습니다: " + target.wstring());
    const std::wstring fileName = target.filename().wstring();
    fs::path temporary;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        temporary = target.parent_path() /
                    (L"." + fileName + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(attempt));
        if (!fs::exists(temporary)) break;
        temporary.clear();
    }
    if (temporary.empty()) fail(L"임시 PNG 이름을 만들 수 없습니다.");

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    try {
        checkHr(factory->CreateStream(&stream), L"PNG 스트림 생성");
        checkHr(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE), L"임시 PNG 열기");
        checkHr(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), L"PNG 인코더 생성");
        checkHr(encoder->Initialize(stream, WICBitmapEncoderNoCache), L"PNG 인코더 초기화");
        checkHr(encoder->CreateNewFrame(&frame, &properties), L"PNG 프레임 생성");
        checkHr(frame->Initialize(properties), L"PNG 프레임 초기화");
        checkHr(frame->SetSize(image.width, image.height), L"PNG 크기 설정");
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
        checkHr(frame->SetPixelFormat(&format), L"PNG 픽셀 형식 설정");
        std::vector<uint8_t> convertedPixels;
        const std::vector<uint8_t>* pixelsToWrite = &image.pixels;
        if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
            convertedPixels = image.pixels;
            for (size_t offset = 0; offset < convertedPixels.size(); offset += 4)
                std::swap(convertedPixels[offset], convertedPixels[offset + 2]);
            pixelsToWrite = &convertedPixels;
        } else if (!IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA)) {
            fail(L"WIC가 32비트 RGBA/BGRA PNG 형식을 지원하지 않습니다.");
        }
        const uint64_t stride64 = static_cast<uint64_t>(image.width) * 4;
        if (stride64 > UINT_MAX || pixelsToWrite->size() > UINT_MAX) fail(L"PNG 버퍼가 WIC 제한을 초과합니다.");
        checkHr(frame->WritePixels(image.height, static_cast<UINT>(stride64), static_cast<UINT>(pixelsToWrite->size()),
                                   const_cast<BYTE*>(pixelsToWrite->data())),
                L"PNG 픽셀 기록");
        checkHr(frame->Commit(), L"PNG 프레임 저장");
        checkHr(encoder->Commit(), L"PNG 저장");
        releaseCom(properties);
        releaseCom(frame);
        releaseCom(encoder);
        releaseCom(stream);
    } catch (...) {
        releaseCom(properties);
        releaseCom(frame);
        releaseCom(encoder);
        releaseCom(stream);
        DeleteFileW(temporary.c_str());
        throw;
    }

    const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temporary.c_str(), target.c_str(), flags)) {
        DeleteFileW(temporary.c_str());
        fail(L"완성된 PNG를 이동할 수 없습니다: " + target.wstring());
    }
}

std::wstring normalizedPathText(const fs::path& input) {
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(fs::absolute(input, ec), ec);
    if (ec) {
        ec.clear();
        normalized = fs::absolute(input, ec).lexically_normal();
        if (ec) normalized = input.lexically_normal();
    }
    std::wstring value = normalized.wstring();
    if (value.rfind(L"\\\\?\\", 0) == 0) value.erase(0, 4);
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return value;
}

bool isInsideOrEqual(const fs::path& parent, const fs::path& child) {
    const std::wstring parentText = normalizedPathText(parent);
    const std::wstring childText = normalizedPathText(child);
    if (childText == parentText) return true;
    return childText.size() > parentText.size() && childText.compare(0, parentText.size(), parentText) == 0 &&
           childText[parentText.size()] == L'\\';
}

fs::path locateGameRoot(const fs::path& requested) {
    std::error_code ec;
    fs::path root = requested;
    if (fs::is_regular_file(root, ec)) root = root.parent_path();
    root = fs::canonical(root, ec);
    if (ec) fail(L"게임 폴더를 열 수 없습니다: " + requested.wstring());
    for (const auto& spec : {kPkArchive, kBaseArchive}) {
        if (!fs::is_regular_file(root / spec.relativePath, ec))
            fail(L"필수 DLC 파일을 찾을 수 없습니다: " + (root / spec.relativePath).wstring());
    }
    return root;
}

void ensureOutputSafe(const fs::path& gameRoot, const fs::path& outputRoot) {
    if (isInsideOrEqual(gameRoot, outputRoot))
        fail(L"원본 보호를 위해 출력 폴더는 게임 설치 폴더 밖으로 지정하세요.");
}

uint64_t fnvUpdate(uint64_t digest, const uint8_t* data, size_t size) {
    for (size_t index = 0; index < size; ++index) {
        digest ^= data[index];
        digest *= 0x00000100000001B3ull;
    }
    return digest;
}

ExtractResult extractAll(const fs::path& requestedGameRoot, const std::optional<fs::path>& requestedOutput,
                         bool overwrite, ProgressCallback callback, void* callbackContext) {
    const auto started = std::chrono::steady_clock::now();
    const fs::path gameRoot = locateGameRoot(requestedGameRoot);
    std::optional<fs::path> outputRoot;
    if (requestedOutput) {
        std::error_code ec;
        outputRoot = fs::absolute(*requestedOutput, ec).lexically_normal();
        if (ec) fail(L"출력 경로를 해석할 수 없습니다.");
        ensureOutputSafe(gameRoot, *outputRoot);
    }
    const Archive pk = openArchive(gameRoot, kPkArchive);
    const Archive base = openArchive(gameRoot, kBaseArchive);

    if (outputRoot) {
        size_t conflictCount = 0;
        for (const auto& book : kBooks) {
            for (size_t page = 1; page <= book.pageCount; ++page)
                if (fs::exists(*outputRoot / book.title / (std::to_wstring(page) + L".png"))) ++conflictCount;
        }
        if (conflictCount != 0 && !overwrite)
            fail(L"기존 PNG " + std::to_wstring(conflictCount) + L"개가 있습니다. '기존 PNG 덮어쓰기'를 선택하세요.");
        std::error_code ec;
        for (const auto& book : kBooks) {
            fs::create_directories(*outputRoot / book.title, ec);
            if (ec) fail(L"출력 폴더를 만들 수 없습니다: " + (*outputRoot / book.title).wstring());
        }
    }

    IWICImagingFactory* factory = outputRoot ? createWicFactory() : nullptr;
    int completed = 0;
    uint64_t digest = 0xCBF29CE484222325ull;
    try {
        for (const auto& book : kBooks) {
            const Archive& archive = book.usesPkArchive ? pk : base;
            for (size_t page = 0; page < book.pageCount; ++page) {
                const std::vector<uint8_t> raw = readEntry(archive, book.firstEntry + page);
                const Texture texture = parseTexture(raw);
                const CroppedImage image = decodeCropped(texture);
                const std::array<uint8_t, 8> dimensions{
                    static_cast<uint8_t>(image.width), static_cast<uint8_t>(image.width >> 8u),
                    static_cast<uint8_t>(image.width >> 16u), static_cast<uint8_t>(image.width >> 24u),
                    static_cast<uint8_t>(image.height), static_cast<uint8_t>(image.height >> 8u),
                    static_cast<uint8_t>(image.height >> 16u), static_cast<uint8_t>(image.height >> 24u)};
                digest = fnvUpdate(digest, dimensions.data(), dimensions.size());
                digest = fnvUpdate(digest, image.pixels.data(), image.pixels.size());
                if (outputRoot) {
                    const fs::path target = *outputRoot / book.title / (std::to_wstring(page + 1) + L".png");
                    writePng(factory, target, image, overwrite);
                }
                ++completed;
                if (callback) {
                    std::wostringstream line;
                    line << book.title << L"  " << (page + 1) << L".png  (" << image.width << L"×" << image.height << L")";
                    callback(callbackContext, completed, line.str());
                }
            }
        }
    } catch (...) {
        releaseCom(factory);
        throw;
    }
    releaseCom(factory);
    if (completed != kTotalPages) fail(L"내부 페이지 수 검증에 실패했습니다.");
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return {completed, digest, elapsed};
}

int runSelfTests() {
    int phase = 1;
    try {
        const uint32_t seed = 0x0019A6CCu;
        std::array<uint8_t, 4> encrypted = kLinkMagic;
        cryptAt(encrypted.data(), encrypted.size(), seed, 0);
        const auto candidates = recoverSeeds(encrypted.data());
        if (std::find(candidates.begin(), candidates.end(), seed) == candidates.end()) fail(L"seed test");

        phase = 2;
        const std::array<uint8_t, 6> literal{0x50, 'h', 'e', 'l', 'l', 'o'};
        if (lz4Decompress(literal.data(), literal.size(), 5) != std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}))
            fail(L"literal test");
        phase = 3;
        const std::array<uint8_t, 6> overlap{0x32, 'a', 'b', 'c', 0x03, 0x00};
        if (lz4Decompress(overlap.data(), overlap.size(), 9) !=
            std::vector<uint8_t>({'a', 'b', 'c', 'a', 'b', 'c', 'a', 'b', 'c'}))
            fail(L"overlap test");

        phase = 4;
        std::array<uint8_t, 16> redBlock{};
        redBlock[0] = 255;
        redBlock[8] = 0;
        redBlock[9] = 0xF8;
        const auto red = decodeBc3Block(redBlock.data());
        if (!std::all_of(red.begin(), red.end(), [](const auto& pixel) { return pixel == std::array<uint8_t, 4>{255, 0, 0, 255}; }))
            fail(L"BC3 test");

        phase = 5;
        if (!isInsideOrEqual(fs::path(L"F:\\Games\\NOBU16"),
                             fs::path(L"F:\\Games\\elsewhere\\..\\NOBU16\\output")))
            fail(L"path safety test");

        phase = 6;
        IWICImagingFactory* factory = createWicFactory();
        wchar_t tempDirectory[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, tempDirectory) == 0) {
            releaseCom(factory);
            fail(L"temporary path test");
        }
        const fs::path testPath = fs::path(tempDirectory) /
                                  (L"nobu16-dlc-book-extractor-self-test-" + std::to_wstring(GetCurrentProcessId()) + L".png");
        DeleteFileW(testPath.c_str());
        CroppedImage pixel{1, 1, {1, 2, 3, 255}};
        phase = 7;
        writePng(factory, testPath, pixel, false);
        phase = 8;
        const auto bytes = readFileRange(testPath, 0, 8);
        const std::array<uint8_t, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        if (!std::equal(signature.begin(), signature.end(), bytes.begin())) fail(L"PNG signature test");

        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* decodedFrame = nullptr;
        IWICFormatConverter* converter = nullptr;
        checkHr(factory->CreateDecoderFromFilename(testPath.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnLoad, &decoder),
                L"PNG 자체 테스트 디코더 생성");
        checkHr(decoder->GetFrame(0, &decodedFrame), L"PNG 자체 테스트 프레임 읽기");
        checkHr(factory->CreateFormatConverter(&converter), L"PNG 자체 테스트 변환기 생성");
        checkHr(converter->Initialize(decodedFrame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                      nullptr, 0.0, WICBitmapPaletteTypeCustom),
                L"PNG 자체 테스트 RGBA 변환");
        std::array<uint8_t, 4> decodedPixel{};
        checkHr(converter->CopyPixels(nullptr, 4, 4, decodedPixel.data()), L"PNG 자체 테스트 픽셀 읽기");
        releaseCom(converter);
        releaseCom(decodedFrame);
        releaseCom(decoder);
        releaseCom(factory);
        DeleteFileW(testPath.c_str());
        if (decodedPixel != std::array<uint8_t, 4>{1, 2, 3, 255}) fail(L"PNG RGBA round-trip test");
        return 0;
    } catch (...) {
        return phase;
    }
}

struct UiProgress {
    int completed;
    std::wstring line;
};

struct UiDone {
    bool success;
    std::wstring message;
    fs::path outputRoot;
};

struct WorkerArgs {
    HWND window;
    fs::path gameRoot;
    fs::path outputRoot;
    bool overwrite;
};

struct AppState {
    HWND window{};
    HWND gamePath{};
    HWND gameBrowse{};
    HWND outputPath{};
    HWND outputBrowse{};
    HWND overwrite{};
    HWND start{};
    HWND progress{};
    HWND status{};
    HWND log{};
    HWND openOutput{};
    HFONT font{};
    HFONT titleFont{};
    bool running{};
    fs::path lastOutput;
};

void postProgress(void* context, int completed, const std::wstring& line) {
    HWND window = static_cast<HWND>(context);
    auto* update = new UiProgress{completed, line};
    if (!PostMessageW(window, kMessageProgress, 0, reinterpret_cast<LPARAM>(update))) delete update;
}

unsigned __stdcall workerThread(void* rawArgs) {
    std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs*>(rawArgs));
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* done = new UiDone{false, L"", args->outputRoot};
    try {
        const ExtractResult result = extractAll(args->gameRoot, args->outputRoot, args->overwrite, postProgress, args->window);
        std::wostringstream message;
        message << L"총 " << result.pages << L"장을 모두 추출했습니다.\n\n출력 폴더:\n" << args->outputRoot.wstring()
                << L"\n\n소요 시간: " << std::fixed << std::setprecision(1) << result.elapsedSeconds << L"초";
        done->success = true;
        done->message = message.str();
    } catch (const AppError& error) {
        done->message = error.message;
    } catch (const std::exception& error) {
        done->message = L"시스템 오류: " + widenUtf8(error.what());
    } catch (...) {
        done->message = L"알 수 없는 오류가 발생했습니다.";
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (!PostMessageW(args->window, kMessageDone, 0, reinterpret_cast<LPARAM>(done))) delete done;
    return 0;
}

std::wstring windowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<size_t>(length));
    return result;
}

void appendLog(HWND control, const std::wstring& line) {
    SendMessageW(control, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    const std::wstring text = line + L"\r\n";
    SendMessageW(control, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
}

std::optional<fs::path> chooseFolder(HWND owner, const wchar_t* title, const std::wstring& initial) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return std::nullopt;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    IShellItem* initialItem = nullptr;
    if (!initial.empty() && SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&initialItem)))) {
        dialog->SetFolder(initialItem);
        initialItem->Release();
    }
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        return std::nullopt;
    }
    if (FAILED(shown)) {
        dialog->Release();
        return std::nullopt;
    }
    IShellItem* item = nullptr;
    PWSTR value = nullptr;
    std::optional<fs::path> result;
    if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
        result = fs::path(value);
        CoTaskMemFree(value);
    }
    if (item) item->Release();
    dialog->Release();
    return result;
}

void setControlsEnabled(AppState* state, bool enabled) {
    EnableWindow(state->gamePath, enabled);
    EnableWindow(state->gameBrowse, enabled);
    EnableWindow(state->outputPath, enabled);
    EnableWindow(state->outputBrowse, enabled);
    EnableWindow(state->overwrite, enabled);
    EnableWindow(state->start, enabled);
    EnableWindow(state->openOutput, enabled && !state->lastOutput.empty());
}

HWND makeControl(HWND parent, DWORD extendedStyle, const wchar_t* className, const wchar_t* text, DWORD style,
                 int x, int y, int width, int height, int id) {
    return CreateWindowExW(extendedStyle, className, text, style | WS_CHILD | WS_VISIBLE, x, y, width, height, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void applyFont(AppState* state, HWND control) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
}

void beginExtraction(AppState* state) {
    const std::wstring game = windowText(state->gamePath);
    const std::wstring output = windowText(state->outputPath);
    if (game.empty() || output.empty()) {
        MessageBoxW(state->window, L"게임 폴더와 출력 폴더를 모두 지정하세요.", kWindowTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    state->running = true;
    state->lastOutput.clear();
    setControlsEnabled(state, false);
    SetWindowTextW(state->log, L"");
    SetWindowTextW(state->status, L"DLC 아카이브를 검사하는 중...");
    SendMessageW(state->progress, PBM_SETPOS, 0, 0);
    auto* args = new WorkerArgs{state->window, fs::path(game), fs::path(output),
                                SendMessageW(state->overwrite, BM_GETCHECK, 0, 0) == BST_CHECKED};
    HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, workerThread, args, 0, nullptr));
    if (!thread) {
        delete args;
        state->running = false;
        setControlsEnabled(state, true);
        MessageBoxW(state->window, L"추출 작업 스레드를 시작할 수 없습니다.", kWindowTitle, MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(thread);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_CREATE: {
            auto* created = new AppState{};
            created->window = window;
            created->font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            created->titleFont = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            state = created;
            HWND title = makeControl(window, 0, L"STATIC", L"DLC 북 이미지 추출", SS_LEFT, 24, 20, 600, 30, 0);
            HWND description = makeControl(window, 0, L"STATIC",
                L"아트북·메모리얼북·공략 데이터의 이미지를 원본 순서대로 PNG로 저장합니다.", SS_LEFT, 24, 52, 640, 23, 0);
            HWND gameLabel = makeControl(window, 0, L"STATIC", L"게임 설치 폴더", SS_LEFT, 24, 91, 150, 23, 0);
            created->gamePath = makeControl(window, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 24, 117, 548, 29, kIdGamePath);
            created->gameBrowse = makeControl(window, 0, L"BUTTON", L"찾아보기...", BS_PUSHBUTTON | WS_TABSTOP, 581, 116, 94, 31, kIdGameBrowse);
            HWND outputLabel = makeControl(window, 0, L"STATIC", L"출력 폴더", SS_LEFT, 24, 161, 150, 23, 0);
            created->outputPath = makeControl(window, WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 24, 187, 548, 29, kIdOutputPath);
            created->outputBrowse = makeControl(window, 0, L"BUTTON", L"찾아보기...", BS_PUSHBUTTON | WS_TABSTOP, 581, 186, 94, 31, kIdOutputBrowse);
            created->overwrite = makeControl(window, 0, L"BUTTON", L"기존 PNG 덮어쓰기", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 232, 200, 25, kIdOverwrite);
            created->start = makeControl(window, 0, L"BUTTON", L"추출 시작", BS_DEFPUSHBUTTON | WS_TABSTOP, 505, 228, 170, 35, kIdStart);
            created->progress = makeControl(window, 0, PROGRESS_CLASSW, L"", PBS_SMOOTH, 24, 277, 651, 22, kIdProgress);
            SendMessageW(created->progress, PBM_SETRANGE32, 0, kTotalPages);
            created->status = makeControl(window, 0, L"STATIC", L"경로를 지정한 뒤 '추출 시작'을 누르세요.", SS_LEFT, 24, 308, 651, 23, kIdStatus);
            created->log = makeControl(window, WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 24, 337, 651, 145, kIdLog);
            created->openOutput = makeControl(window, 0, L"BUTTON", L"출력 폴더 열기", BS_PUSHBUTTON | WS_TABSTOP, 505, 494, 170, 32, kIdOpenOutput);
            EnableWindow(created->openOutput, FALSE);
            HWND notice = makeControl(window, 0, L"STATIC", L"원본 DLC 파일은 변경하지 않습니다. 출력 폴더는 게임 폴더 밖으로 지정하세요.", SS_LEFT, 24, 500, 465, 22, 0);
            for (HWND control : {title, description, gameLabel, created->gamePath, created->gameBrowse, outputLabel,
                                 created->outputPath, created->outputBrowse, created->overwrite, created->start,
                                 created->status, created->log, created->openOutput, notice})
                applyFont(created, control);
            SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(created->titleFont), TRUE);
            return 0;
        }
        case WM_COMMAND:
            if (!state) break;
            switch (LOWORD(wParam)) {
                case kIdGameBrowse: {
                    const auto selected = chooseFolder(window, L"NOBU16 게임 설치 폴더 선택", windowText(state->gamePath));
                    if (selected) SetWindowTextW(state->gamePath, selected->c_str());
                    return 0;
                }
                case kIdOutputBrowse: {
                    const auto selected = chooseFolder(window, L"이미지를 저장할 폴더 선택", windowText(state->outputPath));
                    if (selected) SetWindowTextW(state->outputPath, selected->c_str());
                    return 0;
                }
                case kIdStart:
                    beginExtraction(state);
                    return 0;
                case kIdOpenOutput:
                    if (!state->lastOutput.empty())
                        ShellExecuteW(window, L"open", state->lastOutput.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                default:
                    break;
            }
            break;
        case kMessageProgress: {
            std::unique_ptr<UiProgress> update(reinterpret_cast<UiProgress*>(lParam));
            if (state && update) {
                SendMessageW(state->progress, PBM_SETPOS, update->completed, 0);
                const std::wstring status = std::to_wstring(update->completed) + L" / " + std::to_wstring(kTotalPages) + L" 추출 중";
                SetWindowTextW(state->status, status.c_str());
                appendLog(state->log, update->line);
            }
            return 0;
        }
        case kMessageDone: {
            std::unique_ptr<UiDone> done(reinterpret_cast<UiDone*>(lParam));
            if (state && done) {
                state->running = false;
                if (done->success) state->lastOutput = done->outputRoot;
                setControlsEnabled(state, true);
                SetWindowTextW(state->status, done->success ? L"완료 — 총 109장을 저장했습니다." : L"추출 실패");
                MessageBoxW(window, done->message.c_str(), kWindowTitle,
                            MB_OK | (done->success ? MB_ICONINFORMATION : MB_ICONERROR));
            }
            return 0;
        }
        case WM_CLOSE:
            if (state && state->running) {
                MessageBoxW(window, L"이미지 추출이 끝난 뒤 창을 닫아 주세요.", kWindowTitle, MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            if (state) {
                if (state->font) DeleteObject(state->font);
                if (state->titleFont) DeleteObject(state->titleFont);
                delete state;
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int runGui(HINSTANCE instance, int showCommand) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&windowClass)) return 1;
    constexpr int width = 720;
    constexpr int height = 580;
    RECT desktop{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
    const int x = desktop.left + ((desktop.right - desktop.left) - width) / 2;
    const int y = desktop.top + ((desktop.bottom - desktop.top) - height) / 2;
    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle,
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

std::vector<std::wstring> commandLineArguments() {
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> result;
    if (values) {
        for (int index = 1; index < count; ++index) result.emplace_back(values[index]);
        LocalFree(values);
    }
    return result;
}

int runHeadless(const std::vector<std::wstring>& args) {
    if (args.size() == 1 && args[0] == L"--self-test") return runSelfTests();
    fs::path gameRoot;
    bool verify = false;
    for (size_t index = 0; index < args.size(); ++index) {
        if (args[index] == L"--verify-only") {
            verify = true;
        } else if (args[index] == L"--game-root" && index + 1 < args.size()) {
            gameRoot = args[++index];
        }
    }
    if (!verify || gameRoot.empty()) return -1;
    try {
        const ExtractResult result = extractAll(gameRoot, std::nullopt, false, nullptr, nullptr);
        return result.pages == kTotalPages && result.digest == 0x088EF11165AB940Cull ? 0 : 2;
    } catch (...) {
        return 1;
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDPIAware();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::vector<std::wstring> args = commandLineArguments();
    const int headless = runHeadless(args);
    int result = headless >= 0 ? headless : runGui(instance, showCommand);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}

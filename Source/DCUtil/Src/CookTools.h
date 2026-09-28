#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace DCCook
{
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

Bytes Read(const fs::path& Path);
void Write(const fs::path& Path, const Bytes& Data);
uint32_t Get(const Bytes& Data, size_t Offset, unsigned Width = 4);
void Put(Bytes& Data, uint32_t Value, unsigned Width = 4);
void Set(Bytes& Data, size_t Offset, uint32_t Value, unsigned Width = 4);
std::string Lower(std::string Text);
void Run(const std::vector<std::string>& Args, const fs::path& Directory, const fs::path& Log,
         const std::string& Marker = "", const std::map<std::string, std::string>& Environment = {},
         const fs::path& SecondaryLog = {}, const std::string& Progress = {});

struct Encoders
{
	std::string Pvrtex = "/opt/toolchains/dc/kos/utils/pvrtex/pvrtex";
	std::string Adpcm = "/opt/toolchains/dc/kos/utils/wav2adpcm/wav2adpcm";
	std::string Ffmpeg = "ffmpeg";
	std::string Convert = "convert";
};

void Texture(const fs::path& Input, const Encoders& Tools);
void Sound(const fs::path& Input, const Encoders& Tools);
void Music(const fs::path& Package, const fs::path& Output, const fs::path& Scratch,
           const Encoders& Tools);
void Lightmaps(const fs::path& Static, const fs::path& Dynamic, const fs::path& Output,
               const fs::path& Scratch, const Encoders& Tools);
} // namespace DCCook

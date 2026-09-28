#include "CookTools.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace DCCook
{
namespace
{
using Chunks = std::map<std::string, Bytes>;
Chunks Wave(const Bytes& Data)
{
	if (Get(Data, 0) != 0x46464952 || Get(Data, 8) != 0x45564157)
		throw std::runtime_error("Not a RIFF WAVE file");
	const uint64_t End = uint64_t(Get(Data, 4)) + 8;
	if (End > Data.size() || End < 12)
		throw std::runtime_error("Invalid RIFF extent");
	Chunks Result;
	for (size_t Pos = 12; Pos < End;)
	{
		if (End - Pos < 8)
			throw std::runtime_error("Truncated WAVE chunk");
		const auto Size = Get(Data, Pos + 4);
		if (Size > End - Pos - 8)
			throw std::runtime_error("Truncated WAVE payload");
		const std::string Key(Data.begin() + Pos, Data.begin() + Pos + 4);
		Result[Key] = Bytes(Data.begin() + Pos + 8, Data.begin() + Pos + 8 + Size);
		Pos += 8 + uint64_t(Size) + (Size & 1);
	}
	if (!Result.count("fmt ") || !Result.count("data"))
		throw std::runtime_error("Missing WAVE format or samples");
	return Result;
}

Bytes MakeWave(const std::vector<std::pair<std::string, Bytes>>& Chunks)
{
	Bytes Data;
	Put(Data, 0x46464952);
	Put(Data, 0);
	Put(Data, 0x45564157);
	for (const auto& Chunk : Chunks)
	{
		if (Chunk.first.size() != 4)
			throw std::runtime_error("Invalid WAVE chunk name");
		Data.insert(Data.end(), Chunk.first.begin(), Chunk.first.end());
		Put(Data, Chunk.second.size());
		Data.insert(Data.end(), Chunk.second.begin(), Chunk.second.end());
		if (Chunk.second.size() & 1)
			Data.push_back(0);
	}
	Set(Data, 4, Data.size() - 8);
	return Data;
}

void CheckWave(const Chunks& W, unsigned Tag, unsigned Rate, unsigned Bits)
{
	const auto& Format = W.at("fmt ");
	if (Get(Format, 0, 2) != Tag || Get(Format, 2, 2) != 1 || Get(Format, 4) != Rate ||
	    Get(Format, 14, 2) != Bits)
		throw std::runtime_error("Encoder produced unexpected WAVE format");
}
} // namespace

void Sound(const fs::path& Input, const Encoders& Tools)
{
	auto Original = Wave(Read(Input));
	const auto& Format = Original.at("fmt ");
	const auto Channels = Get(Format, 2, 2), Rate = Get(Format, 4), Bits = Get(Format, 14, 2);
	if (Get(Format, 0, 2) != 1 || (Channels != 1 && Channels != 2) || (Bits != 8 && Bits != 16) ||
	    !Rate)
		throw std::runtime_error("Unsupported PCM source: " + Input.string());
	auto Pcm = Input;
	Pcm.replace_extension(".pcm.wav");
	auto Output = Input;
	Output.replace_extension(".dca.wav");
	Run({Tools.Ffmpeg, "-nostdin", "-v", "error", "-y", "-i", Input.string(), "-af",
	     "aresample=11025:filter_size=64", "-ac", "1", "-ar", "11025", "-c:a", "pcm_s16le",
	     "-map_metadata", "-1", Pcm.string()},
	    Input.parent_path(), Input.string() + ".ffmpeg.log", "", {}, {},
	    "resampling " + Input.filename().string());
	auto Decoded = Wave(Read(Pcm));
	CheckWave(Decoded, 1, 11025, 16);
	const auto Samples = Decoded.at("data").size() / 2;
	if (!Samples || Samples >= 0x80000000ULL)
		throw std::runtime_error("Invalid sample count");
	Decoded.at("fmt ").resize(16);
	Write(Pcm, MakeWave({{"fmt ", Decoded.at("fmt ")}, {"data", Decoded.at("data")}}));
	Run({Tools.Adpcm, "-t", Pcm.string(), Output.string()}, Input.parent_path(),
	    Input.string() + ".adpcm.log", "", {}, {},
	    "encoding " + Input.filename().string() + " as ADPCM");
	auto Encoded = Wave(Read(Output));
	CheckWave(Encoded, 0x14, 11025, 4);
	if (Encoded.at("data").size() * 2 < Samples || Encoded.at("data").size() * 2 > Samples + 1)
		throw std::runtime_error("ADPCM sample count mismatch");
	Bytes Fact;
	Put(Fact, Samples);
	std::vector<std::pair<std::string, Bytes>> Chunks{{"fmt ", Encoded.at("fmt ")}, {"fact", Fact}};
	if (Original.count("smpl"))
	{
		Bytes Loop = Original.at("smpl");
		const auto Count = Get(Loop, 28);
		if (Count > 1 || Loop.size() < 36 + Count * 24)
			throw std::runtime_error("Unsupported sample loop layout");
		Set(Loop, 8, 90703);
		if (Count)
		{
			if (Get(Loop, 40) || Get(Loop, 52) || Get(Loop, 56))
				throw std::runtime_error("Unsupported non-forward/infinite loop");
			const uint64_t Start = uint64_t(Get(Loop, 44)) * 11025 / Rate;
			const uint64_t EndExclusive =
			    std::min<uint64_t>(Samples, (uint64_t(Get(Loop, 48)) + 1) * 11025 / Rate);
			if (Start >= EndExclusive || (Samples > 65534 && (Start || EndExclusive != Samples)))
				throw std::runtime_error("Unsupported resampled loop extent");
			Set(Loop, 44, Start);
			Set(Loop, 48, EndExclusive - 1);
		}
		Chunks.emplace_back("smpl", Loop);
	}
	Chunks.emplace_back("data", Encoded.at("data"));
	Write(Output, MakeWave(Chunks));
}

void Texture(const fs::path& Input, const Encoders& Tools)
{
	const auto Png = Read(Input);
	const std::array<uint8_t, 8> Signature{{137, 80, 78, 71, 13, 10, 26, 10}};
	if (Png.size() < 24 || !std::equal(Signature.begin(), Signature.end(), Png.begin()))
		throw std::runtime_error("Invalid PNG: " + Input.string());
	auto Big = [&](size_t Offset)
	{
		return uint32_t(Png[Offset]) << 24 | uint32_t(Png[Offset + 1]) << 16 |
		       uint32_t(Png[Offset + 2]) << 8 | Png[Offset + 3];
	};
	uint32_t Width = Big(16), Height = Big(20);
	if (!Width || !Height)
		throw std::runtime_error("Empty PNG");
	const bool Mips = !fs::exists(Input.string() + ".nomip");
	const bool Vq =
	    !fs::exists(Input.string() + ".novq") && (!Mips || uint64_t(Width) * Height >= 4096);
	fs::path Source = Input;
	if (Vq && std::max(Width, Height) > 256)
	{
		while (std::max(Width, Height) > 256)
		{
			Width = std::max(8u, Width / 2);
			Height = std::max(8u, Height / 2);
		}
		Source = Input.string() + ".scaled.png";
		Run({Tools.Convert, Input.string(), "-filter", "Lanczos", "-resize",
		     std::to_string(Width) + "x" + std::to_string(Height) + "!", "-strip", Source.string()},
		    Input.parent_path(), Input.string() + ".resize.log");
	}
	auto Output = Input;
	Output.replace_extension(".dt");
	std::vector<std::string> Args{Tools.Pvrtex,    "-i", Source.string(), "-o",
	                              Output.string(), "-f", "ARGB1555"};
	if (Mips)
		Args.insert(Args.end(), {"--mipmap=quality", "--resize=up", "--mip-resize=up"});
	else
		Args.push_back("--resize=up");
	const auto Entries = std::max(Width, Height) < 256 ? 32u : 256u;
	if (Vq)
		Args.push_back("--compress=" + std::to_string(Entries));
	Run(Args, Input.parent_path(), Input.string() + ".pvrtex.log", "", {}, {},
	    "encoding " + Input.filename().string() + " as PVR texture");
	const auto Data = Read(Output);
	const auto Mode = Get(Data, 16);
	if (Data.size() < 32 || Get(Data, 0) != 0x78546344 || Get(Data, 4) != Data.size() ||
	    bool(Mode & 0x80000000) != Mips || bool(Mode & 0x40000000) != Vq ||
	    (Vq && Data[10] + 1u != Entries) || (8u << ((Mode >> 3) & 7)) < Width ||
	    (8u << (Mode & 7)) < Height)
		throw std::runtime_error("Invalid DT texture policy: " + Output.string());
	if (Source != Input)
		fs::remove(Source);
}

void Music(const fs::path& Package, const fs::path& Output, const fs::path& Scratch,
           const Encoders& Tools)
{
	const auto Data = Read(Package);
	if (Get(Data, 0) != 0x9e2a83c1 || Get(Data, 4, 2) != 61)
		throw std::runtime_error("Expected retail version 61 music package: " + Package.string());
	auto Index = [&](size_t& Pos) -> int32_t
	{
		uint32_t B = Get(Data, Pos++, 1), Value = B & 63;
		const bool Negative = B & 128;
		if (B & 64)
			for (unsigned Shift = 6; Shift <= 27; Shift += 7)
			{
				B = Get(Data, Pos++, 1);
				if (Shift == 27 && (B & 0xf0))
					throw std::runtime_error("Invalid compact index");
				Value |= (B & 127) << Shift;
				if (!(B & 128))
					break;
			}
		if (Value > INT32_MAX)
			throw std::runtime_error("Compact index overflow");
		return Negative ? -int32_t(Value) : int32_t(Value);
	};
	std::vector<std::string> Names, Imports;
	size_t Pos = Get(Data, 16);
	const auto NameCount = Get(Data, 12), ImportCount = Get(Data, 28), ExportCount = Get(Data, 20);
	if (NameCount > Data.size() || ImportCount > Data.size() || ExportCount > Data.size())
		throw std::runtime_error("Invalid package tables");
	for (unsigned i = 0; i < NameCount; ++i)
	{
		std::string Name;
		while (Get(Data, Pos, 1))
			Name.push_back(char(Get(Data, Pos++, 1)));
		++Pos;
		Get(Data, Pos);
		Pos += 4;
		Names.push_back(Name);
	}
	Pos = Get(Data, 32);
	for (unsigned i = 0; i < ImportCount; ++i)
	{
		Index(Pos);
		Index(Pos);
		Get(Data, Pos);
		Pos += 4;
		Imports.push_back(Names.at(Index(Pos)));
	}
	fs::create_directories(Scratch);
	fs::create_directories(Output);
	Pos = Get(Data, 24);
	unsigned Tracks = 0;
	for (unsigned i = 0; i < ExportCount; ++i)
	{
		const auto Class = Index(Pos);
		Index(Pos);
		Get(Data, Pos);
		Pos += 4;
		const auto ObjectName = Index(Pos);
		Get(Data, Pos);
		Pos += 4;
		const auto Size = Index(Pos);
		const auto Offset = Size > 0 ? Index(Pos) : 0;
		if (Size <= 0 || Class >= 0 || Imports.at(-int64_t(Class) - 1) != "Music")
			continue;
		if (Offset < 0 || uint64_t(Offset) + Size > Data.size())
			throw std::runtime_error("Invalid music export extent");
		size_t Body = Offset;
		if (Names.at(Index(Body)) != "None")
			throw std::runtime_error("Music export has unsupported properties");
		const auto Type = Names.at(Index(Body));
		const auto Count = Index(Body);
		if (Count <= 0 || Body + Count != uint64_t(Offset) + Size)
			throw std::runtime_error("Music export size mismatch");
		const auto Name = Names.at(ObjectName);
		auto SafeName = [](const std::string& Name)
		{
			return !Name.empty() && Name != "." && Name != ".." &&
			       Name.find_first_of("/\\:\r\n") == std::string::npos;
		};
		if (!SafeName(Name) || !SafeName(Type))
			throw std::runtime_error("Invalid music export name");
		Bytes Module(Data.begin() + Body, Data.begin() + Body + Count);
		if (Get(Module, 0) != 0x4d504d49 && (Module.size() < 48 || Get(Module, 44) != 0x4d524353))
			throw std::runtime_error("Music export is not IT/S3M");
		const auto Input = Scratch / (Name + "." + Type);
		const auto Pcm = Scratch / (Name + ".pcm.wav");
		const auto Track = Output / (Name + ".dcw");
		Write(Input, Module);
		Run({Tools.Ffmpeg, "-nostdin", "-v", "error", "-y", "-f", "libopenmpt", "-i",
		     Input.string(), "-ar", "22050", "-ac", "1", "-c:a", "pcm_s16le", "-map_metadata", "-1",
		     Pcm.string()},
		    Scratch, Input.string() + ".ffmpeg.log", "", {}, {}, "rendering " + Name + " music");
		auto Decoded = Wave(Read(Pcm));
		CheckWave(Decoded, 1, 22050, 16);
		const auto Samples = Decoded.at("data").size() / 2;
		if (!Samples)
			throw std::runtime_error("Empty rendered music");
		Decoded.at("fmt ").resize(16);
		Write(Pcm, MakeWave({{"fmt ", Decoded.at("fmt ")}, {"data", Decoded.at("data")}}));
		Run({Tools.Adpcm, "-t", Pcm.string(), Track.string()}, Scratch,
		    Input.string() + ".adpcm.log", "", {}, {}, "encoding " + Name + " music");
		const auto TrackData = Read(Track);
		const auto Encoded = Wave(TrackData);
		CheckWave(Encoded, 0x14, 22050, 4);
		const auto EncodedSamples = Encoded.at("data").size() * 2;
		if (EncodedSamples + 1 < Samples || EncodedSamples > Samples + 1)
			throw std::runtime_error("Music encoder dropped samples");
		size_t DataOffset = 12;
		while (Get(TrackData, DataOffset) != 0x61746164)
		{
			const auto ChunkSize = Get(TrackData, DataOffset + 4);
			DataOffset += 8 + uint64_t(ChunkSize) + (ChunkSize & 1);
		}
		if (DataOffset + 8 > 96)
			throw std::runtime_error("Music data exceeds runtime WAVE header scan");
		Bytes Metadata;
		for (auto V : {0x314d4344u, 22050u, uint32_t(EncodedSamples), 1u, 0u, 0u})
			Put(Metadata, V);
		Write(Output / (Name + ".dcm"), Metadata);
		++Tracks;
	}
	if (!Tracks)
		throw std::runtime_error("No music exports: " + Package.string());
}
} // namespace DCCook

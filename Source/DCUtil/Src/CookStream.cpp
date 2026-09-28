#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct Package
{
	fs::path Path;
	Bytes Data;
	std::array<std::set<uint16_t>, 3> Used;
	uint32_t Id = 0;
};

struct Record
{
	std::string Name;
	uint32_t Offset, Length, Physical;
};

Bytes Read(const fs::path& Path)
{
	const auto Size = fs::file_size(Path);
	if (Size >= 0x80000000ULL)
		throw std::runtime_error("Input exceeds signed archive size: " + Path.string());
	Bytes Data(static_cast<size_t>(Size));
	std::ifstream File(Path, std::ios::binary);
	if (!File || (Size && !File.read(reinterpret_cast<char*>(Data.data()), Size)))
		throw std::runtime_error("Cannot read " + Path.string());
	return Data;
}

uint32_t Number(const std::string& Text)
{
	if (Text.empty() || Text.find_first_not_of("0123456789") != std::string::npos)
		throw std::runtime_error("Invalid trace number: " + Text);
	const auto Value = std::stoull(Text);
	if (Value >= 0x80000000ULL)
		throw std::runtime_error("Trace number exceeds archive limit");
	return static_cast<uint32_t>(Value);
}

std::vector<std::string> Fields(std::string Line, size_t Count)
{
	if (!Line.empty() && Line.back() == '\r')
		Line.pop_back();
	std::vector<std::string> Result;
	size_t Start = 0;
	for (size_t Field = 1; Field < Count; ++Field)
	{
		const auto End = Line.find('\t', Start);
		if (End == std::string::npos)
			throw std::runtime_error("Incomplete trace row");
		Result.push_back(Line.substr(Start, End - Start));
		Start = End + 1;
	}
	Result.push_back(Line.substr(Start));
	return Result;
}

std::string Name(const fs::path& Path)
{
	std::string Result = Path.filename().string();
	if (Result.empty() || Result.size() >= 64)
		throw std::runtime_error("Invalid package basename");
	for (char& Ch : Result)
	{
		if (static_cast<unsigned char>(Ch) >= 128)
			throw std::runtime_error("Package basename must be ASCII");
		if (Ch >= 'A' && Ch <= 'Z')
			Ch += 'a' - 'A';
	}
	return Result;
}

void Put(Bytes& Data, uint32_t Value, unsigned Width = 4)
{
	for (unsigned Byte = 0; Byte < Width; ++Byte)
		Data.push_back(static_cast<uint8_t>(Value >> (Byte * 8)));
}

uint32_t Get(const Bytes& Data, size_t Offset)
{
	if (Offset > Data.size() || Data.size() - Offset < 4)
		throw std::runtime_error("Truncated package header");
	uint32_t Value = 0;
	for (unsigned Byte = 0; Byte < 4; ++Byte)
		Value |= uint32_t(Data[Offset + Byte]) << (Byte * 8);
	return Value;
}

void Write(std::ofstream& File, const void* Data, size_t Size)
{
	if (Size && !File.write(static_cast<const char*>(Data), Size))
		throw std::runtime_error("Stream output write failed");
}

void Pack(const fs::path& Raw, const fs::path& Output, const fs::path& SystemPath)
{
	if (fs::exists(Output))
		throw std::runtime_error("Output already exists: " + Output.string());
	const auto System = fs::canonical(SystemPath);
	const auto Root = System.parent_path();
	const Bytes Body = Read(Raw);
	std::map<std::string, Package> Packages;
	std::map<std::string, std::string> Paths;
	std::vector<Record> Records;
	uint64_t Cursor = 0;
	uint32_t Flags = 2;
	std::ifstream Manifest(Raw.string() + ".manifest.tsv");
	if (!Manifest)
		throw std::runtime_error("Missing stream manifest");
	std::string Line;
	while (std::getline(Manifest, Line))
	{
		if (!Line.empty() && Line.back() == '\r')
			Line.pop_back();
		if (Line.compare(0, 13, "# mip_policy\t") == 0)
		{
			if (Line != "# mip_policy\tdeferred")
				throw std::runtime_error("Unsupported stream mip policy");
			Flags |= 1;
		}
		if (Line.empty() || Line[0] == '#')
			continue;
		const auto Row = Fields(Line, 4);
		auto Cached = Paths.find(Row[3]);
		if (Cached == Paths.end())
		{
			const fs::path Path = fs::canonical(System / Row[3]);
			const auto Relative = Path.lexically_relative(Root);
			if (Relative.empty() || *Relative.begin() == "..")
				throw std::runtime_error("Stream source outside cooked tree");
			const auto Key = Name(Path);
			auto Inserted = Packages.emplace(Key, Package{});
			Package& Source = Inserted.first->second;
			if (Inserted.second)
			{
				Source.Path = Path;
				Source.Data = Read(Path);
			}
			else if (Source.Path != Path)
				throw std::runtime_error("Ambiguous package basename: " + Key);
			Cached = Paths.emplace(Row[3], Key).first;
		}
		const auto& Key = Cached->second;
		const Package& Source = Packages.at(Key);
		Record R{Key, Number(Row[2]), Number(Row[1]), Number(Row[0])};
		if (R.Physical != Cursor || !R.Length ||
		    uint64_t(R.Offset) + R.Length > Source.Data.size() || Cursor + R.Length > Body.size())
			throw std::runtime_error("Invalid or nonsequential stream record");
		if (!std::equal(Body.begin() + Cursor, Body.begin() + Cursor + R.Length,
		                Source.Data.begin() + R.Offset))
			throw std::runtime_error("Stream trace differs from package: " + Key);
		Records.push_back(R);
		Cursor += R.Length;
	}
	if (!Manifest.eof() || Records.empty() || Cursor != Body.size() || Packages.size() > 1024)
		throw std::runtime_error("Invalid or incomplete stream manifest");
	std::ifstream Indices(Raw.string() + ".indices.tsv");
	if (!Indices)
		throw std::runtime_error("Missing linker index trace");
	while (std::getline(Indices, Line))
	{
		if (Line.empty() || Line[0] == '#')
			continue;
		const auto Row = Fields(Line, 3);
		const auto Source = Packages.find(Name(Row[0]));
		const auto Kind = Number(Row[1]), Index = Number(Row[2]);
		if (Source == Packages.end() || Kind > 2 || Index > 65535)
			throw std::runtime_error("Invalid linker index trace row");
		Source->second.Used[Kind].insert(static_cast<uint16_t>(Index));
	}
	if (!Indices.eof())
		throw std::runtime_error("Cannot read linker index trace");
	Bytes IndexData;
	Put(IndexData, 0x58494344);
	Put(IndexData, Packages.size());
	uint32_t Id = 0;
	for (auto& Item : Packages)
	{
		Package& P = Item.second;
		P.Id = Id++;
		std::array<uint32_t, 3> Counts{};
		if (P.Data.size() >= 36 && Get(P.Data, 0) == 0x9e2a83c1)
			Counts = {{Get(P.Data, 12), Get(P.Data, 28), Get(P.Data, 20)}};
		for (unsigned Kind = 0; Kind < 3; ++Kind)
		{
			if (Counts[Kind] > 65535 ||
			    (!P.Used[Kind].empty() && *P.Used[Kind].rbegin() >= Counts[Kind]))
				throw std::runtime_error("Linker index outside package table: " + Item.first);
			Put(IndexData, Counts[Kind]);
		}
		for (const auto& Used : P.Used)
			Put(IndexData, Used.size());
		for (const auto& Used : P.Used)
			for (auto Index : Used)
				Put(IndexData, Index, 2);
	}
	const uint64_t BodyOffset = 32 + Packages.size() * 68 + IndexData.size();
	const uint64_t Size = BodyOffset + Records.size() * 12 + Body.size();
	if (Size >= 0x80000000ULL)
		throw std::runtime_error("Stream exceeds signed archive limit");
	Bytes Header;
	for (auto Value : {uint32_t(0x32534344), 3u, uint32_t(Packages.size()),
	                   uint32_t(Records.size()), 32u, uint32_t(BodyOffset), uint32_t(Size), Flags})
		Put(Header, Value);
	for (const auto& Item : Packages)
	{
		const auto Start = Header.size();
		Header.resize(Start + 64, 0);
		std::copy(Item.first.begin(), Item.first.end(), Header.begin() + Start);
		Put(Header, Item.second.Data.size());
	}
	Header.insert(Header.end(), IndexData.begin(), IndexData.end());
	// Reserve a private directory so a failed run cannot overwrite another job's scratch file.
	const fs::path Scratch = Output.string() + ".packing";
	if (!fs::create_directory(Scratch))
		throw std::runtime_error("Packing directory already exists: " + Scratch.string());
	const fs::path Temporary = Scratch / "stream.dcs";
	try
	{
		std::ofstream File(Temporary, std::ios::binary);
		if (!File)
			throw std::runtime_error("Cannot create stream output");
		Write(File, Header.data(), Header.size());
		for (const auto& R : Records)
		{
			Bytes Entry;
			Put(Entry, Packages.at(R.Name).Id);
			Put(Entry, R.Offset);
			Put(Entry, R.Length);
			Write(File, Entry.data(), Entry.size());
			Write(File, Body.data() + R.Physical, R.Length);
		}
		File.close();
		if (!File)
			throw std::runtime_error("Cannot close stream output");
		fs::rename(Temporary, Output);
		fs::remove(Scratch);
	}
	catch (...)
	{
		std::error_code Ignored;
		fs::remove(Temporary, Ignored);
		fs::remove(Scratch, Ignored);
		throw;
	}
	std::printf("DCS3 packed files=%zu records=%zu bytes=%llu\n", Packages.size(), Records.size(),
	            static_cast<unsigned long long>(Size));
}
} // namespace

bool DCCookStreamCommand(int Argc, const char** Argv, int& Result)
{
	if (Argc < 2 || std::string(Argv[1]) != "--pack-stream")
		return false;
	Result = EXIT_FAILURE;
	if (Argc != 5)
	{
		std::fprintf(stderr, "Usage: DCUtil --pack-stream RAW OUTPUT COOKED_SYSTEM\n");
		return true;
	}
	try
	{
		Pack(Argv[2], Argv[3], Argv[4]);
		Result = EXIT_SUCCESS;
	}
	catch (const std::exception& Error)
	{
		std::fprintf(stderr, "Stream packing failed: %s\n", Error.what());
	}
	return true;
}

#include "CookTools.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <tuple>
#include <zlib.h>

namespace DCCook
{
namespace
{
using Rows = std::array<uint32_t, 32>;
struct Position
{
	uint8_t Page = 255, X = 0, Y = 0;
};

Position Place(std::vector<Rows>& Occupancy, unsigned Width, unsigned Height, unsigned Limit)
{
	const unsigned W = (Width + 7) / 8, H = (Height + 7) / 8;
	if (!W || !H || W > 8 || H > 8)
		return {};
	for (unsigned Page = 0; Page < Limit; ++Page)
	{
		if (Page == Occupancy.size())
			Occupancy.emplace_back(Rows{});
		auto& Row = Occupancy[Page];
		for (unsigned Y = 0; Y + H <= 32; Y += H)
			for (unsigned X = 0; X + W <= 32; X += W)
			{
				const uint32_t Mask = ((1u << W) - 1) << X;
				bool Free = true;
				for (unsigned yy = Y; yy < Y + H; ++yy)
					Free &= !(Row[yy] & Mask);
				if (!Free)
					continue;
				for (unsigned yy = Y; yy < Y + H; ++yy)
					Row[yy] |= Mask;
				return {uint8_t(Page), uint8_t(X), uint8_t(Y)};
			}
	}
	return {};
}

void Blit(std::vector<Bytes>& Pages, Position P, const Bytes& Raw, unsigned W, unsigned H)
{
	while (Pages.size() <= P.Page)
		Pages.emplace_back(256 * 256 * 2, 0);
	if (Raw.size() != W * H * 2)
		throw std::runtime_error("Invalid lightmap tile size");
	for (unsigned Y = 0; Y < H; ++Y)
		std::copy_n(Raw.begin() + Y * W * 2, W * 2,
		            Pages[P.Page].begin() + ((P.Y * 8 + Y) * 256 + P.X * 8) * 2);
}

Bytes Bitmap(const Bytes& Pixels)
{
	Bytes Bmp(54 + 256 * 256 * 3, 0);
	Set(Bmp, 0, 0x4d42, 2);
	Set(Bmp, 2, Bmp.size());
	Set(Bmp, 10, 54);
	Set(Bmp, 14, 40);
	Set(Bmp, 18, 256);
	Set(Bmp, 22, 256);
	Set(Bmp, 26, 1, 2);
	Set(Bmp, 28, 24, 2);
	Set(Bmp, 34, 256 * 256 * 3);
	Set(Bmp, 38, 2835);
	Set(Bmp, 42, 2835);
	for (unsigned Y = 0; Y < 256; ++Y)
		for (unsigned X = 0; X < 256; ++X)
		{
			const auto Color = Get(Pixels, (Y * 256 + X) * 2, 2);
			const auto Dest = 54 + ((255 - Y) * 256 + X) * 3;
			Bmp[Dest] = (Color & 31) * 255 / 31;
			Bmp[Dest + 1] = ((Color >> 5) & 63) * 255 / 63;
			Bmp[Dest + 2] = ((Color >> 11) & 31) * 255 / 31;
		}
	return Bmp;
}

void EncodePages(const std::vector<Bytes>& Pages, Bytes& Directory, Bytes& Payload,
                 const fs::path& Scratch, const std::string& Prefix, const Encoders& Tools)
{
	for (size_t i = 0; i < Pages.size(); ++i)
	{
		const auto Base = Scratch / (Prefix + "-" + std::to_string(i));
		Write(Base.string() + ".bmp", Bitmap(Pages[i]));
		Run({Tools.Pvrtex, "-i", Base.string() + ".bmp", "-o", Base.string() + ".dt", "-f",
		     "RGB565", "--compress=256", "--resize=up"},
		    Scratch, Base.string() + ".log");
		const auto Data = Read(Base.string() + ".dt");
		if (Data.size() < 32 || Get(Data, 0) != 0x78546344 || Get(Data, 4) != Data.size() ||
		    Data[10] != 255 || !(Get(Data, 16) & 0x40000000))
			throw std::runtime_error("Invalid VQ lightmap page");
		Put(Directory, Payload.size());
		Put(Directory, Data.size());
		Payload.insert(Payload.end(), Data.begin(), Data.end());
		if ((i + 1) % 4 == 0 || i + 1 == Pages.size())
		{
			std::printf("Cooker: VQ %s atlas pages %zu/%zu\n", Prefix.c_str(), i + 1,
			            Pages.size());
			std::fflush(stdout);
		}
	}
}

Bytes Slice(const Bytes& Data, uint64_t Offset, uint64_t Size)
{
	if (Offset > Data.size() || Size > Data.size() - Offset)
		throw std::runtime_error("Lightmap payload out of range");
	return Bytes(Data.begin() + Offset, Data.begin() + Offset + Size);
}

Bytes StaticAtlas(const Bytes& Source, const fs::path& Scratch, const Encoders& Tools)
{
	const auto Count = Get(Source, 8), TileBytes = Get(Source, 12);
	const uint64_t PayloadStart = 16 + uint64_t(Count) * 16;
	if (Get(Source, 0) != 0x314d4c44 || Get(Source, 4) != 3 ||
	    Source.size() != PayloadStart + TileBytes)
		throw std::runtime_error("Invalid v3 lightmap file");
	std::vector<Bytes> Entries;
	for (size_t i = 0; i < Count; ++i)
		Entries.push_back(Slice(Source, 16 + i * 16, 16));
	auto Key = [](const Bytes& E) { return std::make_pair(Get(E, 0, 2), E[2]); };
	std::stable_sort(Entries.begin(), Entries.end(),
	                 [&](const Bytes& A, const Bytes& B) { return Key(A) < Key(B); });
	std::vector<Rows> Occupancy;
	std::vector<Bytes> Pages;
	Bytes Placements, Directory, Payload;
	for (size_t i = 0; i < Entries.size(); ++i)
	{
		const auto& E = Entries[i];
		if (i && Key(E) == Key(Entries[i - 1]))
			throw std::runtime_error("Duplicate lightmap key");
		const auto W = Get(E, 4, 2), H = Get(E, 6, 2), Offset = Get(E, 8), Size = Get(E, 12);
		if (!W || !H || uint64_t(Offset) + Size > TileBytes)
			throw std::runtime_error("Invalid lightmap tile");
		const auto P = Place(Occupancy, W, H, 15);
		while (Pages.size() < Occupancy.size())
			Pages.emplace_back(256 * 256 * 2, 0);
		if (P.Page != 255)
		{
			Bytes Raw = Slice(Source, PayloadStart + Offset, Size);
			if (E[3] == 1)
			{
				Bytes Decoded(W * H * 2);
				uLongf Length = Decoded.size();
				if (uncompress(Decoded.data(), &Length, Raw.data(), Raw.size()) != Z_OK ||
				    Length != Decoded.size())
					throw std::runtime_error("Invalid compressed lightmap tile");
				Raw.swap(Decoded);
			}
			else if (E[3])
				throw std::runtime_error("Unknown lightmap codec");
			Blit(Pages, P, Raw, W, H);
		}
		Placements.insert(Placements.end(), {P.Page, P.X, P.Y, 0});
	}
	EncodePages(Pages, Directory, Payload, Scratch, "atlas", Tools);
	Bytes Result;
	for (auto V :
	     {0x314d4c44u, 4u, Count, TileBytes, uint32_t(Pages.size()), uint32_t(Payload.size())})
		Put(Result, V);
	for (const auto& E : Entries)
		Result.insert(Result.end(), E.begin(), E.end());
	Result.insert(Result.end(), Placements.begin(), Placements.end());
	Result.insert(Result.end(), Directory.begin(), Directory.end());
	Result.insert(Result.end(), Source.begin() + PayloadStart, Source.end());
	Result.insert(Result.end(), Payload.begin(), Payload.end());
	return Result;
}

Bytes DynamicAtlas(const Bytes& Source, const fs::path& Scratch, const Encoders& Tools)
{
	const auto Count = Get(Source, 8), PayloadSize = Get(Source, 12);
	const uint64_t PayloadStart = 16 + uint64_t(Count) * 28;
	if (Get(Source, 0) != 0x31594e44 || Get(Source, 4) != 2 || Count > 65536 ||
	    Source.size() != PayloadStart + PayloadSize)
		throw std::runtime_error("Invalid dynamic lightmap file");
	std::vector<Bytes> Raw;
	for (size_t i = 0; i < Count; ++i)
		Raw.push_back(Slice(Source, 16 + i * 28, 28));
	auto Key = [](const Bytes& E) { return std::make_tuple(Get(E, 0, 2), E[2], E[3]); };
	std::stable_sort(Raw.begin(), Raw.end(),
	                 [&](const Bytes& A, const Bytes& B) { return Key(A) < Key(B); });
	struct Group
	{
		size_t Begin, End;
		unsigned Priority;
	};
	std::vector<Group> Groups;
	for (size_t Begin = 0, End; Begin < Raw.size(); Begin = End)
	{
		End = Begin + 1;
		while (End < Raw.size() && Get(Raw[Begin], 0, 2) == Get(Raw[End], 0, 2) &&
		       Raw[Begin][2] == Raw[End][2])
			++End;
		const auto& First = Raw[Begin];
		const unsigned Expected = First[23] == 3 || First[23] == 5 ? 2 : 8;
		if (End - Begin != Expected)
			throw std::runtime_error("Incomplete dynamic lightmap group");
		for (size_t i = Begin; i < End; ++i)
			if (Raw[i][3] != i - Begin || Get(Raw[i], 4) != Get(First, 4) ||
			    !std::equal(First.begin() + 16, First.end(), Raw[i].begin() + 16))
				throw std::runtime_error("Inconsistent dynamic lightmap group");
		const auto Effect = Get(First, 24);
		if (Effect > 255)
			throw std::runtime_error("Invalid dynamic light effect");
		const unsigned Priority = !Effect && (First[23] == 2 || First[23] == 7) ? 0
		                          : Expected == 2                               ? 1
		                          : !Effect                                     ? 2
		                                                                        : 3;
		Groups.push_back({Begin, End, Priority});
	}
	std::stable_sort(Groups.begin(), Groups.end(),
	                 [](const Group& A, const Group& B) { return A.Priority < B.Priority; });
	std::vector<Rows> Occupancy;
	std::vector<Bytes> Pages, Entries;
	size_t Accepted = 0, Varying = 0;
	for (const auto& G : Groups)
	{
		const auto W = Get(Raw[G.Begin], 4, 2), H = Get(Raw[G.Begin], 6, 2);
		if (W < 8 || W > 64 || H < 8 || H > 64)
			continue;
		auto Trial = Occupancy;
		std::vector<Position> Positions;
		for (size_t i = G.Begin; i < G.End; ++i)
		{
			const auto Offset = Get(Raw[i], 8), Size = Get(Raw[i], 12);
			if (Size != W * H * 2 || uint64_t(Offset) + Size > PayloadSize)
				throw std::runtime_error("Invalid dynamic tile payload");
			const auto P = Place(Trial, W, H, 24);
			if (P.Page == 255)
				break;
			Positions.push_back(P);
		}
		if (Positions.size() != G.End - G.Begin)
			continue;
		Occupancy.swap(Trial);
		++Accepted;
		bool Different = false;
		const auto First = Slice(Source, PayloadStart + Get(Raw[G.Begin], 8), W * H * 2);
		for (size_t i = G.Begin; i < G.End; ++i)
		{
			const auto& E = Raw[i];
			const auto P = Positions[i - G.Begin];
			const auto Pixels = Slice(Source, PayloadStart + Get(E, 8), W * H * 2);
			Different |= Pixels != First;
			Blit(Pages, P, Pixels, W, H);
			Bytes Entry{E[0], E[1], E[2], E[3], P.Page, P.X, P.Y, E[20]};
			Put(Entry, Get(E, 16));
			Entry.insert(Entry.end(), {E[21], E[22], E[23], uint8_t(Get(E, 24))});
			Entries.push_back(Entry);
		}
		Varying += Different;
	}
	if (Accepted && !Varying)
		throw std::runtime_error("All dynamic variants are identical");
	std::sort(Entries.begin(), Entries.end(),
	          [&](const Bytes& A, const Bytes& B) { return Key(A) < Key(B); });
	Bytes Directory, Payload, Result;
	EncodePages(Pages, Directory, Payload, Scratch, "dynamic", Tools);
	for (auto V : {0x314d4444u, 1u, uint32_t(Entries.size()), uint32_t(Pages.size()),
	               uint32_t(Payload.size()), 0u})
		Put(Result, V);
	for (const auto& E : Entries)
		Result.insert(Result.end(), E.begin(), E.end());
	Result.insert(Result.end(), Directory.begin(), Directory.end());
	Result.insert(Result.end(), Payload.begin(), Payload.end());
	return Result;
}
} // namespace

void Lightmaps(const fs::path& Static, const fs::path& Dynamic, const fs::path& Output,
               const fs::path& Scratch, const Encoders& Tools)
{
	fs::create_directories(Scratch);
	const auto StaticData = StaticAtlas(Read(Static), Scratch, Tools);
	const auto DynamicData = DynamicAtlas(Read(Dynamic), Scratch, Tools);
	auto Ddm = Output;
	Ddm.replace_extension(".ddm");
	Write(Output, StaticData);
	Write(Ddm, DynamicData);
}
} // namespace DCCook

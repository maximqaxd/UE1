#include "EnginePrivate.h"
#include "UnDCVMU.h"
#include "UnDCState.h"
#if defined(PLATFORM_DREAMCAST)
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <zlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// ASCII pixel art: 32x32 monochrome Unreal-inspired U/ring, centered on 48x32.
static const char* const Logo[32] = {
	"............########............", "........###############.........",
	"......#####..........#####......", ".....###................###.....",
	"....###..................###....", "...###....................###...",
	"..###......................###..", "..##..###............###....##..",
	".##...#####........#####.....##.", ".##....######....######......##.",
	"##......######..######........##", "##.......#####..#####.........##",
	"##.......#####..#####.........##", "##.......#####..#####.........##",
	"##.......#####..#####.........##", "##.......#####..#####.........##",
	"##.......#####..#####.........##", "##.......#####..#####.........##",
	"##.......#####..#####.........##", "##.......#####..#####.........##",
	"##.......#####..#####.........##", ".##......############........##.",
	".##.....#############........##.", "..##...####..########.......##..",
	"..###.........########.....###..", "...###..........######....###...",
	"....###...........###....###....", ".....###................###.....",
	"......#####..........#####......", "........###############.........",
	"...........##########...........", "................................"};

void DCVMUStartup()
{
	unsigned char Bits[192] __attribute__((aligned(4))) = {};
	for (int Y = 0; Y < 32; ++Y)
		for (int X = 0; X < 32; ++X)
			if (Logo[Y][X] == '#')
				Bits[Y * 6 + (X + 8) / 8] |= 0x80 >> ((X + 8) & 7);
	for (int Index = 0;; ++Index)
	{
		maple_device_t* Dev = maple_enum_type(Index, MAPLE_FUNC_LCD);
		if (!Dev)
			break;
		vmu_draw_lcd_rotated(Dev, Bits);
	}
}

struct FVMUHeader
{
	char Magic[8];
	DWORD Version, Sequence, SaveBytes, StateBytes, CRC;
	char Map[64];
};
UBOOL DCVMUCanSave(char* Error)
{
	maple_device_t* Dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!Dev)
	{
		appStrcpy(Error, "No VMU memory card connected");
		return 0;
	}
	if (vmufs_free_blocks(Dev) < 2)
	{
		appStrcpy(Error, "VMU has no space for a replacement save; previous save kept");
		return 0;
	}
	return 1;
}
enum
{
	VMUMaxPayload = 100 * 1024,
	VMUMaxSave = 8 * 1024 * 1024,
	VMUMaxState = 64 * 1024
};
static void VMUName(char* Name, int Slot, int Bank)
{
	sprintf(Name, "UE1S%02d%c", Slot, 'A' + Bank);
}
static UBOOL VMUHeaderValid(const vmu_pkg_t& PackageInfo)
{
	if (strcmp(PackageInfo.app_id, "UNREAL_DC") || PackageInfo.data_len < (int)sizeof(FVMUHeader))
		return 0;
	const FVMUHeader* Header = (const FVMUHeader*)PackageInfo.data;
	if (memcmp(Header->Magic, "UE1SAVE", 8) || (Header->Version != 1 && Header->Version != 2) || !Header->SaveBytes ||
		Header->SaveBytes > VMUMaxSave || Header->StateBytes > VMUMaxState)
		return 0;
	int Index = 0;
	for (; Index < 63 && Header->Map[Index]; ++Index)
	{
		char Character = Header->Map[Index];
		if (!((Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
			  (Character >= '0' && Character <= '9') || Character == '_'))
			return 0;
	}
	return Index > 0 && Header->Map[Index] == 0;
}
static int VMURead(maple_device_t* Dev, int Slot, int Bank, void** Data, int* Bytes,
				   vmu_pkg_t* PackageInfo)
{
	char Name[16];
	VMUName(Name, Slot, Bank);
	*Data = NULL;
	if (vmufs_read(Dev, Name, Data, Bytes) < 0)
		return 0;
	if (*Bytes < 128 || vmu_pkg_parse((uint8_t*)*Data, *Bytes, PackageInfo) < 0 ||
		!VMUHeaderValid(*PackageInfo))
	{
		free(*Data);
		*Data = NULL;
		return 0;
	}
	return 1;
}
static int VMULatest(maple_device_t* Dev, int Slot, DWORD& Sequence)
{
	int Best = -1;
	Sequence = 0;
	for (int BankIndex = 0; BankIndex < 2; ++BankIndex)
	{
		void* BankData = NULL;
		int ChunkBytes;
		vmu_pkg_t PackageInfo = {};
		if (VMURead(Dev, Slot, BankIndex, &BankData, &ChunkBytes, &PackageInfo))
		{
			DWORD CandidateSequence = ((FVMUHeader*)PackageInfo.data)->Sequence;
			if (Best < 0 || CandidateSequence > Sequence)
			{
				Best = BankIndex;
				Sequence = CandidateSequence;
			}
			free(BankData);
		}
	}
	return Best;
}
void DCVMURefreshMenus()
{
	char Names[9][100];
	maple_device_t* Dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	for (int Slot = 0; Slot < 9; ++Slot)
	{
		strcpy(Names[Slot], "..Empty..");
		if (!Dev)
			continue;
		DWORD Sequence;
		int Bank = VMULatest(Dev, Slot, Sequence);
		void* Data = NULL;
		int Bytes;
		vmu_pkg_t PackageInfo = {};
		if (Bank >= 0 && VMURead(Dev, Slot, Bank, &Data, &Bytes, &PackageInfo))
		{
			snprintf(Names[Slot], 100, "VMU: %s", ((FVMUHeader*)PackageInfo.data)->Map);
			free(Data);
		}
	}
	// Refresh cooked menu defaults without relying on a writable CD INI file.
	for (TObjectIterator<UClass> MenuClass; MenuClass; ++MenuClass)
	{
		if (appStricmp(MenuClass->GetName(), "UnrealSlotMenu") &&
			appStricmp(MenuClass->GetName(), "UnrealSaveMenu") &&
			appStricmp(MenuClass->GetName(), "UnrealLoadMenu"))
			continue;
		for (TFieldIterator<UStringProperty> SlotProperty(*MenuClass); SlotProperty; ++SlotProperty)
		{
			if (appStricmp(SlotProperty->GetName(), "SlotNames") || SlotProperty->ArrayDim != 9)
				continue;
			for (int Slot = 0; Slot < 9; ++Slot)
			{
				char* Destination = (char*)MenuClass->GetDefaultObject() + SlotProperty->Offset +
					Slot * SlotProperty->GetElementSize();
				appStrncpy(Destination, Names[Slot], SlotProperty->GetElementSize());
			}
		}
	}
}
UBOOL DCVMUSave(INT Slot, const char* File, const char* Map, const TArray<BYTE>& State, char* Error, UBOOL Canonical)
{
	if (Slot < 0 || Slot > 9)
	{
		appStrcpy(Error, "VMU slot must be 0-9");
		return 0;
	}
	maple_device_t* Dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!Dev)
	{
		appStrcpy(Error, "No VMU memory card connected");
		return 0;
	}
	FILE* Snapshot = fopen(File, "rb");
	if (!Snapshot)
	{
		appStrcpy(Error, "Cannot open full-world snapshot");
		return 0;
	}
	if (fseek(Snapshot, 0, SEEK_END))
	{
		fclose(Snapshot);
		appStrcpy(Error, "Cannot seek RAM save snapshot; previous save kept");
		return 0;
	}
	long Size = ftell(Snapshot);
	if (fseek(Snapshot, 0, SEEK_SET))
	{
		fclose(Snapshot);
		appStrcpy(Error, "Cannot rewind RAM save snapshot; previous save kept");
		return 0;
	}
	if (Size <= 0 || Size > VMUMaxSave || State.Num() > VMUMaxState)
	{
		fclose(Snapshot);
		appStrcpy(Error, "Full save exceeds supported snapshot size");
		return 0;
	}
	BYTE* Payload = (BYTE*)malloc(VMUMaxPayload);
	if (!Payload)
	{
		fclose(Snapshot);
		appStrcpy(Error, "Not enough RAM to compress save");
		return 0;
	}
	FVMUHeader Header = {};
	memcpy(Header.Magic, "UE1SAVE", 8);
	Header.Version = Canonical ? 2 : 1;
	Header.SaveBytes = Size;
	Header.StateBytes = State.Num();
	appStrncpy(Header.Map, Map, sizeof(Header.Map));
	DWORD Previous = 0;
	int Bank = VMULatest(Dev, Slot, Previous);
	Header.Sequence = Previous + 1;
	z_stream Stream = {};
	int Result = deflateInit(&Stream, Z_BEST_COMPRESSION);
	if (Result != Z_OK)
	{
		free(Payload);
		fclose(Snapshot);
		appStrcpy(Error, "Cannot initialize save compression");
		return 0;
	}
	Stream.next_out = Payload + sizeof(Header);
	Stream.avail_out = VMUMaxPayload - sizeof(Header);
	BYTE Buffer[4096];
	uLong CRC = crc32(0, NULL, 0);
	int StatePos = 0;
	long ReadBytes = 0;
	bool ReadFailed = false;
	while (Result == Z_OK)
	{
		int ChunkBytes = 0;
		if (StatePos < State.Num())
		{
			ChunkBytes = Min(4096, State.Num() - StatePos);
			memcpy(Buffer, &State(StatePos), ChunkBytes);
			StatePos += ChunkBytes;
		}
		else if (ReadBytes < Size)
		{
			const size_t Wanted = Min((long)sizeof(Buffer), Size - ReadBytes);
			ChunkBytes = fread(Buffer, 1, Wanted, Snapshot);
			ReadBytes += ChunkBytes;
			if ((size_t)ChunkBytes != Wanted)
			{
				ReadFailed = true;
				break;
			}
		}
		CRC = crc32(CRC, Buffer, ChunkBytes);
		Stream.next_in = Buffer;
		Stream.avail_in = ChunkBytes;
		const int Flush = StatePos == State.Num() && ReadBytes == Size ? Z_FINISH : Z_NO_FLUSH;
		do
		{
			Result = deflate(&Stream, Flush);
		} while (Result == Z_OK && Stream.avail_out && (Stream.avail_in || Flush == Z_FINISH));
		if (!Stream.avail_out)
			break;
	}
	int PayloadBytes = sizeof(Header) + Stream.total_out;
	const uLong Consumed = Stream.total_in;
	const unsigned SpaceLeft = Stream.avail_out;
	debugf("DCVMU pack world=%ld state=%d read=%ld consumed=%lu packed=%d remaining=%u z=%d read_error=%d",
		Size, State.Num(), ReadBytes, Consumed, PayloadBytes, SpaceLeft, Result, (int)ReadFailed);
	deflateEnd(&Stream);
	fclose(Snapshot);
	if (ReadFailed || Result != Z_STREAM_END || ReadBytes != Size || Consumed != Size + State.Num())
	{
		free(Payload);
		if (ReadFailed || ReadBytes != Size)
			appStrcpy(Error, "RAM snapshot read failed; previous save kept");
		else if (!SpaceLeft && Result != Z_STREAM_END)
			appStrcpy(Error, "Full save exceeds 100 KB VMU payload budget; previous save kept");
		else
			appSprintf(Error, "Save compression failed (zlib %d); previous save kept", Result);
		return 0;
	}
	Header.CRC = CRC;
	memcpy(Payload, &Header, sizeof(Header));
	uint8_t Icon[512] = {};
	for (int Y = 0; Y < 32; ++Y)
		for (int X = 0; X < 32; ++X)
			if (Logo[Y][X] == '#')
				Icon[Y * 16 + X / 2] |= (X & 1) ? 1 : 16;
	vmu_pkg_t PackageInfo = {};
	strcpy(PackageInfo.desc_short, "Unreal save");
	snprintf(PackageInfo.desc_long, sizeof(PackageInfo.desc_long), "%s slot %d", Map, Slot);
	strcpy(PackageInfo.app_id, "UNREAL_DC");
	PackageInfo.icon_cnt = 1;
	PackageInfo.icon_data = Icon;
	PackageInfo.icon_pal[0] = 0xffff;
	PackageInfo.icon_pal[1] = 0xf000;
	PackageInfo.eyecatch_type = VMUPKG_EC_NONE;
	PackageInfo.data = Payload;
	PackageInfo.data_len = PayloadBytes;
	uint8_t* Package = NULL;
	int PackageBytes = 0;
	Result = vmu_pkg_build(&PackageInfo, &Package, &PackageBytes);
	free(Payload);
	if (Result < 0)
	{
		appStrcpy(Error, "Cannot build VMU save header");
		return 0;
	}
	int FreeBlocks = vmufs_free_blocks(Dev), RequiredBlocks = (PackageBytes + 511) / 512;
	// Never erase the valid bank to make room for its replacement.
	if (FreeBlocks < RequiredBlocks)
	{
		free(Package);
		appSprintf(Error, "VMU needs %d free blocks; has %d. Previous save kept", RequiredBlocks,
				   Max(0, FreeBlocks));
		return 0;
	}
	char Name[16];
	VMUName(Name, Slot, Bank == 0 ? 1 : 0);
	Result = vmufs_write(Dev, Name, Package, PackageBytes, VMUFS_OVERWRITE);
	void* Check = NULL;
	int CheckBytes = 0;
	vmu_pkg_t Checked = {};
	const UBOOL Valid = Result == 0 &&
						VMURead(Dev, Slot, Bank == 0 ? 1 : 0, &Check, &CheckBytes, &Checked) &&
						Checked.data_len == PayloadBytes;
	// Compare the complete stored package, including payload and KOS CRC.
	UBOOL Verified = Valid && CheckBytes >= PackageBytes && !memcmp(Check, Package, PackageBytes);
	free(Check);
	free(Package);
	if (!Verified)
	{
		appStrcpy(Error, "VMU write/readback failed; previous save kept");
		return 0;
	}
	if (Bank >= 0)
	{
		VMUName(Name, Slot, Bank);
		vmufs_delete(Dev, Name);
	}
	debugf("DCVMU saved slot=%d raw=%ld compressed=%d blocks=%d", Slot, Size, PayloadBytes,
		   RequiredBlocks);
	return 1;
}

UBOOL DCVMULoad(INT Slot, char* Map, char* File, TArray<BYTE>& State, char* Error)
{
	if (Slot < 0 || Slot > 9)
	{
		appStrcpy(Error, "VMU slot must be 0-9");
		return 0;
	}
	maple_device_t* Dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!Dev)
	{
		appStrcpy(Error, "No VMU memory card connected");
		return 0;
	}
	DWORD Sequence;
	int Bank = VMULatest(Dev, Slot, Sequence);
	void* Data = NULL;
	int Bytes = 0;
	vmu_pkg_t PackageInfo = {};
	if (Bank < 0 || !VMURead(Dev, Slot, Bank, &Data, &Bytes, &PackageInfo))
	{
		appStrcpy(Error, "No valid Unreal save in this VMU slot");
		return 0;
	}
	FVMUHeader Header;
	memcpy(&Header, PackageInfo.data, sizeof(Header));
	appSprintf(File, "/ram/%s.%s", Header.Map, Header.Version == 2 ? "dsv" : "usa");
	FILE* Snapshot = fopen(File, "wb");
	if (!Snapshot)
	{
		free(Data);
		appStrcpy(Error, "Cannot create RAM save snapshot");
		return 0;
	}
	State.Empty();
	State.Add(Header.StateBytes);
	z_stream Stream = {};
	int Result = inflateInit(&Stream);
	BYTE Buffer[4096];
	Stream.next_in = (Bytef*)PackageInfo.data + sizeof(Header);
	Stream.avail_in = PackageInfo.data_len - sizeof(Header);
	DWORD Total = 0;
	uLong CRC = crc32(0, NULL, 0);
	bool ValidStream = Result == Z_OK;
	while (ValidStream && Result != Z_STREAM_END)
	{
		Stream.next_out = Buffer;
		Stream.avail_out = sizeof(Buffer);
		Result = inflate(&Stream, Z_NO_FLUSH);
		int ChunkBytes = sizeof(Buffer) - Stream.avail_out;
		if ((Result != Z_OK && Result != Z_STREAM_END) ||
			Total + ChunkBytes > Header.StateBytes + Header.SaveBytes)
		{
			ValidStream = false;
			break;
		}
		CRC = crc32(CRC, Buffer, ChunkBytes);
		int StateChunkBytes =
			Total < Header.StateBytes ? Min((DWORD)ChunkBytes, Header.StateBytes - Total) : 0;
		if (StateChunkBytes)
			memcpy(&State(Total), Buffer, StateChunkBytes);
		if (ChunkBytes > StateChunkBytes &&
			fwrite(Buffer + StateChunkBytes, 1, ChunkBytes - StateChunkBytes, Snapshot) !=
				(size_t)(ChunkBytes - StateChunkBytes))
		{
			ValidStream = false;
			break;
		}
		Total += ChunkBytes;
	}
	if (Result == Z_STREAM_END && Stream.avail_in)
		ValidStream = false;
	inflateEnd(&Stream);
	if (fclose(Snapshot))
		ValidStream = false;
	free(Data);
	if (!ValidStream || Total != Header.StateBytes + Header.SaveBytes || CRC != Header.CRC)
	{
		remove(File);
		State.Empty();
		appStrcpy(Error, "Save decompression, length or checksum failed");
		return 0;
	}
	appStrcpy(Map, Header.Map);
	if (Header.Version == 2)
	{
		char Baseline[256];
		appSprintf(Baseline, "/cd/Maps/%s.dsb", Map);
		if (!DCCheckWorldState(Map, Baseline, File, Error))
		{
			remove(File);
			State.Empty();
			return 0;
		}
	}
	return 1;
}
#endif

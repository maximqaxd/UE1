#include "EnginePrivate.h"
#include "UnDCState.h"
#include "UnDCLoading.h"

#if defined(PLATFORM_DREAMCAST) || defined(DC_RESOURCE_COOKER)
#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <zlib.h>
#include "DCStateDelta.h"

namespace
{
enum { MaxObjects = 65536, MaxNames = 65536, MaxBody = 16*1024*1024, MaxPacked = 96*1024 };

struct File
{
	FILE* Handle;
	File(const char* Path, const char* Mode) : Handle(fopen(Path, Mode))
	{
		if (!Handle) throw "Cannot open world-state file";
	}
	~File() { if (Handle) fclose(Handle); }
	void Close()
	{
		FILE* Closing = Handle;
		Handle = NULL;
		if (fclose(Closing)) throw "Cannot close world-state file";
	}
};

struct IO
{
	bool Loading;
	IO(bool InLoading) : Loading(InLoading) {}
	virtual ~IO() {}
	virtual void Bytes(void* Data, unsigned Size) = 0;
	void Number(unsigned& Value) { Bytes(&Value, 4); }
	void Text(std::string& Value)
	{
		unsigned Size = Value.size();
		Number(Size);
		if (Size > 1024) throw "World-state identifier too long";
		if (Loading) Value.resize(Size);
		if (Size) Bytes(&Value[0], Size);
		if (Value.find('\0') != std::string::npos) throw "Invalid world-state identifier";
	}
};

struct RawIO : IO
{
	FILE* Stream;
	RawIO(FILE* InStream, bool Load) : IO(Load), Stream(InStream) {}
	void Bytes(void* Data, unsigned Size)
	{
		if ((Loading ? fread(Data, 1, Size, Stream) : fwrite(Data, 1, Size, Stream)) != Size)
			throw "World-state file truncated or write failed";
	}
};

struct ZipIO : IO
{
	FILE* Stream;
	z_stream Codec;
	BYTE Buffer[1024];
	unsigned Written;
	unsigned OutputLimit;
	bool Ended;
	long InputStart;
	ZipIO(FILE* InStream, bool Load, unsigned Limit=MaxPacked)
		: IO(Load), Stream(InStream), Codec(), Written(0), OutputLimit(Limit), Ended(false), InputStart(Load ? ftell(InStream) : 0)
	{
		if (InputStart<0) throw "Cannot locate world-state stream";
		if ((Load ? inflateInit(&Codec) : deflateInit(&Codec, 9)) != Z_OK)
			throw "Cannot initialize world-state compression";
	}
	~ZipIO() { if (Loading) inflateEnd(&Codec); else deflateEnd(&Codec); }
	void Output(unsigned Count)
	{
		if (Count > OutputLimit-Written) throw "World state exceeds compression budget; previous save kept";
		if (fwrite(Buffer, 1, Count, Stream) != Count) throw "World-state write failed";
		Written += Count;
	}
	void Bytes(void* Data, unsigned Size)
	{
		if (!Size) return;
		if (Ended) throw "Unexpected end of world state";
		if (!Loading)
		{
			Codec.next_in = (Bytef*)Data;
			Codec.avail_in = Size;
			while (Codec.avail_in)
			{
				Codec.next_out = Buffer;
				Codec.avail_out = sizeof(Buffer);
				if (deflate(&Codec, Z_NO_FLUSH) != Z_OK) throw "World-state compression failed";
				Output(sizeof(Buffer)-Codec.avail_out);
			}
			return;
		}
		Codec.next_out = (Bytef*)Data;
		Codec.avail_out = Size;
		while (Codec.avail_out)
		{
			if (!Codec.avail_in)
			{
				Codec.avail_in = fread(Buffer, 1, sizeof(Buffer), Stream);
				Codec.next_in = Buffer;
				if (!Codec.avail_in) throw "Truncated compressed world state";
			}
			int Result = inflate(&Codec, Z_NO_FLUSH);
			if (Result == Z_STREAM_END)
			{
				Ended = true;
				if (Codec.avail_out) throw "Short compressed world state";
				break;
			}
			if (Result != Z_OK) throw "Invalid compressed world state";
		}
	}
	void Finish(bool EntireFile=true)
	{
		if (!Loading)
		{
			int Result;
			do
			{
				Codec.next_out = Buffer;
				Codec.avail_out = sizeof(Buffer);
				Result = deflate(&Codec, Z_FINISH);
				if (Result != Z_OK && Result != Z_STREAM_END) throw "Cannot finish world state";
				Output(sizeof(Buffer)-Codec.avail_out);
			} while (Result != Z_STREAM_END);
		}
		else
		{
			while (!Ended)
			{
				if (!Codec.avail_in)
				{
					Codec.avail_in = fread(Buffer, 1, sizeof(Buffer), Stream);
					Codec.next_in = Buffer;
					if (!Codec.avail_in) throw "Missing world-state checksum";
				}
				BYTE Extra;
				Codec.next_out = &Extra;
				Codec.avail_out = 1;
				int Result = inflate(&Codec, Z_NO_FLUSH);
				if (!Codec.avail_out) throw "Trailing world-state payload";
				if (Result == Z_STREAM_END) Ended = true;
				else if (Result != Z_OK) throw "Invalid world-state checksum";
			}
			if (EntireFile)
			{
				// Dreamcast's file pool wraps seek/tell/read, not libc FILE macros.
				// Check exact compressed length without passing its opaque handle to fgetc.
				if (fseek(Stream, 0, SEEK_END)) throw "Cannot verify world-state length";
				long End = ftell(Stream);
				if (End<InputStart || Codec.avail_in || (uLong)(End-InputStart)!=Codec.total_in)
					throw "Trailing world-state stream";
			}
		}
	}
};

struct Entry
{
	std::string Path, Class;
	UObject* Object;
	bool Needed;
	Entry() : Object(NULL), Needed(false) {}
};

std::string FoldCase(std::string Text)
{
	for (unsigned i=0; i<Text.size(); ++i) Text[i]=appToLower(Text[i]);
	return Text;
}

UObject* FindExact(UClass* Class, const std::string& Path)
{
	UObject* Found = GObj.FindObject(Class, ANY_PACKAGE, Path.c_str(), Class != NULL);
	if (Found && !appStricmp(Found->GetPathName(), Path.c_str())) return Found;
	// ResolveName treats dotted outers as packages, not UClass/UState objects.
	// Script frames can refer to states/functions nested underneath a class.
	for (TObjectIterator<UObject> It; It; ++It)
		if ((!Class || It->GetClass() == Class) && !appStricmp(It->GetPathName(), Path.c_str()))
			return *It;
	return NULL;
}

struct Catalog
{
	ULevel* Level;
	std::vector<std::string> Names;
	std::vector<Entry> Objects;
	std::map<std::string, unsigned> NameIds, ObjectIds;
	std::vector<unsigned> Queue;
	bool Frozen;
	Catalog(ULevel* InLevel) : Level(InLevel), Frozen(false) { Objects.resize(1); }
	static std::string Key(const Entry& E) { return FoldCase(E.Class+"|"+E.Path); }
	unsigned Name(FName N)
	{
		std::string Text = *N;
		for (unsigned i=0; i<Text.size(); ++i) Text[i] = appToLower(Text[i]);
		std::map<std::string,unsigned>::iterator It = NameIds.find(Text);
		if (It != NameIds.end()) return It->second;
		if (Frozen || Names.size() >= MaxNames) throw "World-state name catalog changed";
		unsigned Id = Names.size();
		Names.push_back(Text);
		NameIds[Text] = Id;
		return Id;
	}
	unsigned Object(UObject* Obj)
	{
		if (!Obj || (Obj->GetFlags() & RF_Transient)) return 0;
		Entry E;
		E.Path = Obj->GetPathName();
		E.Class = Obj->GetClass()->GetPathName();
		std::string K = Key(E);
		std::map<std::string,unsigned>::iterator It = ObjectIds.find(K);
		unsigned Id;
		if (It == ObjectIds.end())
		{
			if (Frozen || Objects.size() >= MaxObjects) throw "World-state object catalog changed";
			Id = Objects.size();
			Objects.push_back(E);
			ObjectIds[K] = Id;
		}
		else Id = It->second;
		Objects[Id].Object = Obj;
		if (Obj->IsIn(Level->GetParent()) && !Objects[Id].Needed)
		{
			if (Frozen || Obj->IsA(UClass::StaticClass)) throw "Unsupported local class in world state";
			if (Obj->IsA(UTexture::StaticClass) || Obj->IsA(UMesh::StaticClass) ||
				Obj->IsA(USound::StaticClass) || Obj->IsA(UMusic::StaticClass))
				throw "Map-local texture, mesh or audio state needs a resource adapter; previous save kept";
			Objects[Id].Needed = true;
			Queue.push_back(Id);
			Object(Obj->GetParent());
			Object(Obj->GetClass());
		}
		return Id;
	}
	void Transfer(IO& Ar, unsigned FirstName=0, unsigned FirstObject=1)
	{
		unsigned Count = Names.size();
		Ar.Number(Count);
		if (Count > MaxNames || Count<FirstName) throw "Too many state names";
		if (Ar.Loading) { Names.resize(Count); if (!FirstName) NameIds.clear(); }
		for (unsigned i=FirstName; i<Count; ++i)
		{
			Ar.Text(Names[i]);
			if (Ar.Loading && !NameIds.insert(std::make_pair(Names[i], i)).second)
				throw "Duplicate state name";
		}
		Count = Objects.size();
		Ar.Number(Count);
		if (!Count || Count > MaxObjects || Count<FirstObject) throw "Invalid state object count";
		if (Ar.Loading) { Objects.resize(Count); if (FirstObject==1) ObjectIds.clear(); }
		for (unsigned i=FirstObject; i<Count; ++i)
		{
			Ar.Text(Objects[i].Path);
			Ar.Text(Objects[i].Class);
			if (Ar.Loading && !ObjectIds.insert(std::make_pair(Key(Objects[i]), i)).second)
				throw "Duplicate state object";
		}
	}
	UObject* Resolve(unsigned Id)
	{
		if (!Id) return NULL;
		if (Id >= Objects.size()) throw "Invalid world-state object reference";
		Entry& E = Objects[Id];
		if (E.Object) return E.Object;
		UClass* Class = GObj.LoadClass(UObject::StaticClass, NULL, E.Class.c_str(), NULL, LOAD_NoFail, NULL);
		E.Object = FindExact(Class, E.Path);
		if (!E.Object)
		{
			std::string Prefix = std::string(Level->GetParent()->GetName())+".";
			if (FoldCase(E.Path).compare(0, Prefix.size(), FoldCase(Prefix)) == 0)
			{
				if (!E.Needed) throw "Reference to absent local state object";
				unsigned Dot = E.Path.find_last_of('.');
				std::string ParentPath = E.Path.substr(0, Dot);
				UObject* Parent = FindExact(NULL, ParentPath);
				if (!Parent)
				{
					unsigned ParentId = 0;
					for (unsigned i=1; i<Objects.size(); ++i)
						if (Objects[i].Needed && !appStricmp(Objects[i].Path.c_str(), ParentPath.c_str()))
						{
							if (ParentId) throw "Ambiguous world-state object parent";
							ParentId = i;
						}
					if (ParentId) Parent = Resolve(ParentId);
				}
				if (!Parent) throw "Missing world-state object parent";
				E.Object = GObj.ConstructObject(Class, Parent, FName(E.Path.substr(Dot+1).c_str()), 0);
			}
			else E.Object = GObj.LoadObject(Class, NULL, E.Path.c_str(), NULL, LOAD_NoFail, NULL);
		}
		if (!E.Object) throw "Cannot resolve world-state object";
		return E.Object;
	}
};

struct BodyArchive : FArchive
{
	Catalog& Symbols;
	IO* Stream;
	unsigned Position, Limit;
	BodyArchive(Catalog& InSymbols, IO* InStream, unsigned InLimit=MaxBody)
		: Symbols(InSymbols), Stream(InStream), Position(0), Limit(InLimit)
	{
		ArIsLoading = Stream && Stream->Loading;
		ArIsSaving = !ArIsLoading;
		ArForEdit = 0;
	}
	UBOOL IsStateArchive() const { return 1; }
	INT Tell() { return Position; }
	INT MapName(FName* N) { return Symbols.Name(*N); }
	INT MapObject(UObject* O) { return Symbols.Object(O); }
	FArchive& Serialize(void* Data, INT Size)
	{
		if (Size < 0 || (unsigned)Size > Limit-Position) throw "Invalid world-state body extent";
		if (Stream) Stream->Bytes(Data, Size);
		Position += Size;
		return *this;
	}
	FArchive& operator<<(FName& N)
	{
		unsigned Id = ArIsSaving ? Symbols.Name(N) : 0;
		Serialize(&Id, 4);
		if (ArIsLoading)
		{
			if (Id >= Symbols.Names.size()) throw "Invalid world-state name reference";
			N = FName(Symbols.Names[Id].c_str());
		}
		return *this;
	}
	FArchive& operator<<(UObject*& O)
	{
		unsigned Id = ArIsSaving ? Symbols.Object(O) : 0;
		Serialize(&Id, 4);
		if (ArIsLoading) O = Symbols.Resolve(Id);
		return *this;
	}
};

struct Record
{
	unsigned Id, Flags, Size, Offset, Packed;
};

void Header(IO& Ar, const char* Magic, ULevel* Level)
{
	char Tag[8];
	appMemcpy(Tag, Magic, 8);
	Ar.Bytes(Tag, 8);
	if (appMemcmp(Tag, Magic, 8)) throw "Unsupported world-state format";
	std::string Map = Level->GetParent()->GetName();
	Ar.Text(Map);
	if (appStricmp(Map.c_str(), Level->GetParent()->GetName())) throw "World-state map mismatch";
}

unsigned FileCRC(FILE* Stream)
{
	long Position = ftell(Stream);
	if (Position<0 || fseek(Stream, 0, SEEK_END)) throw "Cannot size state baseline";
	long Length = ftell(Stream);
	if (Length<0 || fseek(Stream, 0, SEEK_SET)) throw "Cannot seek state baseline";
	BYTE Buffer[1024];
	unsigned CRC = crc32(0, NULL, 0);
	while (Length)
	{
		unsigned Count = Min((long)sizeof(Buffer), Length);
		if (fread(Buffer, 1, Count, Stream)!=Count) throw "Cannot read state baseline";
		CRC = crc32(CRC, Buffer, Count);
		Length -= Count;
	}
	if (fseek(Stream, Position, SEEK_SET)) throw "Cannot verify state baseline";
	return CRC;
}

std::vector<Record> ScanBaseline(FILE* Stream, Catalog& Symbols)
{
	RawIO Ar(Stream, true);
	Header(Ar, "DCBASE04", Symbols.Level);
	Symbols.Transfer(Ar);
	unsigned Count;
	Ar.Number(Count);
	if (Count > MaxObjects) throw "Invalid baseline record count";
	std::vector<Record> Records(Symbols.Objects.size());
	for (unsigned i=0; i<Count; ++i)
	{
		Record R = {};
		Ar.Number(R.Id); Ar.Number(R.Flags); Ar.Number(R.Size); Ar.Number(R.Packed);
		if (!R.Id || R.Id >= Records.size() || Records[R.Id].Id || R.Size > MaxBody)
			throw "Invalid baseline record";
		if (!R.Packed || R.Packed>MaxBody) throw "Invalid packed baseline length";
		R.Offset = ftell(Stream);
		Records[R.Id] = R;
		if (fseek(Stream, R.Packed, SEEK_CUR)) throw "Invalid baseline extent";
	}
	long End = ftell(Stream);
	fseek(Stream, 0, SEEK_END);
	if (End != ftell(Stream)) throw "Baseline length mismatch";
	return Records;
}

std::vector<Record> Collect(Catalog& Symbols)
{
	Symbols.Object(Symbols.Level);
	std::vector<Record> Records;
	for (unsigned i=0; i<Symbols.Queue.size(); ++i)
	{
		unsigned Id = Symbols.Queue[i];
		UObject* Obj = Symbols.Objects[Id].Object;
		BodyArchive Ar(Symbols, NULL);
		Obj->Serialize(Ar);
		Record R = {Id, Obj->GetFlags() & RF_Load, Ar.Position, 0, 0};
		Records.push_back(R);
	}
	Symbols.Frozen = true;
	return Records;
}

struct DeltaIO : IO
{
	IO& Target;
	FILE* Baseline;
	ZipIO* Decoder;
	unsigned Remaining;
	DeltaIO(IO& InTarget, FILE* InBaseline, const Record& R)
		: IO(InTarget.Loading), Target(InTarget), Baseline(InBaseline), Decoder(NULL), Remaining(R.Id ? R.Size : 0)
	{
		if (Remaining && fseek(Baseline, R.Offset, SEEK_SET)) throw "Cannot seek state baseline";
		if (Remaining) Decoder = new ZipIO(Baseline, true);
	}
	~DeltaIO() { delete Decoder; }
	void Bytes(void* Data, unsigned Size)
	{
		BYTE Buffer[1024], Base[1024];
		BYTE* Cursor = (BYTE*)Data;
		while (Size)
		{
			unsigned Count = Min(Size, (unsigned)sizeof(Buffer));
			unsigned Shared = Min(Count, Remaining);
			appMemset(Base, 0, Count);
			if (Shared) Decoder->Bytes(Base, Shared);
			Remaining -= Shared;
			if (Loading) Target.Bytes(Buffer, Count);
			for (unsigned i=0; i<Count; ++i)
				if (Loading) Cursor[i] = Buffer[i]^Base[i]; else Buffer[i] = Cursor[i]^Base[i];
			if (!Loading) Target.Bytes(Buffer, Count);
			Cursor += Count; Size -= Count;
		}
	}
};

struct MemoryIO : IO
{
	std::vector<BYTE>& Data;
	unsigned Position;
	MemoryIO(std::vector<BYTE>& InData, bool Load) : IO(Load), Data(InData), Position(0) {}
	void Bytes(void* Value, unsigned Size)
	{
		if (Loading)
		{
			if (Size>Data.size()-Position) throw "Truncated state record";
			if (Size) appMemcpy(Value, &Data[Position], Size);
		}
		else
		{
			if (Size>DCStateDelta::Limit-Position) throw "State record exceeds small-buffer limit";
			const BYTE* P = (const BYTE*)Value;
			Data.insert(Data.end(), P, P+Size);
		}
		Position+=Size;
	}
};

void ReadBase(FILE* Base, const Record& R, std::vector<BYTE>& Data)
{
	if (R.Size>DCStateDelta::Limit) throw "Invalid small baseline size";
	Data.resize(R.Id ? R.Size : 0);
	if (!Data.empty())
	{
		if (fseek(Base, R.Offset, SEEK_SET)) throw "Cannot seek state baseline record";
		ZipIO Decoder(Base, true);
		Decoder.Bytes(&Data[0], Data.size());
		Decoder.Finish(false);
	}
}

void WriteBody(IO& Ar, FILE* Base, const Record& Original, Catalog& Symbols, const Record& R)
{
	BYTE Mode=0;
	if (R.Size<=DCStateDelta::Limit && Original.Size<=DCStateDelta::Limit)
	{
		std::vector<BYTE> Reference, Target, Code;
		ReadBase(Base, Original, Reference);
		MemoryIO Memory(Target, false);
		BodyArchive Body(Symbols, &Memory, R.Size);
		Symbols.Objects[R.Id].Object->Serialize(Body);
		if (Body.Position!=R.Size) throw "State body size changed";
		DCStateDelta::Encode(Reference, Target, Code);
		if (Code.size()<Target.size())
		{
			Mode=1; Ar.Bytes(&Mode, 1);
			unsigned Count=Code.size(); Ar.Number(Count);
			Ar.Bytes(&Code[0], Count);
		}
		else
		{
			Ar.Bytes(&Mode, 1);
			DeltaIO Delta(Ar, Base, Original);
			if (!Target.empty()) Delta.Bytes(&Target[0], Target.size());
		}
		return;
	}
	Ar.Bytes(&Mode, 1);
	DeltaIO Delta(Ar, Base, Original);
	BodyArchive Body(Symbols, &Delta, R.Size);
	Symbols.Objects[R.Id].Object->Serialize(Body);
	if (Body.Position!=R.Size) throw "State body size changed";
}

void ReadBody(IO& Ar, FILE* Base, const Record& Original, Catalog& Symbols, const Record& R, bool Apply)
{
	BYTE Mode;
	Ar.Bytes(&Mode, 1);
	if (Mode==1)
	{
		unsigned Count; Ar.Number(Count);
		if (Count>DCStateDelta::Limit*2 || R.Size>DCStateDelta::Limit) throw "Invalid state patch length";
		std::vector<BYTE> Reference, Code(Count), Target;
		ReadBase(Base, Original, Reference);
		if (Count) Ar.Bytes(&Code[0], Count);
		if (!DCStateDelta::Decode(Reference, Code, R.Size, Target)) throw "Invalid state patch commands";
		if (Apply)
		{
			MemoryIO Memory(Target, true);
			BodyArchive Body(Symbols, &Memory, R.Size);
			Symbols.Resolve(R.Id)->Serialize(Body);
			if (Body.Position!=R.Size) throw "State body extent mismatch";
		}
	}
	else if (!Mode)
	{
		DeltaIO Delta(Ar, Base, Original);
		if (Apply)
		{
			BodyArchive Body(Symbols, &Delta, R.Size);
			Symbols.Resolve(R.Id)->Serialize(Body);
			if (Body.Position!=R.Size) throw "State body extent mismatch";
		}
		else
		{
			BYTE Buffer[1024];
			for (unsigned Left=R.Size; Left;)
			{
				unsigned Count=Min(Left, (unsigned)sizeof(Buffer));
				Delta.Bytes(Buffer, Count); Left-=Count;
			}
		}
	}
	else throw "Unknown state record codec";
}

void ResetProperties(UObject* Obj)
{
	UClass* Class = Obj->GetClass();
	for (TFieldIterator<UProperty> It(Class); It; ++It)
	{
		if (It->PropertyFlags & (CPF_Transient|CPF_Intrinsic)) continue;
		unsigned Size = It->GetElementSize()*It->ArrayDim;
		if (It->Offset < 0 || (unsigned)It->Offset+Size > (unsigned)Class->Defaults.Num())
			throw "Invalid class defaults for world-state restore";
		BYTE* Dest = (BYTE*)Obj+It->Offset;
		BYTE* Src = &Class->Defaults(It->Offset);
		UBoolProperty* Bool = Cast<UBoolProperty>(*It);
		if (Bool)
		{
			for (INT i=0; i<It->ArrayDim; ++i)
				((DWORD*)Dest)[i] = (((DWORD*)Dest)[i]&~Bool->BitMask)|(((DWORD*)Src)[i]&Bool->BitMask);
		}
		else appMemcpy(Dest, Src, Size);
	}
}
}

UBOOL DCCookStateBaseline(ULevel* Level, const char* Path, char* Error)
{
	try
	{
		Catalog Symbols(Level);
		std::vector<Record> Records = Collect(Symbols);
		File Output(Path, "wb");
		RawIO Ar(Output.Handle, false);
		Header(Ar, "DCBASE04", Level);
		Symbols.Transfer(Ar);
		unsigned Count = Records.size();
		Ar.Number(Count);
		for (unsigned i=0; i<Count; ++i)
		{
			Record& R = Records[i];
			Ar.Number(R.Id); Ar.Number(R.Flags); Ar.Number(R.Size);
			long PackedPosition = ftell(Output.Handle);
			Ar.Number(R.Packed);
			ZipIO Packed(Output.Handle, false, MaxBody);
			BodyArchive Body(Symbols, &Packed, R.Size);
			Symbols.Objects[R.Id].Object->Serialize(Body);
			if (Body.Position != R.Size) throw "Baseline serialization changed between passes";
			Packed.Finish();
			long End = ftell(Output.Handle);
			R.Packed = Packed.Written;
			if (fseek(Output.Handle, PackedPosition, SEEK_SET)) throw "Cannot finalize baseline record";
			Ar.Number(R.Packed);
			if (fseek(Output.Handle, End, SEEK_SET)) throw "Cannot finalize baseline record";
		}
		long Bytes = ftell(Output.Handle);
		Output.Close();
		debugf("DCSTATE baseline map=%s objects=%u names=%u bytes=%ld", Level->GetParent()->GetName(),
			Count, (unsigned)Symbols.Names.size(), Bytes);
		return 1;
	}
	catch (const char* Reason) { appStrncpy(Error, Reason, 256); return 0; }
}

UBOOL DCCheckWorldState(const char* Map, const char* Baseline, const char* Path, char* Error)
{
	try
	{
		File Base(Baseline, "rb"), Input(Path, "rb");
		ZipIO Ar(Input.Handle, true);
		char Tag[8];
		Ar.Bytes(Tag, sizeof(Tag));
		if (appMemcmp(Tag, "DCSTATE4", 8)) throw "Unsupported canonical save version";
		std::string SavedMap;
		Ar.Text(SavedMap);
		unsigned CRC; Ar.Number(CRC);
		if (appStricmp(SavedMap.c_str(), Map) || CRC!=FileCRC(Base.Handle))
			throw "Save needs the exact cooked map baseline from its original build";
		return 1;
	}
	catch (const char* Reason) { appStrncpy(Error, Reason, 256); return 0; }
}

UBOOL DCWriteWorldState(ULevel* Level, const char* BaselinePath, const char* Path, char* Error)
{
	try
	{
		File Base(BaselinePath, "rb");
		unsigned CRC = FileCRC(Base.Handle);
		Catalog Symbols(Level);
		std::vector<Record> Baseline = ScanBaseline(Base.Handle, Symbols);
		unsigned BaseNames=Symbols.Names.size(), BaseObjects=Symbols.Objects.size();
		std::vector<Record> Records = Collect(Symbols);
		File Output(Path, "wb");
		ZipIO Ar(Output.Handle, false);
		Header(Ar, "DCSTATE4", Level);
		Ar.Number(CRC);
		Symbols.Transfer(Ar, BaseNames, BaseObjects);
		unsigned Count = Records.size();
		Ar.Number(Count);
		// All identities precede bodies, allowing spawned objects to be allocated first.
		for (unsigned i=0; i<Count; ++i)
		{
			Ar.Number(Records[i].Id); Ar.Number(Records[i].Flags); Ar.Number(Records[i].Size);
		}
		for (unsigned i=0; i<Count; ++i)
		{
			Record& R = Records[i];
			Record Empty = {};
			WriteBody(Ar, Base.Handle, R.Id < Baseline.size() ? Baseline[R.Id] : Empty, Symbols, R);
		}
		Ar.Finish();
		Output.Close();
		debugf("DCSTATE saved map=%s objects=%u packed=%u baseline=%08x", Level->GetParent()->GetName(),
			Count, Ar.Written, CRC);
		return 1;
	}
	catch (const char* Reason) { remove(Path); appStrncpy(Error, Reason, 256); return 0; }
}

UBOOL DCReadWorldState(ULevel* Level, const char* BaselinePath, const char* Path, char* Error)
{
	try
	{
		File Base(BaselinePath, "rb");
		unsigned BaselineCRC = FileCRC(Base.Handle);
		// First pass validates the complete compressed stream before modifying objects.
		for (unsigned Pass=0; Pass<2; ++Pass)
		{
			if (fseek(Base.Handle, 0, SEEK_SET)) throw "Cannot rewind state baseline";
			Catalog Symbols(Level);
			std::vector<Record> Baseline = ScanBaseline(Base.Handle, Symbols);
			unsigned BaseNames=Symbols.Names.size(), BaseObjects=Symbols.Objects.size();
			File Input(Path, "rb");
			ZipIO Ar(Input.Handle, true);
			Header(Ar, "DCSTATE4", Level);
			unsigned CRC, Count;
			Ar.Number(CRC);
			if (CRC != BaselineCRC) throw "World save belongs to a different cooked baseline";
			Symbols.Transfer(Ar, BaseNames, BaseObjects);
			Ar.Number(Count);
			if (!Count || Count > MaxObjects) throw "Invalid world-state record count";
			std::vector<Record> Records(Count);
			unsigned Total=0;
			for (unsigned i=0; i<Count; ++i)
			{
				Record& R = Records[i];
				Ar.Number(R.Id); Ar.Number(R.Flags); Ar.Number(R.Size);
				if (!R.Id || R.Id >= Symbols.Objects.size() || Symbols.Objects[R.Id].Needed ||
					R.Size > MaxBody || (R.Flags & ~RF_Load)) throw "Invalid world-state record";
				if (R.Size>MaxBody-Total) throw "World-state total size exceeds limit";
				Total+=R.Size;
				std::string Prefix=std::string(Level->GetParent()->GetName())+".";
				if (FoldCase(Symbols.Objects[R.Id].Path).compare(0, Prefix.size(), FoldCase(Prefix))!=0)
					throw "State record is outside the level package";
				Symbols.Objects[R.Id].Needed = true;
			}
			Entry Root;
			Root.Path = Level->GetPathName();
			Root.Class = Level->GetClass()->GetPathName();
			std::map<std::string, unsigned>::iterator RootId = Symbols.ObjectIds.find(Catalog::Key(Root));
			if (RootId == Symbols.ObjectIds.end() || !Symbols.Objects[RootId->second].Needed)
				throw "World state has no level record";
			if (Pass)
			{
				// Allocate the complete graph before deserializing references.
				for (unsigned i=0; i<Count; ++i) Symbols.Resolve(Records[i].Id);
			}
			for (unsigned i=0; i<Count; ++i)
			{
				Record& R = Records[i];
				if (Pass)
				{
					UObject* Obj = Symbols.Resolve(R.Id);
					Obj->ClearFlags(RF_Load);
					Obj->SetFlags(R.Flags);
					ResetProperties(Obj);
				}
				Record Empty = {};
				ReadBody(Ar, Base.Handle, R.Id < Baseline.size() ? Baseline[R.Id] : Empty, Symbols, R, Pass!=0);
				appDCLoadingProgress(0.1f + 0.45f * (Pass + (FLOAT)(i+1)/Count));
			}
			Ar.Finish();
		}
		debugf("DCSTATE restored map=%s baseline=%08x", Level->GetParent()->GetName(), BaselineCRC);
		return 1;
	}
	catch (const char* Reason) { appStrncpy(Error, Reason, 256); return 0; }
}

UBOOL DCTestWorldState(ULevel* Level, const char* Baseline, char* Error)
{
	std::string First = std::string(Baseline)+".test-a";
	std::string Second = std::string(Baseline)+".test-b";
	if (Level->BrushTracker)
	{
		Level->BrushTracker->Exit();
		delete Level->BrushTracker;
		Level->BrushTracker = NULL;
	}
#if defined(DC_RESOURCE_COOKER)
	if (ParseParam(appCmdLine(), "DCSTATEEXERCISE"))
	{
		if (Level->Model->Surfs->Num()) Level->Model->Surfs->Element(0).PanU += 17;
		UBOOL ChangedMover=0, RemovedPickup=0;
		for (INT i=0; i<Level->Num(); ++i)
		{
			if (!ChangedMover)
				if (AMover* Mover=Cast<AMover>(Level->Actors(i)))
				{
					Mover->PhysAlpha=0.375f;
					Mover->PhysRate=0.25f;
					// A stationary saved mover must still enter a fresh brush tracker.
					if( Level->Hash && Mover->bCollideActors ) Level->Hash->RemoveActor(Mover);
					Mover->Location.Z += 64.f;
					if( Level->Hash && Mover->bCollideActors ) Level->Hash->AddActor(Mover);
					Mover->SavedPos = Mover->Location;
					Mover->SavedRot = Mover->Rotation;
					ChangedMover=1;
				}
			if (!RemovedPickup)
				if (AInventory* Item=Cast<AInventory>(Level->Actors(i)))
					if (!Item->Owner) RemovedPickup=Level->DestroyActor(Item);
		}
		UClass* ProbeClass=GObj.LoadClass(AActor::StaticClass, NULL, "Engine.Light", NULL, LOAD_NoFail, NULL);
		AActor* Probe=(AActor*)GObj.ConstructObject(ProbeClass, Level->GetParent(), FName("DCStateProbe"));
		Probe->XLevel=Level;
		Probe->Level=Level->GetLevelInfo();
		Probe->Region.Zone=Level->GetLevelInfo();
		Probe->Owner=Level->GetLevelInfo();
		Probe->bStatic=0;
		Probe->bNoDelete=0;
		Probe->bHidden=1;
		Probe->InitExecution();
		Level->Actors(Level->Add(1))=Probe;
		debugf("DCSTATE exercise surface=1 mover=%d deleted_pickup=%d spawned=1", ChangedMover, RemovedPickup);
	}
#endif
	Level->CleanupDestroyed(1);
	if (!DCWriteWorldState(Level, Baseline, First.c_str(), Error)) return 0;
	// Ensure this is a restore test, not just serialization of unchanged objects.
	for (INT i=0; i<Level->Num(); ++i)
		if (APawn* Pawn = Cast<APawn>(Level->Actors(i))) Pawn->Health = 1;
	if (!DCReadWorldState(Level, Baseline, First.c_str(), Error) ||
		!DCWriteWorldState(Level, Baseline, Second.c_str(), Error)) return 0;
	try
	{
		File A(First.c_str(), "rb"), B(Second.c_str(), "rb");
		for (;;)
		{
			BYTE Left[1024], Right[1024];
			unsigned L = fread(Left, 1, sizeof(Left), A.Handle);
			unsigned R = fread(Right, 1, sizeof(Right), B.Handle);
			if (L != R || appMemcmp(Left, Right, L)) throw "Native world-state round trip differs";
			if (!L) break;
		}
		debugf("DCSTATE native_round_trip=passed map=%s", Level->GetParent()->GetName());
		return 1;
	}
	catch (const char* Reason) { appStrncpy(Error, Reason, 256); return 0; }
}
#endif

#include "CookTools.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <zlib.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

extern bool DCCookStreamCommand(int Argc, const char** Argv, int& Result);

namespace DCCook
{
namespace
{
struct SplitRecipe
{
	const char* Original;
	const char* Name;
	const char* Zones;
};

const SplitRecipe Splits[] = {
    {"Dig", "Dig1", "18+19+22+23+24+26+28+32+37+40+41"},
    {"Dig", "Dig2",
	 "1+2+3+4+5+6+7+8+9+10+11+12+13+14+15+16+17+20+21+25+27+29+30+31+33+34+35+36+38+39+42+43"},
    {"DasaCellars", "DasaCellars1",
	 "1+2+3+4+5+11+12+13+14+15+16+17+18+19+20+21+22+23+24+27+28+31+32+33+34+35+36+37"},
    {"DasaCellars", "DasaCellars2", "6+7+8+9+10+13+16+23+25+26+27+28+29+30+31+32+33+38+39"},
    {"Ruins", "Ruins1", "1+2+3+4+5+6+11+12+13+14+15+16+17+18+20+21+22+23+28+31"},
    {"Ruins", "Ruins2", "4+5+6+7+8+9+10+11+13+18+19+21+23+24+25+26+27+28+29+30+31"},
    {"Chizra", "Chizra1", "1+2+3+4+5+7+8+9+10+11+12+13+14+17+18+20+21"},
    {"Chizra", "Chizra2", "19+22+23+24+26+27+28+29+30+31+32+33+34+35+37+38+39+40+41+42+43"},
    {"Terraniux", "Terraniux1", "1+3+4+5+6+8+20+24+26+28+29"},
    {"Terraniux", "Terraniux2",
	 "2+7+9+10+11+12+13+14+15+16+17+18+19+21+22+23+25+27+30+31+32+33+34+35+36+37+38+39+40"},
    {"IsvKran32", "IsvKran32A", "1+2+3+4+7+8+9+10+11+15+16+17+19+24+26+31"},
    {"IsvKran32", "IsvKran32B", "5+6+21+22+32"},
    {"SkyTown", "SkyTown1",
	 "1+2+3+4+5+8+9+10+11+12+13+14+15+16+17+18+19+20+21+22+23+27+29+30+31+32+33+34+35+36+37+38+39+"
	 "40+41+42+43+44+45+46+47+48+49+50+51"},
    {"SkyTown", "SkyTown2",
	 "4+5+6+7+11+12+16+17+18+19+21+22+23+24+25+26+27+28+29+31+36+37+39+40+41+42+43+44+45+46+47+48+"
	 "49+50+51"}};

const char* Campaign[] = {
    "Entry",       "Unreal",      "Vortex2",      "NyLeve",       "Dig1",        "Dig2",
    "Dug",         "Chizra1",     "Chizra2",      "Ceremony",     "Dark",        "Harobed",
    "TerraLift",   "Terraniux1",  "Terraniux2",   "Noork",        "Ruins1",      "Ruins2",
    "Trench",      "IsvKran4",    "IsvKran32A",   "IsvKran32B",   "IsvDeck1",    "SpireVillage",
    "TheSunspire", "SkyCaves",    "SkyTown1",     "SkyTown2",     "SkyBase",     "VeloraEnd",
    "Bluff",       "DasaPass",    "DasaCellars1", "DasaCellars2", "NaliBoat",    "NaliC",
    "NaliLord",    "DCrater",     "ExtremeBeg",   "ExtremeLab",   "ExtremeCore", "ExtremeGen",
    "ExtremeDGen", "ExtremeDark", "ExtremeEnd",   "QueenEnd",     "endgame",     "Gateway",
    "Passage",     "DKNightOp"};

std::vector<fs::path> Files(const fs::path& Directory, const std::set<std::string>& Extensions)
{
	std::vector<fs::path> Result;
	for (const auto& Item : fs::directory_iterator(Directory))
		if (Item.is_regular_file() && Extensions.count(Lower(Item.path().extension().string())))
			Result.push_back(Item.path());
	std::sort(Result.begin(), Result.end(), [](const fs::path& A, const fs::path& B)
	          { return Lower(A.filename().string()) < Lower(B.filename().string()); });
	for (size_t i = 1; i < Result.size(); ++i)
		if (Lower(Result[i].filename().string()) == Lower(Result[i - 1].filename().string()))
			throw std::runtime_error("Case-ambiguous inputs: " + Result[i].string());
	return Result;
}

fs::path Find(const fs::path& Directory, const std::string& Name)
{
	fs::path Result;
	for (const auto& Item : fs::directory_iterator(Directory))
		if (Lower(Item.path().filename().string()) == Lower(Name))
		{
			if (!Result.empty())
				throw std::runtime_error("Ambiguous input: " + Name);
			Result = Item.path();
		}
	if (Result.empty())
		throw std::runtime_error("Missing input: " + (Directory / Name).string());
	return Result;
}

bool Inside(const fs::path& Path, const fs::path& Root)
{
	const auto Relative = Path.lexically_relative(Root);
	return !Relative.empty() && *Relative.begin() != "..";
}

std::string ExecutablePath(const std::string& Name)
{
	if (Name.empty())
		throw std::runtime_error("Empty encoder path");
	if (fs::path(Name).has_parent_path())
		return fs::canonical(Name).string();
	const char* Search = std::getenv("PATH");
	std::istringstream Paths(Search ? Search : "");
	std::string Directory;
	while (std::getline(Paths, Directory, ':'))
	{
		const auto Candidate = fs::path(Directory.empty() ? "." : Directory) / Name;
		if (fs::is_regular_file(Candidate))
			return fs::canonical(Candidate).string();
	}
	throw std::runtime_error("Missing encoder: " + Name);
}

std::string Option(const std::string& Key, const fs::path& Path)
{
	const auto Text = Path.string();
	if (Text.find_first_of("\"\r\n") != std::string::npos)
		throw std::runtime_error("Path cannot be represented in the engine command line");
	return Key + "=\"" + Text + "\"";
}

std::string Checksum(const fs::path& Path)
{
	std::ifstream File(Path, std::ios::binary);
	if (!File)
		throw std::runtime_error("Cannot fingerprint " + Path.string());
	char Buffer[65536];
	uLong Crc = crc32(0, Z_NULL, 0);
	uint64_t Size = 0;
	while (File.read(Buffer, sizeof(Buffer)) || File.gcount())
	{
		Crc = crc32(Crc, reinterpret_cast<const Bytef*>(Buffer), File.gcount());
		Size += File.gcount();
	}
	if (!File.eof())
		throw std::runtime_error("Cannot fingerprint " + Path.string());
	return std::to_string(Size) + ":" + std::to_string(Crc);
}

struct Pipeline
{
	struct WorkLock
	{
#if !defined(_WIN32)
		int Fd = -1;
		~WorkLock()
		{
			if (Fd >= 0)
				close(Fd);
		}
		void Acquire(const fs::path& Work)
		{
			Fd = open((Work / ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
			if (Fd < 0 || flock(Fd, LOCK_EX | LOCK_NB) != 0)
				throw std::runtime_error("Cook work tree is locked by another process");
		}
#else
		void Acquire(const fs::path&)
		{
			throw std::runtime_error("Run the cooker under Linux/WSL");
		}
#endif
	} Lock;
	fs::path Source, Output, Work, Build, Profile, Boot;
	fs::path Stage, Runner, Audit;
	Encoders Tools;
	std::string OnlyMap;
	std::string Until;
	bool Plan = false;
	bool Resume = false;
	bool Reimport = false;

	void Localization()
	{
		for (const auto& File : Files(Source / "System", {".int"}))
			fs::copy_file(File, Runner / File.filename(), fs::copy_options::overwrite_existing);
	}

	std::string Outputs(const std::vector<fs::path>& Paths)
	{
		std::string Result;
		for (const auto& Path : Paths)
			Result += Path.lexically_relative(Work).generic_string() + "\t" + Checksum(Path) + "\n";
		return Result;
	}

	bool Done(const std::string& Task, const std::vector<fs::path>& Paths)
	{
		const auto Marker = Work / "done" / Task;
		if (!fs::is_regular_file(Marker))
			return false;
		for (const auto& Path : Paths)
			if (!fs::is_regular_file(Path))
				return false;
		const auto Saved = Read(Marker);
		return std::string(Saved.begin(), Saved.end()) == Outputs(Paths);
	}

	void Complete(const std::string& Task, const std::vector<fs::path>& Paths)
	{
		const auto Text = Outputs(Paths);
		Write(Work / "done" / Task, Bytes(Text.begin(), Text.end()));
	}

	std::string Fingerprint()
	{
		std::string Text = "DCUtil cook v1\n" + Source.string() + "\nmap=" + Lower(OnlyMap) + "\n";
		auto Add = [&](const fs::path& Path)
		{ Text += Path.generic_string() + "\t" + Checksum(Path) + "\n"; };
		for (const char* Dir : {"System", "Maps", "Textures", "Sounds", "Music"})
			for (const auto& File :
			     Files(Source / Dir, {".u", ".unr", ".utx", ".uax", ".umx", ".ini", ".int"}))
				Add(File);
		for (const auto& File : Files(Profile, {".ini", ".dcb"}))
			Add(File);
		const auto Worker = Build / "DCUtil/DCUtil.bin";
		// Resume uses the preserved worker, not a newly linked executable.
		Text += Worker.generic_string() + "\t" + Checksum(Resume ? Runner / "DCUtil.bin" : Worker) +
		        "\n";
		std::vector<fs::path> Libraries;
		for (const auto& Directory : fs::directory_iterator(Build))
			if (Directory.is_directory())
			{
				const auto Lib = Directory.path() / (Directory.path().filename().string() + ".so");
				if (fs::is_regular_file(Lib))
					Libraries.push_back(Lib);
			}
		std::sort(Libraries.begin(), Libraries.end());
		for (const auto& Lib : Libraries)
			Add(Lib);
		for (const auto& Tool : {Tools.Pvrtex, Tools.Adpcm, Tools.Ffmpeg, Tools.Convert})
		{
			Text += "tool=" + Tool + "\n";
			if (fs::is_regular_file(Tool))
				Add(Tool);
		}
		if (!Boot.empty())
			Add(Boot);
		return Text;
	}

	void Engine(const std::string& Label, std::vector<std::string> Args,
	            const std::string& Marker, const std::string& Action)
	{
		std::printf("Cooker: %s\n", Action.c_str());
		std::fflush(stdout);
		Args.insert(Args.begin(), (Runner / "DCUtil.bin").string());
		Args.push_back("LOG=worker.log");
		if (fs::exists(Runner / "worker.log"))
			fs::remove(Runner / "worker.log");
		Run(Args, Runner, Audit / (Label + ".log"), Marker,
		    {{"LD_LIBRARY_PATH", Runner.string()},
		     {"SDL_VIDEODRIVER", "dummy"},
		     {"SDL_AUDIODRIVER", "dummy"}},
		    Runner / "worker.log", Action);
	}

	void ExportIni()
	{
		std::ifstream In(Profile / "Default.ini");
		std::ofstream Out(Runner / "DCExport.ini");
		const char* Paths[] = {"System/*.u", "Maps/*.unr", "Textures/*.utx", "Sounds/*.uax",
		                       "Music/*.umx"};
		std::string Line;
		unsigned Replaced = 0;
		while (std::getline(In, Line))
		{
			if (!Line.empty() && Line.back() == '\r')
				Line.pop_back();
			bool Found = false;
			for (unsigned i = 0; i < 5; ++i)
			{
				const auto Key = "Paths[" + std::to_string(i) + "]=";
				if (Lower(Line).find(Lower(Key)) == 0)
				{
					Out << Key << (Source / Paths[i]).string() << '\n';
					Found = true;
					++Replaced;
					break;
				}
			}
			if (!Found)
				Out << Line << '\n';
		}
		Out.close();
		if (!Out || Replaced != 5)
			throw std::runtime_error("Invalid cooker configuration search paths");
	}

	void Prepare()
	{
		if (!fs::is_directory(Source / "System"))
			throw std::runtime_error("Source is not an Unreal installation");
		Source = fs::canonical(Source);
		Output = fs::weakly_canonical(fs::absolute(Output));
		Work = fs::weakly_canonical(fs::absolute(Work));
		Build = fs::canonical(Build);
		Profile = fs::canonical(Profile);
		Tools.Pvrtex = ExecutablePath(Tools.Pvrtex);
		Tools.Adpcm = ExecutablePath(Tools.Adpcm);
		Tools.Ffmpeg = ExecutablePath(Tools.Ffmpeg);
		Tools.Convert = ExecutablePath(Tools.Convert);
		if (Inside(Output, Source) || Inside(Source, Output) || Inside(Work, Source) ||
		    Inside(Source, Work) || Inside(Output, Work) || Inside(Work, Output) ||
		    Inside(Build, Work) || Inside(Profile, Work) || Inside(Build, Output) ||
		    Inside(Profile, Output))
			throw std::runtime_error("Source, work and output trees must be separate");
		if (fs::exists(Output) || (fs::exists(Work) && !Resume))
			throw std::runtime_error(
			    "Output exists or work requires --resume; choose a fresh output");
		for (const char* Dir : {"System", "Maps", "Textures", "Sounds", "Music"})
			if (!fs::is_directory(Source / Dir))
				throw std::runtime_error(std::string("Missing source folder: ") + Dir);
		if (!fs::is_regular_file(Build / "DCUtil/DCUtil.bin"))
			throw std::runtime_error("Missing host DCUtil build");
		for (const char* Name :
		     {"Default.ini", "Unreal.ini", "DCMover.ini", "loadbg.dcb", "loadbar.dcb"})
			if (!fs::is_regular_file(Profile / Name))
				throw std::runtime_error(std::string("Missing cook profile: ") + Name);
		for (const auto& Recipe : Splits)
			Find(Source / "Maps", std::string(Recipe.Original) + ".unr");
		for (const char* Name : Campaign)
		{
			bool IsSplit = false;
			for (const auto& Recipe : Splits)
				IsSplit |= Lower(Name) == Lower(Recipe.Name);
			if (!IsSplit)
				Find(Source / "Maps", std::string(Name) + ".unr");
		}
		if (!OnlyMap.empty())
		{
			bool Found = false;
			for (const char* Name : Campaign)
				Found |= Lower(Name) == Lower(OnlyMap);
			if (!Found)
				throw std::runtime_error("Map is not in the single-player campaign");
		}
		if (Plan)
		{
			std::printf("Source: %s\nWork: %s\nOutput: %s\n", Source.string().c_str(),
			            Work.string().c_str(), Output.string().c_str());
			std::puts("1. Copy original packages into private work tree\n2. Split seven campaign "
			          "maps into fourteen halves\n"
			          "3. Export/encode/import resources, cook meshes, compact map data\n4. Bake "
			          "static and dynamic VQ lightmaps\n"
			          "5. Record DCS streams and native save baselines\n6. Verify package-free "
			          "replay, mover poses and cold saves\n"
			          "7. Render music to ADPCM\n8. Publish single-player disc resources");
			return;
		}
		Stage = Work / "packages";
		Runner = Stage / "Runner";
		Audit = Work / "logs";
		if (!Resume)
			fs::create_directories(Work);
		Lock.Acquire(Work);
		const auto Identity = Fingerprint();
		if (Resume)
		{
			const auto Saved = Read(Work / "inputs.txt");
			if (std::string(Saved.begin(), Saved.end()) != Identity)
				throw std::runtime_error("Cook inputs/tools changed; use a fresh work directory");
			if (!fs::is_regular_file(Work / "prepared"))
				throw std::runtime_error("Work preparation was incomplete; use a fresh directory");
			Localization();
			return;
		}
		fs::create_directories(Work);
		Write(Work / "inputs.txt", Bytes(Identity.begin(), Identity.end()));
		fs::create_directories(Work / "done");
		fs::create_directories(Runner);
		fs::create_directories(Audit);
		for (const char* Dir : {"System", "Maps", "Textures", "Sounds", "Music"})
		{
			fs::create_directories(Stage / Dir);
			for (const auto& Path :
			     Files(Source / Dir, {".u", ".unr", ".utx", ".uax", ".umx", ".ini", ".int"}))
				fs::copy_file(Path, Stage / Dir / Path.filename());
		}
		for (const auto& Directory : fs::directory_iterator(Build))
			if (Directory.is_directory() && Directory.path().filename() != "RelWithDebInfo")
				for (const auto& Lib : Files(Directory.path(), {".so"}))
					if (Lib.stem() == Directory.path().filename())
						fs::copy_file(Lib, Runner / Lib.filename());
		fs::copy_file(Build / "DCUtil/DCUtil.bin", Runner / "DCUtil.bin");
		for (const auto& Path : Files(Profile, {".ini"}))
		{
			fs::copy_file(Path, Runner / Path.filename());
			fs::copy_file(Path, Stage / "System" / Path.filename(),
			              fs::copy_options::overwrite_existing);
		}
		ExportIni();
		Localization();
		Write(Work / "prepared", {});
	}

	void SplitMaps()
	{
		fs::create_directories(Work / "split");
		for (const auto& Recipe : Splits)
		{
			if (!OnlyMap.empty() && Lower(OnlyMap) != Lower(Recipe.Name))
				continue;
			const auto Output = Work / "split" / (std::string(Recipe.Name) + ".unr");
			const auto Task = std::string(Recipe.Name) + "-split";
			if (!Reimport && Done(Task, {Output}))
				continue;
			Engine(
			    std::string(Recipe.Name) + "-split",
			    {Option("TESTSPLIT", Find(Source / "Maps", std::string(Recipe.Original) + ".unr")),
				 Option("OUT", Work / "split" / (std::string(Recipe.Name) + ".unr")),
				 std::string("KEEPZONES=") + Recipe.Zones, std::string("ROLE=") + Recipe.Name,
				 "INI=DCExport.ini"},
			    "TESTSPLIT OK", std::string("splitting ") + Recipe.Original + ".unr -> " +
		                        Recipe.Name + ".unr");
			Complete(Task, {Output});
		}
	}

	void Package(const fs::path& Input, const fs::path& Destination)
	{
		const auto Name = Input.filename().string();
		const bool Cached = Done(Name + "-resources", {Destination});
		if (Cached && !Reimport)
			return;
		const auto Resources = Work / "resources" / Name;
		fs::create_directories(Resources);
		if (!Cached)
			Engine(Name + "-export",
			       {Option("EXPORTDC", Input), Option("RES", Resources), "INI=DCExport.ini"},
			       "RESOURCES OK", "exporting sounds and textures from " + Name);
		const auto Images = Files(Resources, {".png"});
		const auto Sounds = Files(Resources, {".wav"});
		size_t Converted = 0;
		size_t TextureCount = 0;
		for (const auto& Path : Images)
			if (Path.stem().extension() != ".scaled")
				++TextureCount;
		if (TextureCount)
		{
			std::printf("Cooker: encoding %s textures (0/%zu)\n", Name.c_str(), TextureCount);
			std::fflush(stdout);
		}
		for (const auto& Path : Images)
			if (Path.stem().extension() != ".scaled")
			{
				auto Encoded = Path;
				Encoded.replace_extension(".dt");
				if (!Cached || !fs::is_regular_file(Encoded))
					Texture(Path, Tools);
				if (++Converted % 8 == 0 || Converted == TextureCount)
				{
					std::printf("Cooker: encoded %s textures (%zu/%zu)\n", Name.c_str(),
					            Converted, TextureCount);
					std::fflush(stdout);
				}
			}
		Converted = 0;
		size_t SoundCount = 0;
		for (const auto& Path : Sounds)
			if (Path.stem().extension() != ".pcm" && Path.stem().extension() != ".dca")
				++SoundCount;
		if (SoundCount)
		{
			std::printf("Cooker: encoding %s sounds (0/%zu)\n", Name.c_str(), SoundCount);
			std::fflush(stdout);
		}
		for (const auto& Path : Sounds)
			if (Path.stem().extension() != ".pcm" && Path.stem().extension() != ".dca")
			{
				auto Encoded = Path;
				Encoded.replace_extension(".dca.wav");
				if (!Cached || !fs::is_regular_file(Encoded))
					Sound(Path, Tools);
				if (++Converted % 8 == 0 || Converted == SoundCount)
				{
					std::printf("Cooker: encoded %s sounds (%zu/%zu)\n", Name.c_str(),
					            Converted, SoundCount);
					std::fflush(stdout);
				}
			}
		const auto Import = Work / "import" / Name;
		fs::create_directories(Import);
		Engine(Name + "-import",
		       {Option("IMPORTDC", Input), Option("RES", Resources), Option("OUT", Import / Name),
		        "INI=DCExport.ini"},
		       "RESOURCES OK", "importing converted resources and cooking meshes in " + Name);
		fs::copy_file(Import / Name, Destination, fs::copy_options::overwrite_existing);
		Complete(Name + "-resources", {Destination});
	}

	void Resources()
	{
		for (const char* Dir : {"System", "Textures", "Sounds", "Music"})
			for (const auto& Path : Files(Source / Dir, {".u", ".utx", ".uax", ".umx"}))
				Package(Path, Stage / Dir / Path.filename());
		for (const char* Name : Campaign)
		{
			if (!OnlyMap.empty() && Lower(Name) != Lower(OnlyMap) && Lower(Name) != "entry")
				continue;
			fs::path Input;
			for (const auto& Recipe : Splits)
				if (Lower(Name) == Lower(Recipe.Name))
					Input = Work / "split" / (std::string(Name) + ".unr");
			const bool IsSplit = !Input.empty();
			if (!IsSplit)
				Input = Find(Source / "Maps", std::string(Name) + ".unr");
			Package(Input, Stage / "Maps" /
			                   (IsSplit ? std::string(Name) + ".unr" : Input.filename().string()));
		}
	}

	void PackStream(const fs::path& Raw, const fs::path& Output)
	{
		const fs::path Partial = Output.string() + ".packing";
		if (fs::exists(Partial))
		{
			fs::remove(Partial / "stream.dcs");
			if (!fs::remove(Partial))
				throw std::runtime_error("Cannot clear interrupted stream packing");
		}
		const std::vector<std::string> Strings{"DCUtil", "--pack-stream", Raw.string(),
		                                       Output.string(), (Stage / "System").string()};
		std::vector<const char*> Args;
		for (const auto& S : Strings)
			Args.push_back(S.c_str());
		int Result = 1;
		if (!DCCookStreamCommand(Args.size(), Args.data(), Result) || Result)
			throw std::runtime_error("Native stream packing failed");
	}

	void Map(const std::string& Name)
	{
		std::vector<fs::path> Outputs;
		for (const char* Ext : {".dlm", ".ddm", ".dcs", ".dsb"})
			if (Lower(Name) != "entry" || std::string(Ext) != ".dsb")
				Outputs.push_back(Stage / "Maps" / (Name + Ext));
		if (!Reimport && Done(Name + "-map", Outputs))
			return;
		const auto Scratch = Work / "maps" / Name;
		fs::create_directories(Scratch);
		const auto RawLight = Scratch / (Name + ".v3");
		Engine(Name + "-lightmaps", {Name, Option("-BAKEDCLIGHTMAPS", RawLight), "INI=Default.ini"},
		       "DCLIGHTMAP cooked", "baking lightmaps for " + Name + ".unr");
		std::printf("Cooker: packing VQ lightmaps for %s.unr\n", Name.c_str());
		std::fflush(stdout);
		Lightmaps(RawLight, RawLight.string() + ".dyn", Stage / "Maps" / (Name + ".dlm"), Scratch,
		          Tools);
		const auto Raw = Scratch / "session.raw";
		const auto Stream = Stage / "Maps" / (Name + ".dcs");
		const auto Baseline = Stage / "Maps" / (Name + ".dsb");
		std::vector<std::string> Record{Name, "-COOKSESSION", "-DEFERMIPS", Option("OUT", Raw),
		                                "INI=Default.ini"};
		if (Lower(Name) != "unreal")
			Record.push_back("-DCDIRECTSESSION");
		if (Lower(Name) != "entry" && Lower(Name) != "unreal")
		{
			Record.push_back(Option("DCCOOKSTATE", Baseline));
			Record.push_back(Option("DCTESTSTATE", Baseline));
		}
		Engine(Name + "-record", Record, "DCSESSION recipe_teardown_verified",
		       "recording stream for " + Name + ".dcs");
		if (Lower(Name) == "unreal")
		{
			// The attract-map stream includes Entry; a restored session does not.
			Engine(Name + "-baseline",
			       {Name, "-COOKSESSION", "-DCDIRECTSESSION", "-DEFERMIPS",
			        Option("OUT", Scratch / "state.raw"), Option("DCCOOKSTATE", Baseline),
			        Option("DCTESTSTATE", Baseline), "INI=Default.ini"},
			       "DCSTATE native_round_trip=passed", "recording save baseline for Unreal.dsb");
		}
		// The input manifest was verified before resuming this private work tree.
		if (fs::exists(Stream))
			fs::remove(Stream);
		std::printf("Cooker: packing %s.dcs\n", Name.c_str());
		std::fflush(stdout);
		PackStream(Raw, Stream);
		std::vector<std::string> Verify{Name,
		                                "-VERIFYSESSION",
		                                "-DCTESTOBJECTORDER",
		                                "-DCTESTMOVERS",
		                                Option("STREAM", Stream),
		                                "INI=Default.ini"};
		if (Lower(Name) != "unreal")
			Verify.push_back("-DCDIRECTSESSION");
		Engine(Name + "-replay", Verify, "DCSESSION recipe_teardown_verified",
		       "verifying stream " + Name + ".dcs");
		const auto LogData = Read(Audit / (Name + "-replay.log"));
		const std::string Log(LogData.begin(), LogData.end());
		for (const char* Marker :
		     {"DCRESOURCE VERIFIED", "loose_texture_opens=0", "DCSESSION recipe_verified",
		      "DCSTREAM session_released stores=0", "DCQUEUE object_order_perturbed swaps=",
		      "DCRCACHE verified points=", "DCMOVERPEAK map="})
			if (Log.find(Marker) == std::string::npos)
				throw std::runtime_error(Name + ": missing replay check " + Marker);
		if (Lower(Name) != "entry")
		{
			const auto Cold = Scratch / "cold-input";
			fs::copy_file(Baseline.string() + ".test-a", Cold,
			              fs::copy_options::overwrite_existing);
			// Restore targets the first loaded level, so bypass the boot Entry map.
			std::vector<std::string> ColdArgs{Name,
			                                  "-VERIFYSESSION",
			                                  "-DCDIRECTSESSION",
			                                  "-DCSTATEINDEXED",
			                                  Option("STREAM", Stream),
			                                  Option("DCRESTORESTATE", Cold),
			                                  Option("DCRESTOREBASE", Baseline),
			                                  Option("DCTESTSTATE", Baseline),
			                                  "INI=Default.ini"};
			Engine(Name + "-cold-save", ColdArgs, "DCSTATE native_round_trip=passed",
			       "verifying save restore for " + Name + ".dsb");
			if (Read(Cold) != Read(Baseline.string() + ".test-a"))
				throw std::runtime_error(Name + ": cold restore changed serialized state");
		}
		Complete(Name + "-map", Outputs);
	}

	void Publish()
	{
		const auto Prepared = Work / "gamedata";
		for (const char* Dir : {"System", "Maps", "Music"})
			fs::create_directories(Prepared / Dir);
		for (const auto& File : Files(Source / "System", {".int"}))
			fs::copy_file(File, Prepared / "System" / File.filename());
		for (const auto& File : Files(Profile, {".ini", ".dcb"}))
			fs::copy_file(File, Prepared / "System" / File.filename());
		for (const char* Name : Campaign)
		{
			if (!OnlyMap.empty() && Lower(Name) != Lower(OnlyMap) && Lower(Name) != "entry")
				continue;
			for (const char* Ext : {".dcs", ".dlm", ".ddm", ".dsb"})
			{
				if (Lower(Name) == "entry" && std::string(Ext) == ".dsb")
					continue;
				fs::copy_file(Stage / "Maps" / (std::string(Name) + Ext),
				              Prepared / "Maps" / (std::string(Name) + Ext));
			}
		}
		for (const auto& File : Files(Stage / "Music", {".dcw", ".dcm"}))
			fs::copy_file(File, Prepared / "Music" / File.filename());
		if (!Boot.empty())
			fs::copy_file(Boot, Prepared / "1ST_READ.BIN");
		if (fs::exists(Output))
			throw std::runtime_error("Output appeared during cook; refusing to replace it");
		fs::create_directories(Output.parent_path());
		fs::rename(Prepared, Output);
		std::printf("DC cook complete: %s\n", Output.string().c_str());
	}

	void Cook()
	{
		Prepare();
		if (Plan)
			return;
		if (Reimport)
			for (const char* Name : Campaign)
				fs::remove(Work / "done" / (std::string(Name) + "-map"));
		SplitMaps();
		if (Until == "splits")
			return;
		Resources();
		if (Until == "resources")
			return;
		for (const char* Name : Campaign)
		{
			if (!OnlyMap.empty() && Lower(Name) != Lower(OnlyMap) && Lower(Name) != "entry")
				continue;
			Map(Name);
		}
		if (Until == "maps")
			return;
		for (const auto& Package : Files(Source / "Music", {".umx"}))
		{
			std::printf("Cooker: rendering %s -> Dreamcast ADPCM\n",
			            Package.filename().string().c_str());
			std::fflush(stdout);
			const auto Scratch = Work / "music" / Package.stem();
			Music(Package, Stage / "Music", Scratch, Tools);
		}
		if (Until == "music")
			return;
		Publish();
	}
};
} // namespace
} // namespace DCCook

bool DCCookPipelineCommand(int Argc, const char** Argv, int& Result)
{
	using namespace DCCook;
	if (Argc < 2 || std::string(Argv[1]).find("--cook=") != 0)
		return false;
	Result = 1;
	try
	{
		if (std::string(Argv[1]) != "--cook=everything")
			throw std::runtime_error("Expected --cook=everything");
		Pipeline P;
		for (int i = 2; i < Argc; ++i)
		{
			const std::string Arg = Argv[i];
			const auto Equal = Arg.find('=');
			const auto Key = Arg.substr(0, Equal);
			const auto Value = Equal == std::string::npos ? "" : Arg.substr(Equal + 1);
			if (Key == "--source")
				P.Source = Value;
			else if (Key == "--output")
				P.Output = Value;
			else if (Key == "--work")
				P.Work = Value;
			else if (Key == "--build")
				P.Build = Value;
			else if (Key == "--profile")
				P.Profile = Value;
			else if (Key == "--boot")
				P.Boot = fs::absolute(Value);
			else if (Key == "--map")
				P.OnlyMap = Value;
			else if (Key == "--until")
				P.Until = Value;
			else if (Key == "--plan" && Equal == std::string::npos)
				P.Plan = true;
			else if (Key == "--resume" && Equal == std::string::npos)
				P.Resume = true;
			else if (Key == "--reimport" && Equal == std::string::npos)
				P.Reimport = true;
			else if (Key == "--pvrtex")
				P.Tools.Pvrtex = Value;
			else if (Key == "--wav2adpcm")
				P.Tools.Adpcm = Value;
			else if (Key == "--ffmpeg")
				P.Tools.Ffmpeg = Value;
			else if (Key == "--convert")
				P.Tools.Convert = Value;
			else
				throw std::runtime_error("Unknown cook option: " + Arg);
		}
		const auto Executable = fs::canonical(Argv[0]);
		if (P.Build.empty())
			P.Build = Executable.parent_path().parent_path();
		if (P.Profile.empty())
			P.Profile = Executable.parent_path() / "Profile";
		if (P.Output.empty())
			P.Output = "gamedata";
		if (P.Work.empty())
			P.Work = P.Output.string() + ".cook";
		if (P.Source.empty())
			throw std::runtime_error(
			    "Usage: DCUtil --cook=everything --source=ORIGINAL --output=GAMEDATA [--plan]");
		if (!P.Until.empty() && P.Until != "splits" && P.Until != "resources" &&
		    P.Until != "maps" && P.Until != "music")
			throw std::runtime_error("--until must be splits, resources, maps or music");
		P.Cook();
		Result = 0;
	}
	catch (const std::exception& Error)
	{
		std::fprintf(stderr, "DC cook failed: %s\n", Error.what());
	}
	return true;
}

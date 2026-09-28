#include "Src/CookPipeline.cpp"
#include <cassert>
#include <chrono>

int main(int Argc, char** Argv)
{
	using namespace DCCook;
	if (Argc == 6 && std::string(Argv[1]) == "--replay-stage")
	{
		try
		{
			const auto Source = fs::canonical(Argv[2]);
			const auto Build = fs::canonical(Argv[3]);
			Pipeline P;
			P.Work = fs::absolute(Argv[4]);
			if (fs::exists(P.Work))
				throw std::runtime_error("Test work directory exists");
			P.Stage = P.Work / "packages";
			P.Runner = P.Stage / "Runner";
			P.Audit = P.Work / "logs";
			fs::create_directories(P.Runner);
			fs::create_directories(P.Audit);
			fs::create_directories(P.Work / "done");
			fs::create_directories(P.Stage / "Maps");
			for (const char* Dir : {"System", "Textures", "Sounds", "Music"})
				fs::create_directory_symlink(Source / Dir, P.Stage / Dir);
			for (const std::string& Name : std::set<std::string>{"Entry", Argv[5]})
				for (const char* Ext : {".unr", ".dlm", ".ddm"})
				{
					const auto Input = Find(Source / "Maps", Name + Ext);
					fs::copy_file(Input, P.Stage / "Maps" / Input.filename());
				}
			for (const auto& Path : Files(Source / "System", {".ini", ".int"}))
				fs::copy_file(Path, P.Runner / Path.filename());
			fs::copy_file(Build / "DCUtil/DCUtil.bin", P.Runner / "DCUtil.bin");
			for (const auto& Dir : fs::directory_iterator(Build))
				if (Dir.is_directory())
				{
					const auto Lib = Dir.path() / (Dir.path().filename().string() + ".so");
					if (fs::is_regular_file(Lib))
						fs::copy_file(Lib, P.Runner / Lib.filename());
				}
			P.Map(Argv[5]);
			std::puts("Native map pipeline replay passed");
			return 0;
		}
		catch (const std::exception& Error)
		{
			std::fprintf(stderr, "%s\n", Error.what());
			return 1;
		}
	}
	std::set<std::string> Maps, Parts, Originals;
	for (const char* Name : Campaign)
		assert(Maps.insert(Lower(Name)).second);
	assert(Maps.size() == 50);
	for (const auto& Recipe : Splits)
	{
		assert(Parts.insert(Lower(Recipe.Name)).second);
		assert(Maps.count(Lower(Recipe.Name)) && !Maps.count(Lower(Recipe.Original)));
		Originals.insert(Lower(Recipe.Original));
		std::istringstream In(Recipe.Zones);
		std::string Number;
		std::set<int> Zones;
		while (std::getline(In, Number, '+'))
		{
			const int Zone = std::stoi(Number);
			assert(Zone > 0 && Zone < 64 && Zones.insert(Zone).second);
		}
	}
	assert(Parts.size() == 14 && Originals.size() == 7);
	assert(Inside("/work/cook/maps", "/work/cook"));
	assert(!Inside("/work/cooker", "/work/cook"));
	assert(!Inside("/work", "/work/cook"));
	assert(Option("OUT", "a b") == "OUT=\"a b\"");
	bool Rejected = false;
	try
	{
		Option("OUT", "a\"b");
	}
	catch (const std::exception&)
	{
		Rejected = true;
	}
	assert(Rejected);
	Pipeline P;
	P.Work = fs::temp_directory_path() /
	         ("dc-pipeline-test-" +
	          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(P.Work / "done");
	const auto Output = P.Work / "map.dcs";
	Write(Output, {1, 2, 3});
	assert(!P.Done("map", {Output}));
	P.Complete("map", {Output});
	assert(P.Done("map", {Output}));
	Write(Output, {3, 2, 1});
	assert(!P.Done("map", {Output}));
	fs::remove(Output);
	assert(!P.Done("map", {Output}));
	fs::remove_all(P.Work);
	std::puts("Cook pipeline tests passed");
}

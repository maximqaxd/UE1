#include "Src/CookMedia.cpp"
#include "Src/CookLightmaps.cpp"
#include <cassert>
#include <iostream>

int main(int Argc, char** Argv)
{
	using namespace DCCook;
	try
	{
		Encoders Tools;
		if (Argc > 1)
		{
			const std::string Kind = Argv[1];
			if (Kind == "texture" && Argc == 3)
				Texture(fs::absolute(Argv[2]), Tools);
			else if (Kind == "sound" && Argc == 3)
				Sound(fs::absolute(Argv[2]), Tools);
			else if (Kind == "music" && Argc == 5)
				Music(fs::absolute(Argv[2]), fs::absolute(Argv[3]), fs::absolute(Argv[4]), Tools);
			else if (Kind == "lightmaps" && Argc == 6)
				Lightmaps(fs::absolute(Argv[2]), fs::absolute(Argv[3]), fs::absolute(Argv[4]),
				          fs::absolute(Argv[5]), Tools);
			else
				throw std::runtime_error("Invalid test command");
			return 0;
		}
		Bytes Format;
		Put(Format, 1, 2);
		Put(Format, 1, 2);
		Put(Format, 11025);
		Put(Format, 22050);
		Put(Format, 2, 2);
		Put(Format, 16, 2);
		const auto Encoded = MakeWave({{"fmt ", Format}, {"JUNK", {1}}, {"data", {0, 0, 1, 0}}});
		auto Decoded = Wave(Encoded);
		CheckWave(Decoded, 1, 11025, 16);
		assert(Decoded.at("data").size() == 4 && Decoded.at("JUNK") == Bytes{1});
		std::vector<Rows> Occupancy;
		auto P = Place(Occupancy, 16, 8, 1);
		assert(P.Page == 0 && P.X == 0 && P.Y == 0);
		P = Place(Occupancy, 16, 8, 1);
		assert(P.Page == 0 && P.X == 2 && P.Y == 0);
		P = Place(Occupancy, 0, 8, 1);
		assert(P.Page == 255);
		auto Bmp = Bitmap(Bytes(256 * 256 * 2, 255));
		assert(Bmp[54] == 255 && Bmp[55] == 255 && Bmp[56] == 255);
		bool Rejected = false;
		try
		{
			Wave(Bytes{0});
		}
		catch (const std::exception&)
		{
			Rejected = true;
		}
		assert(Rejected);
		std::cout << "Cook media tests passed\n";
	}
	catch (const std::exception& Error)
	{
		std::cerr << Error.what() << '\n';
		return 1;
	}
}

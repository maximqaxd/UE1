#include "Src/CookStream.cpp"
#include <cassert>
#include <chrono>

static void Save(const fs::path& Path, const Bytes& Data)
{
	std::ofstream File(Path, std::ios::binary);
	Write(File, Data.data(), Data.size());
}

int main()
{
	const fs::path Root =
	    fs::temp_directory_path() /
	    ("dc-stream-test-" +
		 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(Root / "System");
	const fs::path Raw = Root / "session.raw";
	Bytes PackageData(64, 0);
	Bytes Header;
	for (auto Value : {0x9e2a83c1u, 0u, 0u, 3u, 0u, 2u, 0u, 1u, 0u})
		Put(Header, Value);
	std::copy(Header.begin(), Header.end(), PackageData.begin());
	Save(Root / "System/Test.u", PackageData);
	Save(Raw, Bytes(PackageData.begin(), PackageData.begin() + 40));
	std::ofstream(Raw.string() + ".manifest.tsv")
	    << "# mip_policy\tdeferred\n0\t20\t0\tTest.u\n20\t20\t20\tTest.u\n";
	std::ofstream(Raw.string() + ".indices.tsv")
	    << "Test.u\t0\t2\nTest.u\t0\t0\nTest.u\t0\t2\nTest.u\t2\t1\n";
	Pack(Raw, Root / "test.dcs", Root / "System");
	const Bytes Result = Read(Root / "test.dcs");
	assert(Get(Result, 0) == 0x32534344 && Get(Result, 4) == 3);
	assert(Get(Result, 8) == 1 && Get(Result, 12) == 2 && Get(Result, 28) == 3);
	assert(Get(Result, 100) == 0x58494344);
	assert(Get(Result, 108) == 3 && Get(Result, 112) == 1 && Get(Result, 116) == 2);
	assert(Get(Result, 120) == 2 && Get(Result, 124) == 0 && Get(Result, 128) == 1);
	assert(Result[132] == 0 && Result[134] == 2 && Result[136] == 1);
	const auto Offset = Get(Result, 20);
	assert(Offset == 138 && Result.size() == Offset + 24 + 40);
	assert(Get(Result, Offset) == 0 && Get(Result, Offset + 8) == 20);
	assert(std::equal(PackageData.begin(), PackageData.begin() + 20, Result.begin() + Offset + 12));
	bool Rejected = false;
	try
	{
		Pack(Raw, Root / "test.dcs", Root / "System");
	}
	catch (const std::exception&)
	{
		Rejected = true;
	}
	assert(Rejected && Read(Root / "test.dcs") == Result);
	std::ofstream(Raw.string() + ".indices.tsv") << "Test.u\t0\t3\n";
	Rejected = false;
	try
	{
		Pack(Raw, Root / "bad.dcs", Root / "System");
	}
	catch (const std::exception&)
	{
		Rejected = true;
	}
	assert(Rejected && !fs::exists(Root / "bad.dcs"));
	std::ofstream(Raw.string() + ".manifest.tsv") << "0\t40\t0\t../../outside.u\n";
	Rejected = false;
	try
	{
		Pack(Raw, Root / "outside.dcs", Root / "System");
	}
	catch (const std::exception&)
	{
		Rejected = true;
	}
	assert(Rejected && !fs::exists(Root / "outside.dcs"));
	fs::remove_all(Root);
}

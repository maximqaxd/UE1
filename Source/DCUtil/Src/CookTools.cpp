#include "CookTools.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <cerrno>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace DCCook
{
Bytes Read(const fs::path& Path)
{
	const auto Size = fs::file_size(Path);
	if (Size >= 0x80000000ULL)
		throw std::runtime_error("Input exceeds archive limit: " + Path.string());
	Bytes Data(static_cast<size_t>(Size));
	std::ifstream File(Path, std::ios::binary);
	if (!File || (Size && !File.read(reinterpret_cast<char*>(Data.data()), Size)))
		throw std::runtime_error("Cannot read " + Path.string());
	return Data;
}

void Write(const fs::path& Path, const Bytes& Data)
{
	std::ofstream File(Path, std::ios::binary);
	if (!File ||
	    (!Data.empty() && !File.write(reinterpret_cast<const char*>(Data.data()), Data.size())))
		throw std::runtime_error("Cannot write " + Path.string());
	File.close();
	if (!File)
		throw std::runtime_error("Cannot close " + Path.string());
}

uint32_t Get(const Bytes& Data, size_t Offset, unsigned Width)
{
	if (Width > 4 || Offset > Data.size() || Width > Data.size() - Offset)
		throw std::runtime_error("Truncated binary field");
	uint32_t Value = 0;
	for (unsigned i = 0; i < Width; ++i)
		Value |= uint32_t(Data[Offset + i]) << (8 * i);
	return Value;
}

void Put(Bytes& Data, uint32_t Value, unsigned Width)
{
	if (Width > 4)
		throw std::runtime_error("Invalid binary field width");
	for (unsigned i = 0; i < Width; ++i)
		Data.push_back(static_cast<uint8_t>(Value >> (8 * i)));
}

void Set(Bytes& Data, size_t Offset, uint32_t Value, unsigned Width)
{
	Get(Data, Offset, Width);
	for (unsigned i = 0; i < Width; ++i)
		Data[Offset + i] = static_cast<uint8_t>(Value >> (8 * i));
}

std::string Lower(std::string Text)
{
	for (char& Ch : Text)
		if (Ch >= 'A' && Ch <= 'Z')
			Ch += 'a' - 'A';
	return Text;
}

void Run(const std::vector<std::string>& Args, const fs::path& Directory, const fs::path& Log,
         const std::string& Marker, const std::map<std::string, std::string>& Environment,
         const fs::path& SecondaryLog, const std::string& Progress)
{
	if (Args.empty())
		throw std::runtime_error("Empty cook command");
#if defined(_WIN32)
	throw std::runtime_error("Run the host cooker under Linux/WSL");
#else
	std::vector<char*> Argv;
	for (const auto& Arg : Args)
		Argv.push_back(const_cast<char*>(Arg.c_str()));
	Argv.push_back(nullptr);
	const auto AbsoluteLog = fs::absolute(Log);
	const pid_t Child = fork();
	if (Child < 0)
		throw std::runtime_error("Cannot fork cooker worker");
	if (Child == 0)
	{
		setpgid(0, 0);
		int Fd = open(AbsoluteLog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (Fd < 0 || dup2(Fd, STDOUT_FILENO) < 0 || dup2(Fd, STDERR_FILENO) < 0)
			_exit(126);
		close(Fd);
		if (chdir(Directory.c_str()) != 0)
			_exit(126);
		for (const auto& Item : Environment)
			if (setenv(Item.first.c_str(), Item.second.c_str(), 1))
				_exit(126);
		execvp(Argv[0], Argv.data());
		_exit(127);
	}
	setpgid(Child, Child);
	int Status = 0;
	const auto Started = std::chrono::steady_clock::now();
	const auto Deadline = Started + std::chrono::minutes(20);
	auto NextUpdate = Started + std::chrono::seconds(15);
	std::streamoff LogOffset = 0;
	auto ReportWorker = [&]()
	{
		if (Progress.empty())
			return false;
		std::ifstream File(AbsoluteLog);
		if (!File)
			return false;
		File.seekg(LogOffset);
		const std::string New((std::istreambuf_iterator<char>(File)),
		                      std::istreambuf_iterator<char>());
		size_t Start = 0;
		bool Reported = false;
		for (size_t End; (End = New.find('\n', Start)) != std::string::npos; Start = End + 1)
		{
			const std::string Line = New.substr(Start, End - Start);
			if (Line.compare(0, 7, "DCCOOK ") == 0)
			{
				std::printf("Cooker: %s\n", Line.c_str() + 7);
				std::fflush(stdout);
				Reported = true;
			}
		}
		LogOffset += static_cast<std::streamoff>(Start);
		return Reported;
	};
	for (;;)
	{
		const auto Done = waitpid(Child, &Status, WNOHANG);
		const auto Now = std::chrono::steady_clock::now();
		if (!Progress.empty() && (Done == Child || Now >= NextUpdate))
		{
			const bool Reported = ReportWorker();
			if (Done != Child && !Reported)
			{
				const auto Seconds =
				    std::chrono::duration_cast<std::chrono::seconds>(Now - Started).count();
				std::printf("Cooker: %s (%llds elapsed)\n", Progress.c_str(),
				            static_cast<long long>(Seconds));
				std::fflush(stdout);
			}
			NextUpdate = Now + std::chrono::seconds(15);
		}
		if (Done == Child)
			break;
		if (Done < 0 && errno != EINTR)
			throw std::runtime_error("Cannot wait for cooker worker");
		if (std::chrono::steady_clock::now() >= Deadline)
		{
			kill(-Child, SIGKILL);
			while (waitpid(Child, &Status, 0) < 0 && errno == EINTR)
			{
			}
			throw std::runtime_error("Cook command timed out; see " + AbsoluteLog.string());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	const auto Data = Read(AbsoluteLog);
	std::string Text(Data.begin(), Data.end());
	if (!SecondaryLog.empty() && fs::is_regular_file(SecondaryLog))
	{
		const auto Extra = Read(SecondaryLog);
		Text += "\n" + std::string(Extra.begin(), Extra.end());
		Write(AbsoluteLog, Bytes(Text.begin(), Text.end()));
	}
	const bool GoodStatus = WIFEXITED(Status) && (WEXITSTATUS(Status) == 0 ||
	                                              (!Marker.empty() && WEXITSTATUS(Status) == 1));
	if (!GoodStatus || (!Marker.empty() && Text.find(Marker) == std::string::npos) ||
	    Text.find("Critical:") != std::string::npos)
		throw std::runtime_error("Cook command failed: " + Args[0] + "; see " +
		                         AbsoluteLog.string());
#endif
}
} // namespace DCCook

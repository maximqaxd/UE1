#include "DCStateDelta.h"
#include <assert.h>
#include <random>

int main()
{
	std::mt19937 Random(12345);
	for (unsigned Test=0; Test<3000; ++Test)
	{
		std::vector<uint8_t> Base(Random()%2048), Target, Code, Result;
		for (unsigned i=0; i<Base.size(); ++i) Base[i]=Random()%32;
		Target=Base;
		for (unsigned i=0; i<20; ++i)
		{
			if (Target.empty() || Random()%3==0)
				Target.insert(Target.begin()+(Random()%(Target.size()+1)), Random());
			else if (Random()%2) Target.erase(Target.begin()+Random()%Target.size());
			else Target[Random()%Target.size()]=Random();
		}
		DCStateDelta::Encode(Base, Target, Code);
		assert(DCStateDelta::Decode(Base, Code, Target.size(), Result));
		assert(Result==Target);
		if (!Code.empty())
		{
			Code.pop_back();
			assert(!DCStateDelta::Decode(Base, Code, Target.size(), Result));
		}
	}
	std::vector<uint8_t> Base(32768, 7), Code, Result;
	DCStateDelta::Encode(Base, Base, Code);
	assert(DCStateDelta::Decode(Base, Code, Base.size(), Result) && Result==Base);
	assert(!DCStateDelta::Decode(Base, {0,255,255,1,0}, 1, Result));
	assert(!DCStateDelta::Decode(Base, {1,0,0}, 0, Result));
	assert(!DCStateDelta::Decode(Base, {2,1,0,0}, 1, Result));
	assert(!DCStateDelta::Decode(Base, {}, 32769, Result));
}

#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>

namespace DCStateDelta
{
enum { Limit = 32768 };
inline unsigned Hash(const uint8_t* P)
{
	uint32_t Word;
	memcpy(&Word, P, 4);
	return (Word*2654435761u)>>21;
}
inline void Word(std::vector<uint8_t>& Out, unsigned Value)
{
	Out.push_back(Value&255); Out.push_back(Value>>8);
}
inline void Encode(const std::vector<uint8_t>& Base, const std::vector<uint8_t>& Target,
	std::vector<uint8_t>& Out)
{
	uint16_t Table[2048];
	for (unsigned i=0; i<2048; ++i) Table[i]=0xffff;
	for (unsigned i=0; i+8<=Base.size(); ++i) Table[Hash(&Base[i])]=i;
	unsigned Pos=0, Literal=0;
	Out.clear();
	while (Pos<Target.size())
	{
		int Offset=Pos+8<=Target.size() ? Table[Hash(&Target[Pos])] : -1;
		unsigned Length=0;
		if (Offset>=0 && Offset!=0xffff)
			while (Pos+Length<Target.size() && (unsigned)Offset+Length<Base.size()
				&& Target[Pos+Length]==Base[Offset+Length]) ++Length;
		if (Length>=8)
		{
			if (Pos>Literal)
			{
				Out.push_back(1); Word(Out, Pos-Literal);
				Out.insert(Out.end(), Target.begin()+Literal, Target.begin()+Pos);
			}
			Out.push_back(0); Word(Out, Offset); Word(Out, Length);
			Pos+=Length; Literal=Pos;
		}
		else ++Pos;
	}
	if (Pos>Literal)
	{
		Out.push_back(1); Word(Out, Pos-Literal);
		Out.insert(Out.end(), Target.begin()+Literal, Target.end());
	}
}
inline bool Decode(const std::vector<uint8_t>& Base, const std::vector<uint8_t>& Code,
	unsigned Size, std::vector<uint8_t>& Target)
{
	if (Size>Limit || Code.size()>Limit*2) return false;
	Target.clear();
	unsigned Pos=0;
	while (Pos<Code.size())
	{
		unsigned Kind=Code[Pos++], Offset=0;
		if (Kind>1 || Pos+2>Code.size()) return false;
		unsigned Count=Code[Pos] | (Code[Pos+1]<<8); Pos+=2;
		if (!Kind)
		{
			Offset=Count;
			if (Pos+2>Code.size()) return false;
			Count=Code[Pos] | (Code[Pos+1]<<8); Pos+=2;
		}
		if (!Count || Count>Size-Target.size()) return false;
		if (Kind)
		{
			if (Count>Code.size()-Pos) return false;
			Target.insert(Target.end(), Code.begin()+Pos, Code.begin()+Pos+Count); Pos+=Count;
		}
		else
		{
			if (Offset>Base.size() || Count>Base.size()-Offset) return false;
			Target.insert(Target.end(), Base.begin()+Offset, Base.begin()+Offset+Count);
		}
	}
	return Target.size()==Size;
}
}

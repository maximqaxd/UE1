/*=============================================================================
	UnAudio.cpp: Unreal base audio.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

Revision history:
	* Created by Tim Sweeney
	* Wave modification code by Erik de Neve
=============================================================================*/

#include "EnginePrivate.h" 

/*-----------------------------------------------------------------------------
	USound implementation.
-----------------------------------------------------------------------------*/

void USound::Serialize( FArchive& Ar )
{
	guard(USound::Serialize);
	Super::Serialize( Ar );
	Ar << FileType;
	if( Ar.IsLoading() || Ar.IsSaving() )
	{
		Ar << Data;
		if( Ar.IsLoading() )
		{
			// Derive these from the exposed 'low quality' preference setting.
			INT Force8Bit = 0;
			INT ForceHalve = 0;
			if( Audio && Audio->GetLowQualitySetting() && !GIsEditor )
			{
				Force8Bit = 1;
				ForceHalve = 1;
			}

			// Frequencies below this sample rate will NOT be downsampled.
			DWORD FreqThreshold = 11050;

			// Reduce sound frequency and/or bit depth if required.
			if( Force8Bit || ForceHalve )
			{		
				// If ReadWaveInfo returns true, all relevant Wave chunks were found and 
				// all pointers in the WaveInfo structure have been successfully initialized.
				FWaveModInfo WaveInfo;
				if( WaveInfo.ReadWaveInfo(Data) &&  (WaveInfo.SampleDataSize > 4)  ) 
				{				
					// Three main conversions:
					// * Halving the frequency -> simple 0.25, 0.50, 0.25 kernel. 
					// * Reducing bit-depth 8->16  
					// * Both in one sweep.  
					//
					// Important: Wave data ALWAYS padded to use 16-bit alignment even
					// though the number of bytes in pWaveDataSize may be odd.	
					UBOOL ReduceBits = ((Force8Bit) && (*WaveInfo.pBitsPerSample == 16));
					UBOOL ReduceFreq = ((ForceHalve) && (*WaveInfo.pSamplesPerSec >= FreqThreshold));
					if( ReduceBits && ReduceFreq )
					{
						// Convert 16-bit sample to 8 bit and halve the frequency too.
						WaveInfo.HalveReduce16to8();
					}
					else if (ReduceBits && (!ReduceFreq))
					{	
						// Convert 16-bit sample down to 8-bit.
						WaveInfo.Reduce16to8();
					}
					else if( ReduceFreq )
					{
						// Just halve the frequency. Separate cases for 16 and 8 bits.
						WaveInfo.HalveData();				
					}
					WaveInfo.UpdateWaveData( Data );
				}
			}

			// Register it.
			OriginalSize = Data.Num();
			if( Audio && !GIsEditor )
			{
				Audio->RegisterSound( this );
#if defined(PLATFORM_DREAMCAST)
				// On Dreamcast, once the audio backend has uploaded the sample
				// into AICA RAM we do not need to keep another copy in system RAM.
				Data.Empty();
#endif
			}
		}
	}
	else
	{
		Ar.CountBytes( OriginalSize );
	}

	unguard;
}
void USound::Destroy()
{
	guard(USound::Destroy);
	if( Audio )
		Audio->UnregisterSound( this );
	Super::Destroy();
	unguard;
}
void USound::Export( FOutputDevice& Out, const char* FileType, INT Indent )
{
	guard(USound::Export);
	Out.WriteBinary( &Data(0), Data.Num() );
	unguard;
}

void USound::PostLoad()
{
	guard(USound::PostLoad);
	Super::PostLoad();
	unguard;
};
UAudioSubsystem* USound::Audio;
IMPLEMENT_CLASS(USound);


/*-----------------------------------------------------------------------------
	WaveModInfo implementation - downsampling of wave files.
-----------------------------------------------------------------------------*/
//
//	Figure out the WAVE file layout.
//
UBOOL FWaveModInfo::ReadWaveInfo( TArray<BYTE>& WavData )
{
	guard(FWaveModInfo::ReadWaveInfo);

	if( WavData.Num() < (INT)sizeof(FRiffWaveHeader) )
		return 0;

	BYTE* WaveStart = &WavData(0);
	WaveDataEnd = WaveStart + WavData.Num();

	FRiffWaveHeader RiffHeader;
	appMemcpy( &RiffHeader, WaveStart, sizeof(FRiffWaveHeader) );

	// Verify we've got a real 'WAVE' header.
	if( RiffHeader.wID != mmioFOURCC('W','A','V','E') )
		return 0;

	pMasterSize = (DWORD*)( WaveStart + STRUCT_OFFSET( FRiffWaveHeader, ChunkLen ) );

	// Helper lambda replacement to safely walk chunks.
	BYTE* ChunkCursor = WaveStart + sizeof(FRiffWaveHeader);
	FRiffChunk ChunkHeader;

	// Look for the 'fmt ' chunk.
	while( ChunkCursor + sizeof(FRiffChunk) <= WaveDataEnd )
	{
		appMemcpy( &ChunkHeader, ChunkCursor, sizeof(FRiffChunk) );
		if( ChunkHeader.ChunkID == mmioFOURCC('f','m','t',' ') )
			break;

		ChunkCursor += Pad16Bit( ChunkHeader.ChunkLen ) + sizeof(FRiffChunk);
	}

	if( ChunkCursor + sizeof(FRiffChunk) > WaveDataEnd || ChunkHeader.ChunkID != mmioFOURCC('f','m','t',' ') )
		return 0;

	BYTE* FmtData = ChunkCursor + sizeof(FRiffChunk);
	if( FmtData + sizeof(FFormatChunk) > WaveDataEnd )
		return 0;

	FFormatChunk FmtCopy;
	appMemcpy( &FmtCopy, FmtData, sizeof(FFormatChunk) );

	pBitsPerSample  = (_WORD*)( FmtData + STRUCT_OFFSET( FFormatChunk, wBitsPerSample ) );
	pSamplesPerSec  = (DWORD*)( FmtData + STRUCT_OFFSET( FFormatChunk, nSamplesPerSec ) );
	pAvgBytesPerSec = (DWORD*)( FmtData + STRUCT_OFFSET( FFormatChunk, nAvgBytesPerSec ) );
	pBlockAlign		= (_WORD*)( FmtData + STRUCT_OFFSET( FFormatChunk, nBlockAlign ) );
	pChannels       = (_WORD*)( FmtData + STRUCT_OFFSET( FFormatChunk, nChannels ) );

	// Look for the 'data' chunk.
	ChunkCursor = WaveStart + sizeof(FRiffWaveHeader);
	while( ChunkCursor + sizeof(FRiffChunk) <= WaveDataEnd )
	{
		appMemcpy( &ChunkHeader, ChunkCursor, sizeof(FRiffChunk) );
		if( ChunkHeader.ChunkID == mmioFOURCC('d','a','t','a') )
			break;

		ChunkCursor += Pad16Bit( ChunkHeader.ChunkLen ) + sizeof(FRiffChunk);
	}

	if( ChunkCursor + sizeof(FRiffChunk) > WaveDataEnd || ChunkHeader.ChunkID != mmioFOURCC('d','a','t','a') )
		return 0;

	SampleDataStart = ChunkCursor + sizeof(FRiffChunk);
	pWaveDataSize   = (DWORD*)( ChunkCursor + STRUCT_OFFSET( FRiffChunk, ChunkLen ) );
	SampleDataSize  = ChunkHeader.ChunkLen;
	OldBitsPerSample = FmtCopy.wBitsPerSample;
	SampleDataEnd   = SampleDataStart + SampleDataSize;

	if( SampleDataEnd > WaveDataEnd )
		return 0;

	NewDataSize	= SampleDataSize;

	// Look for a 'smpl' chunk (optional).
	ChunkCursor = WaveStart + sizeof(FRiffWaveHeader);
	while( ChunkCursor + sizeof(FRiffChunk) <= WaveDataEnd )
	{
		appMemcpy( &ChunkHeader, ChunkCursor, sizeof(FRiffChunk) );
		if( ChunkHeader.ChunkID == mmioFOURCC('s','m','p','l') )
			break;

		ChunkCursor += Pad16Bit( ChunkHeader.ChunkLen ) + sizeof(FRiffChunk);
	}

	// Chunk found ? smpl chunk is optional.
	// Find the first sample-loop structure, and the total number of them.
	if( ChunkCursor + sizeof(FRiffChunk) <= WaveDataEnd && ChunkHeader.ChunkID == mmioFOURCC('s','m','p','l') )
	{
		BYTE* SampleChunkData = ChunkCursor + sizeof(FRiffChunk);
		if( SampleChunkData + sizeof(FSampleChunk) <= WaveDataEnd )
		{
			FSampleChunk SampleChunkCopy;
			appMemcpy( &SampleChunkCopy, SampleChunkData, sizeof(FSampleChunk) );
			SampleLoopsNum  = SampleChunkCopy.cSampleLoops; // Number of tSampleLoop structures.
			// First tSampleLoop structure starts right after the tSampleChunk.
			pSampleLoop = (FSampleLoop*) ( SampleChunkData + sizeof(FSampleChunk) ); 
		}
	}
		
	return 1;
	unguard;
}


//
// Update internal variables and shrink the data fields.
//
UBOOL FWaveModInfo::UpdateWaveData( TArray<BYTE>& WavData )
{
	guard(FWaveModInfo::UpdateWaveData);
	if( NewDataSize < SampleDataSize )
	{		
		// Shrinkage of data chunk in bytes -> chunk data must always remain 16-bit-padded.
		INT ChunkShrinkage = Pad16Bit(SampleDataSize)  - Pad16Bit(NewDataSize);

		// Update sizes.
		*pWaveDataSize  = NewDataSize;
		*pMasterSize   -= ChunkShrinkage;

		// Refresh all wave parameters depending on bit depth and/or sample rate.
		*pBlockAlign    =  *pChannels * (*pBitsPerSample >> 3); // channels * Bytes per sample
		*pAvgBytesPerSec = *pBlockAlign * *pSamplesPerSec; //sample rate * Block align

		// Update 'smpl' chunk data also, if present.
		if (SampleLoopsNum && pSampleLoop)
		{
			BYTE* SampleLoopPtr = (BYTE*)pSampleLoop;
			INT SampleDivisor = ( (SampleDataSize *  *pBitsPerSample) / (NewDataSize ) );
			for (INT SL = 0; SL<SampleLoopsNum; SL++)
			{
				FSampleLoop LoopCopy;
				appMemcpy( &LoopCopy, SampleLoopPtr, sizeof(FSampleLoop) );
				LoopCopy.dwStart = LoopCopy.dwStart  * OldBitsPerSample / SampleDivisor;
				LoopCopy.dwEnd   = LoopCopy.dwEnd  * OldBitsPerSample / SampleDivisor;
				appMemcpy( SampleLoopPtr, &LoopCopy, sizeof(FSampleLoop) );
				SampleLoopPtr += sizeof(FSampleLoop); // Next TempSampleLoop structure.
			}	
		}		
			
		// Now shuffle back all data after wave data by SampleDataSize/2 (+ padding) bytes 
		// INT SampleShrinkage = ( SampleDataSize/4) * 2;
		BYTE* NewWaveDataEnd = SampleDataEnd - ChunkShrinkage;

		for ( INT pt = 0; pt< ( WaveDataEnd -  SampleDataEnd); pt++ )
		{ 
			NewWaveDataEnd[pt] =  SampleDataEnd[pt];
		}			

		// Resize the dynamic array.
		WavData.SetNum( WavData.Num() - ChunkShrinkage );
		
		/*
		static INT SavedBytes = 0;
		SavedBytes += ChunkShrinkage;
		debugf(NAME_Log," Audio shrunk by: %i bytes, total savings %i bytes.",ChunkShrinkage,SavedBytes);
		debugf(NAME_Log," New BitsPerSample: %i ", *pBitsPerSample);
		debugf(NAME_Log," New SamplesPerSec: %i ", *pSamplesPerSec);
		debugf(NAME_Log," Olddata/NEW*wav* sizes: %i %i ", SampleDataSize, *pMasterSize);
		*/
	}

	// Noise gate filtering assumes 8-bit sound.
	// Warning: While very useful on SOME sounds, it erased too many low-volume sound fx
	// in its current form - even when 'noise' level scaled to average sound amplitude.
	// if (NoiseGate) NoiseGateFilter();

	return 1;
	unguard;
}

//
// Reduce bit depth and halve the number of samples simultaneously.
//
void FWaveModInfo::HalveReduce16to8()
{
	guard(FWaveModInfo::HalveReduce16to8);

	DWORD SampleWords =  SampleDataSize >> 1;
	INT OrigValue,NewValue;
	INT ErrorDiff = 0;

	DWORD SampleBytes = SampleWords >> 1;

	INT NextSample0 = (INT)((SWORD*) SampleDataStart)[0];
	INT NextSample1,NextSample2;

	BYTE* SampleData =  SampleDataStart;
	for (DWORD T=0; T<SampleBytes; T++)
	{
		NextSample1 = (INT)((SWORD*)SampleData)[T*2];
		NextSample2 = (INT)((SWORD*)SampleData)[T*2+1];
		INT Filtered16BitSample = 32768*4 + NextSample0 + NextSample1 + NextSample1 + NextSample2;
		NextSample0 = NextSample2;

		// Error diffusion works in '18 bit' resolution.
		OrigValue = ErrorDiff + Filtered16BitSample;
		// Rounding: "+0.5"
		NewValue  = (OrigValue + 512) & 0xfffffc00;
		if (NewValue > 0x3fc00) NewValue = 0x3fc00;

		INT NewSample = NewValue >> (8+2);
		SampleData[T] = (BYTE)NewSample;	// Output byte.

		ErrorDiff = OrigValue - NewValue;   // Error cycles back into input.
	}

	NewDataSize = SampleBytes;

	*pBitsPerSample  = 8;
	*pSamplesPerSec  = *pSamplesPerSec >> 1;

	NoiseGate = true;
	unguard;
}

//
// Reduce bit depth.
//
void FWaveModInfo::Reduce16to8()
{
	guard(FWaveModInfo::Reduce16to8);

	DWORD SampleBytes =  SampleDataSize >> 1;
	INT OrigValue,NewValue;
	INT ErrorDiff = 0;
	BYTE* SampleData = SampleDataStart;

	for (DWORD T=0; T<SampleBytes; T++)
	{
		// Error diffusion works in 16 bit resolution.
		OrigValue = ErrorDiff + 32768 + (INT)((SWORD*)SampleData)[T];
		// Rounding: '+0.5', then mask off low 2 bits.
		NewValue  = (OrigValue + 127 ) & 0xffffff00;  // + 128
		if (NewValue > 0xff00) NewValue = 0xff00;

		INT NewSample = NewValue >> 8;
		SampleData[T] = NewSample;

		ErrorDiff = OrigValue - NewValue;  // Error cycles back into input.
	}				

	NewDataSize = SampleBytes;
	*pBitsPerSample  = 8;
	NoiseGate = true;

	unguard;
}

//
// Halve the number of samples.
//
void FWaveModInfo::HalveData()
{
	guard(FWaveModInfo::HalveData);
	if( *pBitsPerSample == 16)
	{						
		DWORD SampleWords =  SampleDataSize >> 1;
		INT OrigValue,NewValue;
		INT ErrorDiff = 0;

		DWORD ScaledSampleWords = SampleWords >> 1; 

		INT NextSample0 = (INT)((SWORD*) SampleDataStart)[0];
		INT NextSample1, NextSample2;

		BYTE* SampleData =  SampleDataStart;
		for (DWORD T=0; T<ScaledSampleWords; T++)
		{
			NextSample1 = (INT)((SWORD*)SampleData)[T*2];
			NextSample2 = (INT)((SWORD*)SampleData)[T*2+1];
			INT Filtered18BitSample = 32768*4 + NextSample0 + NextSample1 + NextSample1 + NextSample2;
			NextSample0 = NextSample2;

			// Error diffusion works with '18 bit' resolution.
			OrigValue = ErrorDiff + Filtered18BitSample;
			// Rounding: '+0.5', then mask off low 2 bits.
			NewValue  = (OrigValue + 2) & 0x3fffc;
			if (NewValue > 0x3fffc) NewValue = 0x3fffc;
			((SWORD*)SampleData)[T] = (NewValue >> 2) - 32768;  // Output SWORD.
			ErrorDiff = OrigValue - NewValue; // Error cycles back into input.
		}				
		NewDataSize = (ScaledSampleWords * 2);
		*pSamplesPerSec  = *pSamplesPerSec >> 1;
	}
	else if( *pBitsPerSample == 8 )
	{									
		INT OrigValue,NewValue;
		INT ErrorDiff = 0;
	
		DWORD SampleBytes = SampleDataSize >> 1;  
		BYTE* SampleData = SampleDataStart;

		INT NextSample0 = SampleData[0];
		INT NextSample1, NextSample2;

		for (DWORD T=0; T<SampleBytes; T++)
		{
			NextSample1 =  SampleData[T*2];
			NextSample2 =  SampleData[T*2+1];
			INT Filtered10BitSample =  NextSample0 + NextSample1 + NextSample1 + NextSample2;
			NextSample0 =  NextSample2;

			// Error diffusion works with '10 bit' resolution.
			OrigValue = ErrorDiff + Filtered10BitSample;
			// Rounding: '+0.5', then mask off low 2 bits.
			NewValue  = (OrigValue + 2) & 0x3fc;
			if (NewValue > 0x3fc) NewValue = 0x3fc;
			SampleData[T] = (BYTE)(NewValue >> 2);	// Output BYTE.
			ErrorDiff = OrigValue - NewValue;		// Error cycles back into input.
		}				
		NewDataSize = SampleBytes; 
		*pSamplesPerSec  = *pSamplesPerSec >> 1;
	}
	unguard;
}

//
//	Noise gate filter. Hard to make general-purpose without hacking up some (soft) sounds.
//
void FWaveModInfo::NoiseGateFilter()
{
	guard(FWaveModInfo::NoiseGateFilter);

	BYTE* SampleData  =  SampleDataStart;
	INT   SampleBytes = *pWaveDataSize; 
	INT	  MinBlankSize = 860 * ((*pSamplesPerSec)/11025); // 600-800...

	// Threshold sound amplitude. About 18 seems OK.
	INT Amplitude	 = 18;

	// Ignore any over-threshold signals if under this size. ( 32 ?)
	INT GlitchSize	 = 32 * ((*pSamplesPerSec)/11025);
	INT StartSilence =  0;
	INT EndSilence	 =  0;
	INT LastErased   = -1;

	for( INT T = 0; T< SampleBytes; T++ )
	{
		UBOOL Loud;
		if	( Abs(SampleData[T]-128) >= Amplitude )
		{
			Loud = true;							
			if (StartSilence > 0)
			{
				if ( (T-StartSilence) < GlitchSize ) Loud = false;
			}							
		}
		else Loud = false;

		if( StartSilence == 0 )
		{
			if( !Loud )
				StartSilence = T;
		}
		else
		{													
			if( ((EndSilence == 0) && (Loud)) || (T ==(SampleBytes-1) ) )
			{
				EndSilence = T;
				//
				// Erase an area of low-amplitude sound ( noise... ) if size >= MinBlankSize.
				//
				// Todo: try erasing smoothly - decay, create some attack, 
				// proportional to the size of the area. ?
				//
				if	(( EndSilence - StartSilence) >= MinBlankSize )
				{					
					for ( INT Er = StartSilence; Er< EndSilence; Er++ )
					{
						SampleData[Er] = 128;
					}
				}
				LastErased = EndSilence-1;
				StartSilence = 0;
				EndSilence   = 0;
			}
		}										
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	UMusic implementation.
-----------------------------------------------------------------------------*/

void UMusic::Serialize( FArchive& Ar )
{
	guard(UMusic::Serialize);
	Super::Serialize( Ar );
	Ar << FileType;
#ifndef PLATFORM_LOW_MEMORY
	if( Ar.IsLoading() || Ar.IsSaving() )
	{
		Ar << Data;
		if( Ar.IsLoading() )
		{
			OriginalSize = Data.Num();
		}
	}
	else
#endif
	{
		Ar.CountBytes( OriginalSize );
	}
#ifdef PLATFORM_LOW_MEMORY
	OriginalSize = 0;
#endif
	unguard;
}
void UMusic::Destroy()
{
	guard(UMusic::Destroy);
	if( Audio )
		Audio->UnregisterMusic( this );
	Super::Destroy();
	unguard;
}
void UMusic::Export( FOutputDevice& Out, const char* FileType, int Indent )
{
	guard(UMusic::Export);
	Out.WriteBinary( &Data(0), Data.Num() );
	unguard;
}
void UMusic::PostLoad()
{
	guard(UMusic::PostLoad);
	Super::PostLoad();
	unguard;
}
UAudioSubsystem* UMusic::Audio;
IMPLEMENT_CLASS(UMusic);

/*-----------------------------------------------------------------------------
	UAudioSubsystem implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_CLASS(UAudioSubsystem);

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

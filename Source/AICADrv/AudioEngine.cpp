/**
 * Audio Engine For Dreamcast by Josh Pearson.
 * Leverages routines built for DCA project, thanks to SKMP for the core functionality here :-)
 * 
 * There are Three Library Functions In Total:
 * 
 * bool AudioEngine_Initialise(void)
 * uint8 AudioEngine_Load(const char * fname, uint32_t seek_bytes_aligned);
 * void AudioEngine_Play(uint8 nStream, uint8 volume, uint8 pan);
 * 
 * 
 * Sound Files Can be Any Length! 
 * If it is small enough to load into a single shot, it will be loaded directly into Sound RAM and played as a SFX.
 * If the file is too large to be played in a single shot, it will be streamed into SRAM as needed in a single thread.
 * 
 */
#include <kos.h>
#include <string>
#include <assert.h>
#include <thread>
#include <mutex>
#include <atomic>

#include "AicaInterface.h"
#include "AudioEngine.h"

stream_info streams[AUDIO_ENGINE_MAX_STREAMS];
sfx_chnnel sfx_channels[AUDIO_ENGINE_MAX_CHANNELS];
sfx_info sfx[AUDIO_ENGINE_MAX_SFX];
static uint8_t sfx_buffer[AICA_MAX_SAMPLES * 2];
static volatile uint32_t ticks;
static bool arm_program_loaded = false;
static int current_playing_music = -1;  // Currently playing music ID
static bool audio_resources_owned = false;
static std::atomic<bool> audio_worker_running(false);

extern "C" uint32_t AICA_GetSampleBytes()
{
	uint32_t Total = 0;
	for( int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; ++i )
	{
		for( int side = 0; side < 2; ++side )
		{
			if( streams[i].aica_buffers[side] )
				Total += STREAM_CHANNEL_BUFFER_SIZE;
		}
	}
	for( int i = 0; i < AUDIO_ENGINE_MAX_SFX; ++i )
	{
		if( sfx[i].aica_buffer && sfx[i].nSfx >= 0 )
		{
			int Bits = sfx[i].type == AICA_SM_16BIT ? 16 : sfx[i].type == AICA_SM_8BIT ? 8 : 4;
			Total += (sfx[i].total_samples * Bits + 7) / 8;
		}
	}
	return Total;
}

std::mutex channel_mtx;
std::thread snd_thread;

void AudioEngine_Shutdown(void)
{
	// Never hold channel_mtx while joining: the worker needs it to finish.
	audio_worker_running.store(false);
	if( snd_thread.joinable() )
		snd_thread.join();
	if( !audio_resources_owned )
		return;

	// No worker can read game memory, submit SQ transfers, or touch the AICA
	// allocations after this point. Stop every channel before releasing RAM.
	for( int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; ++i )
	{
		for( int side = 0; side < 2; ++side )
		{
			if( streams[i].mapped_ch[side] >= 0 )
			{
				aica_stop_chn(streams[i].mapped_ch[side]);
				snd_sfx_chn_free(streams[i].mapped_ch[side]);
				streams[i].mapped_ch[side] = -1;
			}
		}
	}
	for( int i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; ++i )
	{
		if( sfx_channels[i].mapped_ch >= 0 )
		{
			aica_stop_chn(sfx_channels[i].mapped_ch);
			snd_sfx_chn_free(sfx_channels[i].mapped_ch);
		}
		sfx_channels[i].mapped_ch = -1;
		sfx_channels[i].sfx_index = -1;
	}
	// STOP packets are queued, not acknowledgements. Disable the sound CPU
	// before freeing samples so no queued command can reuse old addresses.
	spu_disable();
	for( int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; ++i )
	{
		for( int side = 0; side < 2; ++side )
		{
			if( streams[i].aica_buffers[side] )
				snd_mem_free(streams[i].aica_buffers[side]);
			streams[i].aica_buffers[side] = 0;
		}
		if( streams[i].fd >= 0 )
			fs_close(streams[i].fd);
		streams[i].fd = -1;
		streams[i].playing = false;
		streams[i].mem_data = nullptr;
		streams[i].mem_size = streams[i].mem_offset = 0;
		streams[i].is_memory = false;
	}
	for( int i = 0; i < AUDIO_ENGINE_MAX_SFX; ++i )
	{
		if( sfx[i].aica_buffer )
			snd_mem_free(sfx[i].aica_buffer);
		sfx[i].aica_buffer = 0;
		sfx[i].nSfx = -1;
	}
	current_playing_music = -1;
	snd_shutdown();
	audio_resources_owned = false;
	debugf("DCAUDIO shutdown worker_joined=1 sample_bytes=%u\n", AICA_GetSampleBytes());
}
 
static void StreamRead(int nStream, void* buf, uint32_t size) {
	if(streams[nStream].is_memory) {
		// Read from memory
		uint32_t available = streams[nStream].mem_size - streams[nStream].mem_offset;
		uint32_t to_read = (size < available) ? size : available;
		memcpy(buf, streams[nStream].mem_data + streams[nStream].mem_offset, to_read);
		streams[nStream].mem_offset += to_read;
		// Zero-pad if we read less than requested
		if(to_read < size) {
			memset((uint8_t*)buf + to_read, 0, size - to_read);
		}
	} else {
		// Read from file
		fs_read(streams[nStream].fd, buf, size);
	}
}

// Stream uploads deliberately avoid the _sq variants. spu_memload_sq takes the
// global store-queue mutex, which UPVRRenderDevice::Lock holds from
// pvr_list_begin until Unlock, i.e. for a whole frame. The audio worker runs
// these while holding channel_mtx, so using _sq here lets the worker block on
// the renderer's lock with channel_mtx held while the main thread waits on
// channel_mtx, deadlocking the frame. spu_memload goes over G2 instead and
// takes no such lock; 4K every ~370ms costs far less than a lost frame.
void AudioEngine_LoadFirstChunk(int nStream) {
	 uint32_t read_size = streams[nStream].stereo ? STREAM_STAGING_READ_SIZE_STEREO : STREAM_STAGING_READ_SIZE_MONO;
	 StreamRead(nStream, streams[nStream].buffer, read_size);
	if (streams[nStream].stereo) {
		snd_adpcm_split((uint32_t*)streams[nStream].buffer, (uint32_t*)sfx_buffer, (uint32_t*)sfx_buffer + STREAM_STAGING_READ_SIZE_MONO, STREAM_STAGING_READ_SIZE_STEREO);
		spu_memload(streams[nStream].aica_buffers[0], (uint32_t*)sfx_buffer, STREAM_STAGING_READ_SIZE_STEREO/2);
		spu_memload(streams[nStream].aica_buffers[1], (uint32_t*)sfx_buffer + STREAM_STAGING_READ_SIZE_MONO, STREAM_STAGING_READ_SIZE_STEREO/2);
	}
	else {
		spu_memload(streams[nStream].aica_buffers[0], streams[nStream].buffer, STREAM_CHANNEL_BUFFER_SIZE/2);
	}

	if (streams[nStream].total_samples > STREAM_CHANNEL_SAMPLE_COUNT/2) {
		// If more than one buffer, prefetch the next one
		 StreamRead(nStream, streams[nStream].buffer, read_size);
	}	
}

bool AudioEngine_Initialise(void)
{
	if( audio_resources_owned || snd_thread.joinable() )
		AudioEngine_Shutdown();
	auto init = snd_init();
	if( init < 0 )
		return false;
	ticks = 0;
	memset(streams, 0, sizeof(streams));
	memset(sfx, 0, sizeof(sfx));
	for( int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; ++i )
	{
		streams[i].fd = -1;
		streams[i].mapped_ch[0] = streams[i].mapped_ch[1] = -1;
	}
	for( int i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; ++i )
	{
		sfx_channels[i].mapped_ch = -1;
		sfx_channels[i].sfx_index = -1;
	}
	audio_resources_owned = true;

	for (int i = 0; i< AUDIO_ENGINE_MAX_STREAMS; i++) {
		streams[i].mapped_ch[0] = snd_sfx_chn_alloc();
		streams[i].mapped_ch[1] = snd_sfx_chn_alloc();
		streams[i].aica_buffers[0] = snd_mem_malloc(STREAM_CHANNEL_BUFFER_SIZE);
		streams[i].aica_buffers[1] = snd_mem_malloc(STREAM_CHANNEL_BUFFER_SIZE);
		if( streams[i].mapped_ch[0] < 0 || streams[i].mapped_ch[1] < 0
			|| !streams[i].aica_buffers[0] || !streams[i].aica_buffers[1] )
		{
			AudioEngine_Shutdown();
			return false;
		}
		streams[i].fd = -1;
		 streams[i].is_memory = false;
		 streams[i].mem_data = nullptr;
		 streams[i].mem_size = 0;
		 streams[i].mem_offset = 0;
		streams[i].vol = 255;
		streams[i].loop = 0;
		streams[i].pan[0] = AICA_PAN_LEFT;
		streams[i].pan[1] = AICA_PAN_RIGHT;
		streams[i].last_tick = ticks;
	}

	for (int i = 0; i < (AUDIO_ENGINE_MAX_CHANNELS); i++) {
		sfx_channels[i].mapped_ch = snd_sfx_chn_alloc();   
		if( sfx_channels[i].mapped_ch < 0 )
		{
			AudioEngine_Shutdown();
			return false;
		}
		sfx_channels[i].last_tick = ticks;
		sfx_channels[i].sfx_index = -1;
	}

	for (int i = 0; i < (AUDIO_ENGINE_MAX_SFX); i++) {
		//sfx[i].mapped_ch = snd_sfx_chn_alloc();
		//sfx[i].aica_buffer = snd_mem_malloc(AICA_MAX_SAMPLES / 2);        
		//assert(sfx[i].mapped_ch != -1);
        //assert(sfx[i].aica_buffer > 0);
		sfx[i].loop = 0;
		sfx[i].loop_offset = 0;
		sfx[i].has_offset = 0;
		sfx[i].stereo = 0;
		sfx[i].nSfx = -1;
		sfx[i].pan = AICA_PAN_CENTER;
		sfx[i].last_tick = ticks;
	}

	
	audio_worker_running.store(true);
	try
	{
	snd_thread = std::thread([]() {
		while( audio_worker_running.load() ) {
			{
				std::lock_guard<std::mutex> lk(channel_mtx);
				for (int i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
					 if (sfx_channels[i].sfx_index == -1)
						 continue;
					 
					 int sfx_idx = sfx_channels[i].sfx_index;
						uint16_t channel_pos = (g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_CHANNEL(sfx_channels[i].mapped_ch) + offsetof(aica_channel_t, pos)) & 0xffff);
					 
					 // Check if non-looping sound has finished
					 if (!sfx[sfx_idx].loop) {
						 // For non-looping sounds, check if position has wrapped (finished)
						 if (sfx[sfx_idx].has_offset && channel_pos == 0) {
							 // Sound finished - free the channel
							 aica_stop_chn(sfx_channels[i].mapped_ch);
							 sfx_channels[i].sfx_index = -1;
							 continue;
						 }
						 // Mark that we've started playing (to detect wrap-around)
						 if (channel_pos > 0 && !sfx[sfx_idx].has_offset) {
							 sfx[sfx_idx].has_offset = true;
						 }
					 }
					 // Handle looping sounds
					 else if (!sfx[sfx_idx].in_hnd_loop) {
						 verbosef("SFX %d pos: %d, %d", i, channel_pos, sfx[sfx_idx].total_samples);
						if(channel_pos) {
							 sfx[sfx_idx].has_offset = true;
						}
						 if (channel_pos >= sfx[sfx_idx].total_samples || (sfx[sfx_idx].has_offset && channel_pos == 0)) {
							 sfx[sfx_idx].in_hnd_loop = true;
							 debugf("Starting loop section: for sfx_%d_loop.wav\n", sfx[sfx_idx].nSfx);
							snd_sfx_stop(sfx_channels[i].mapped_ch);
							
							aica_play_chn(sfx_channels[i].mapped_ch, 
								 sfx[sfx_idx].total_samples - sfx[sfx_idx].loop_offset, 
								 sfx[sfx_idx].aica_buffer + (sfx[sfx_idx].loop_offset / 2), 
								 sfx[sfx_idx].type, 
								 sfx[sfx_idx].vol, 
								 sfx[sfx_idx].pan, 
								1,
								 sfx[sfx_idx].rate);
						}
					}
				}
			}

			for (int i = 0; i< AUDIO_ENGINE_MAX_STREAMS; i++) {
				int do_read = 0;
				{
					std::lock_guard<std::mutex> lk(channel_mtx);
					if (streams[i].playing) {
						stream_loop:
						// get channel pos
						uint32_t channel_pos = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_CHANNEL(streams[i].mapped_ch[0]) + offsetof(aica_channel_t, pos)) & 0xffff;
						uint32_t logical_pos = channel_pos;
						if (logical_pos > STREAM_CHANNEL_SAMPLE_COUNT/2) {
							logical_pos -= STREAM_CHANNEL_SAMPLE_COUNT/2;
						}
						//verbosef("Stream %d pos: %d, log: %d, rem: %d, total: %d\n", i, channel_pos, logical_pos, streams[i].played_samples, streams[i].total_samples);
			
						bool can_refill = (streams[i].played_samples + STREAM_CHANNEL_SAMPLE_COUNT/2) < streams[i].total_samples;
						bool can_fetch = (streams[i].played_samples + STREAM_CHANNEL_SAMPLE_COUNT/2 + STREAM_CHANNEL_SAMPLE_COUNT/2 + STREAM_CHANNEL_SAMPLE_COUNT/2) < streams[i].total_samples;
						// copy over data if needed from staging
						if (channel_pos >= STREAM_CHANNEL_SAMPLE_COUNT/2 && !streams[i].next_is_upper_half) {
							streams[i].next_is_upper_half = true;
							if (can_refill) { // could we need a refill?
								// fill lower half
								if (streams[i].stereo) {
									snd_adpcm_split((uint32_t*)streams[i].buffer, (uint32_t*)sfx_buffer, (uint32_t*)sfx_buffer + STREAM_STAGING_READ_SIZE_MONO, STREAM_STAGING_READ_SIZE_STEREO);
									spu_memload(streams[i].aica_buffers[0], (uint32_t*)sfx_buffer, STREAM_STAGING_READ_SIZE_STEREO/2);
									spu_memload(streams[i].aica_buffers[1], (uint32_t*)sfx_buffer + STREAM_STAGING_READ_SIZE_MONO, STREAM_STAGING_READ_SIZE_STEREO/2);
								}
								else {
									spu_memload(streams[i].aica_buffers[0], streams[i].buffer, STREAM_CHANNEL_BUFFER_SIZE/2);
								}
								// queue next read to staging if any
								if (can_fetch) {
									do_read = streams[i].stereo ? STREAM_STAGING_READ_SIZE_STEREO : STREAM_STAGING_READ_SIZE_MONO;
								}
							}
							assert(streams[i].first_refill == false);
							streams[i].played_samples += STREAM_CHANNEL_SAMPLE_COUNT/2;
						} else if (channel_pos < STREAM_CHANNEL_SAMPLE_COUNT/2 && streams[i].next_is_upper_half) {
							streams[i].next_is_upper_half = false;
							if (can_refill) { // could we need a refill?
								if (streams[i].stereo) {
									snd_adpcm_split((uint32_t*)streams[i].buffer, (uint32_t*)sfx_buffer, (uint32_t*)sfx_buffer + STREAM_STAGING_READ_SIZE_MONO, STREAM_STAGING_READ_SIZE_STEREO);
									spu_memload(streams[i].aica_buffers[0] + STREAM_CHANNEL_BUFFER_SIZE/2, (uint32_t*)sfx_buffer, STREAM_STAGING_READ_SIZE_STEREO/2);
									spu_memload(streams[i].aica_buffers[1] + STREAM_CHANNEL_BUFFER_SIZE/2, (uint32_t*)sfx_buffer + STREAM_STAGING_READ_SIZE_MONO, STREAM_STAGING_READ_SIZE_STEREO/2);
								}
								else {
									spu_memload(streams[i].aica_buffers[0] + STREAM_CHANNEL_BUFFER_SIZE/2, streams[i].buffer, STREAM_CHANNEL_BUFFER_SIZE/2);
								}
								// queue next read to staging, if any
								if (can_fetch) {
									do_read = streams[i].stereo ? STREAM_STAGING_READ_SIZE_STEREO : STREAM_STAGING_READ_SIZE_MONO;
								}
							}
							if (streams[i].first_refill) {
								streams[i].first_refill = false;
							} else {
								streams[i].played_samples += STREAM_CHANNEL_SAMPLE_COUNT/2;
							}
						}
						// if end of file, stop
						if ((streams[i].played_samples + logical_pos) > streams[i].total_samples) {
							if(!streams[i].loop) {
								// stop channel
								debugf("Auto stopping stream: %d -> {%d, %d}, %d total\n", i, streams[i].mapped_ch[0], streams[i].mapped_ch[1], streams[i].total_samples);
								aica_stop_chn(streams[i].mapped_ch[0]);
								aica_stop_chn(streams[i].mapped_ch[1]);
								streams[i].playing = false;
							}
							else {
								debugf("Auto looping stream: %d -> {%d, %d}, %d total\n", i, streams[i].mapped_ch[0], streams[i].mapped_ch[1], streams[i].total_samples);
								aica_stop_chn(streams[i].mapped_ch[0]);
								aica_stop_chn(streams[i].mapped_ch[1]);
							 if(streams[i].is_memory) {
								 streams[i].mem_offset = streams[i].loop_offset;
							 } else {
								fs_seek(streams[i].fd, streams[i].file_data_offset + streams[i].loop_offset, SEEK_SET);
							 }
								// played_samples counts from the start of the data
								// chunk, so a loop that restarts partway in must
								// resume the count there. Resetting it to zero
								// makes the end test overrun the file by the length
								// of the skipped intro on every repeat.
								streams[i].played_samples =
									streams[i].loop_offset * (streams[i].stereo ? 1 : 2);
								AudioEngine_LoadFirstChunk(i);
								streams[i].next_is_upper_half = true;
								streams[i].first_refill = true;
								// Restart the hardware decoder too: rewinding encoded
								// bytes without resetting its predictor corrupts ADPCM.
								aica_play_chn(streams[i].mapped_ch[0], STREAM_CHANNEL_SAMPLE_COUNT,
									streams[i].aica_buffers[0], streams[i].type, streams[i].vol,
									streams[i].pan[0], 1, streams[i].rate);
								if( streams[i].stereo )
								{
									aica_play_chn(streams[i].mapped_ch[1], STREAM_CHANNEL_SAMPLE_COUNT,
										streams[i].aica_buffers[1], streams[i].type, streams[i].vol,
										streams[i].pan[1], 1, streams[i].rate);
								}
								do_read = 0;
							}
						}
					}
					
					if (do_read) {
						 StreamRead(i, streams[i].buffer, do_read);
					}
				}
			}
			
			++ticks;

			thd_sleep(50);
		}
	});
	}
	catch( ... )
	{
		AudioEngine_Shutdown();
		return false;
	}
	return true;
}

int getSfxChannelIndex(int nStream) {
	for(int i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
		if(sfx_channels[i].sfx_index == nStream) {
			return i;
		}
	}

	return -1;
}

int AudioEngine_Stop(int nStream) {
	if(nStream < AUDIO_ENGINE_MAX_STREAMS) {
		debugf("Stopping Stream: %d\n", nStream);
		if(!streams[nStream].fd) {
			return 0;
		}

		aica_stop_chn(streams[nStream].mapped_ch[0]);
		aica_stop_chn(streams[nStream].mapped_ch[1]);
		streams[nStream].playing = false;
	}
	else {
		nStream -= AUDIO_ENGINE_MAX_STREAMS; // SFX OFFSET
		assert(nStream < AUDIO_ENGINE_MAX_SFX);

		int sfx_channel = getSfxChannelIndex(nStream);
		 if(sfx_channel < 0) {
			 return 0;
		 }

		if(sfx[nStream].nSfx == -1) {
			return 0;
		}

		aica_stop_chn(sfx_channels[sfx_channel].mapped_ch);
		 sfx_channels[sfx_channel].sfx_index = -1;
		 sfx[nStream].has_offset = false;
		 sfx[nStream].in_hnd_loop = false;
	}

	return nStream;
}

int AudioEngine_Unload(int nStream) {
	std::lock_guard<std::mutex> lock(channel_mtx);
	if( nStream < 0 || nStream >= AUDIO_ENGINE_MAX_STREAMS + AUDIO_ENGINE_MAX_SFX )
		return -1;
	if(nStream < AUDIO_ENGINE_MAX_STREAMS) {
		debugf("Stopping Stream: %d\n", nStream);

		aica_stop_chn(streams[nStream].mapped_ch[0]);
		aica_stop_chn(streams[nStream].mapped_ch[1]);
		streams[nStream].playing = false;
		 
		 if(streams[nStream].is_memory) {
			 // Memory stream - just clear it
			 streams[nStream].mem_data = nullptr;
			 streams[nStream].mem_size = 0;
			 streams[nStream].mem_offset = 0;
			 streams[nStream].is_memory = false;
		 } else if(streams[nStream].fd >= 0) {
		fs_close(streams[nStream].fd);
		streams[nStream].fd = -1;
		 }
	}
	else {
		nStream -= AUDIO_ENGINE_MAX_STREAMS; // SFX OFFSET
		assert(nStream < AUDIO_ENGINE_MAX_SFX);

		if(sfx[nStream].nSfx == -1) {
			return 0;
		}

		// A cached sound need not currently have a playing channel.
		for( int i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; ++i )
		{
			if( sfx_channels[i].sfx_index == nStream )
			{
				aica_stop_chn(sfx_channels[i].mapped_ch);
				sfx_channels[i].sfx_index = -1;
			}
		}

		snd_mem_free(sfx[nStream].aica_buffer);  // Release SRAM buffer
		sfx[nStream].aica_buffer = 0;
		sfx[nStream].nSfx = -1; // Release SFX
	}

	return nStream;
}

int AudioEngine_GetOpenStreamIndex(const char * fname, file_t fd) {
	// 1. Check for Duplicate File Name
	size_t fileNameLength = std::min(strlen(fname), (size_t)128);
    for(int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; i++) {
        if (strncmp(fname, streams[i].fname, fileNameLength) == 0){
			fs_close(streams[i].fd);
			debugf("AudioEngine: File Already In Stream Cache! %i, %s, %s\n", i, fname, streams[i].fname);
            return i;
        }
    }
	// 2. Check for Free Index
    for(int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; i++) {
        if(streams[i].fd == -1) {
            return i;
        }
    }
	// 3. Force Close The Stream Which Was Accessed The Least Recently
	uint32_t last_tick = streams[0].last_tick;
	int index = 0;
    for(int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; i++) {
        if(streams[i].last_tick < last_tick) {
            index = i;
			last_tick = streams[i].last_tick;
        }
    }

	return AudioEngine_Unload(index);
}

int AudioEngine_GetOpenSFXIndex(const char * fname) {
	// 1. Check for Duplicate File Name
    for(int i = 0; i < AUDIO_ENGINE_MAX_SFX; i++) {
        if (sfx[i].nSfx != -1 && strcmp(fname, sfx[i].fname) == 0){
			debugf("AudioEngine: File Already In SFX Cache! %i, %s, %s\n", i, fname, sfx[i].fname);
            return i;
        }
    }
	// 2. Check for Free Index
    for(int i = 0; i < AUDIO_ENGINE_MAX_SFX; i++) {
        if(sfx[i].nSfx == -1) {
            return i;
        }
    }
	// USound owns this slot until UnregisterSound. Reusing it here leaves
	// the original sound with a stale handle to an unrelated sample.
	debugf("AudioEngine: all registered SFX slots are occupied\n");
	return -1;
}

bool AudioEngine_ParseWaveHeader(file_t fd, WavHeader * hdr) {
	uint8_t header_buffer[MAX_WAVE_HEADER_SIZE];
	uint8_t valid_header = 0;
    // Read the RIFF header.
	fs_seek(fd, 0, SEEK_SET);

	fs_read(fd, header_buffer, MAX_WAVE_HEADER_SIZE);

	if(!(header_buffer[0] == 'R' && header_buffer[1] == 'I' && header_buffer[2] == 'F' && header_buffer[3] == 'F')) {
		return 0;
	}

	for(int i = 0; i < MAX_WAVE_HEADER_SIZE - (sizeof(WaveFmtHeader)); i++) {
		if(header_buffer[i] == 'f' && header_buffer[i + 1] == 'm' && header_buffer[i + 2] == 't' && header_buffer[i + 3] == ' ') {
			memcpy(&hdr->fmtHeader, &header_buffer[i], sizeof(WaveFmtHeader));
			++valid_header;
			break;
		}
	}

	for(int i = 0; i < MAX_WAVE_HEADER_SIZE - (sizeof(WaveChunkHeader)); i++) {
		if(header_buffer[i] == 'd' && header_buffer[i + 1] == 'a' && header_buffer[i + 2] == 't' && header_buffer[i + 3] == 'a') {
			memcpy(&hdr->chunkHeader, &header_buffer[i], sizeof(WaveChunkHeader));
			// Leave the handle on the payload, not on the chunk id and length.
			fs_seek(fd, i + sizeof(WaveChunkHeader), SEEK_SET);
			++valid_header;
			break;
		}
	}	

    return valid_header == 2;
}

int AudioEngine_Load(const char * fname, uint32_t seek_bytes_aligned)
{
	file_t f = fs_open(fname, O_RDONLY);
	if( f < 0 ) {
		// Callers such as the music subsystem probe for optional cooked tracks,
		// so a missing or malformed file is reported rather than fatal.
		debugf("AudioEngine_Load: cannot open %s\n", fname);
		return -1;
	}
	WavHeader hdr;
    bool validHeader = AudioEngine_ParseWaveHeader(f, &hdr);
	if( !validHeader ) {
		debugf("AudioEngine_Load: %s is not a supported wave\n", fname);
		fs_close(f);
		return -1;
	}

	debugf("AudioEngine_Load: %s, %i\n",fname, hdr.chunkHeader.size);

    int total_samples = (int)((float)hdr.chunkHeader.size  / (((float)(hdr.fmtHeader.bitsPerSample) / 8) * (float)hdr.fmtHeader.numChannels));

    if(total_samples > AICA_MAX_SAMPLES)
	{
        int nStream = AudioEngine_GetOpenStreamIndex(fname, f);
        if(nStream < 0) {
            debugf("PreloadStreamedFile Error: No Available Streams!\n");
            fs_close(f);
            return nStream;
        }

        assert( nStream < AUDIO_ENGINE_MAX_STREAMS );

	    debugf("PreloadStreamedFile(%p, %d) is %s\n", f, nStream, fname);

		std::lock_guard<std::mutex> lk(channel_mtx);

		strncpy(streams[nStream].fname, fname, 127);
    	streams[nStream].fname[127] = '\0';

		// Stop if playing
		// Keep in sync with StopStreamedFile
		if (streams[nStream].playing) {
			streams[nStream].playing = false;
			aica_stop_chn(streams[nStream].mapped_ch[0]);
			aica_stop_chn(streams[nStream].mapped_ch[1]);
		}

		if (streams[nStream].fd >= 0) {
            // close prevoius handle
			fs_close(streams[nStream].fd);
		}
		streams[nStream].fd = -1;

		streams[nStream].rate = hdr.fmtHeader.sampleRate;
		streams[nStream].stereo = hdr.fmtHeader.numChannels == 2;
		streams[nStream].fd = f;
		streams[nStream].playing = false;
		streams[nStream].total_samples = total_samples;
		streams[nStream].played_samples = 0;
		streams[nStream].next_is_upper_half = true;
		streams[nStream].first_refill = true;
        streams[nStream].type = hdr.fmtHeader.bitsPerSample == 16 ? AICA_SM_16BIT : hdr.fmtHeader.bitsPerSample == 8  ? AICA_SM_8BIT : AICA_SM_ADPCM_LS;
		streams[nStream].last_tick = ticks;

		debugf("Preload Streamed File: %s: stream: %d, freq: %d, chans: %d, byte size: %d, played samples: %d, total samples: %d, blockAlign: %d\n", 
            fname, nStream, hdr.fmtHeader.sampleRate, hdr.fmtHeader.numChannels, hdr.chunkHeader.size, streams[nStream].played_samples, streams[nStream].total_samples, hdr.fmtHeader.blockAlign);

		// The parser leaves the handle on the payload. Record that before any
		// seek: looping rewinds to file_data_offset, so it must stay the start
		// of the data chunk rather than wherever playback happened to begin.
		streams[nStream].file_data_offset = fs_tell(f);

		// How to avoid the lock?
		if (seek_bytes_aligned) {
			streams[nStream].played_samples = seek_bytes_aligned * (streams[nStream].stereo ? 1 : 2);
			debugf("Seeking aligned to: %d, played_samples: %d\n", seek_bytes_aligned, streams[nStream].played_samples);
			fs_seek(streams[nStream].fd, streams[nStream].file_data_offset + seek_bytes_aligned, SEEK_SET);
		}

		// Stage to memory
		AudioEngine_LoadFirstChunk(nStream);

        verbosef("PreloadStreamedFile: %p - %s, %d, %d, %d\n", f, fname, streams[nStream].rate, streams[nStream].stereo, streams[nStream].type);

        return nStream;
	}
    else {
        int nStream = AudioEngine_GetOpenSFXIndex(fname);
        if(nStream < 0) {
            debugf("PreloadStreamedFile Error: No Available Streams!\n");
            fs_close(f);
            return nStream;
        }

        assert( nStream < AUDIO_ENGINE_MAX_SFX );

	    debugf("Preload SFX(%p, %d) is %s\n", f, nStream, fname);

		std::lock_guard<std::mutex> lk(channel_mtx);


		strncpy(sfx[nStream].fname, fname, 127);
    	sfx[nStream].fname[127] = '\0';

		sfx[nStream].rate = hdr.fmtHeader.sampleRate;
		sfx[nStream].stereo = hdr.fmtHeader.numChannels == 2;
		sfx[nStream].nSfx = nStream;
        sfx[nStream].type = hdr.fmtHeader.bitsPerSample == 16 ? AICA_SM_16BIT : hdr.fmtHeader.bitsPerSample == 8  ? AICA_SM_8BIT : AICA_SM_ADPCM_LS;
        sfx[nStream].total_samples = total_samples;
		sfx[nStream].last_tick = ticks;

		debugf("Preload SFX: %s: stream: %d, freq: %d, chans: %d, byte size: %d, total samples: %d\n", 
            fname, nStream, hdr.fmtHeader.sampleRate, hdr.fmtHeader.numChannels, hdr.chunkHeader.size, sfx[nStream].total_samples);

		// Stage to memory
        uint32_t sfx_size = fs_total(f) - fs_tell(f);
		sfx[nStream].aica_buffer = snd_mem_malloc(sfx_size); 
		assert(sfx[nStream].aica_buffer > 0);
		fs_read(f, sfx_buffer, sfx_size);
		spu_memload_sq(sfx[nStream].aica_buffer, sfx_buffer, sfx_size);
		if (sfx[nStream].stereo) {
			//spu_memload_sq(sfx[nStream].aica_buffers[1], sfx[nStream].buffer + STREAM_STAGING_READ_SIZE_MONO, STREAM_CHANNEL_BUFFER_SIZE/2);
		}

        fs_close(f);

        verbosef("Preload SFX File: %p - %s, %d, %d, %d\n", f, fname, sfx[nStream].rate, sfx[nStream].stereo, sfx[nStream].type);

        return nStream + AUDIO_ENGINE_MAX_STREAMS; // Offset for SFX
    }    
}

int AudioEngine_LoadFromWaveInfo(
	const uint8_t* sample_data,
	uint32_t sample_size,
	int sample_rate,
	int channels,
	int bits_per_sample,
	const char* name,
	const uint8_t* full_wav_data,
	uint32_t full_wav_size,
	uint32_t sample_count )
{
	if(!sample_data || sample_size == 0) {
		return -1;
	}

	// Calculate total samples based on format
	// ADPCM (4-bit): 2 samples per byte
	// PCM 8-bit: 1 sample per byte
	// PCM 16-bit: 1 sample per 2 bytes
	int total_samples;
	if( bits_per_sample == 4 )
	{
		// ADPCM: 4 bits per sample = 2 samples per byte
		total_samples = sample_size * 2;
	}
	else if( bits_per_sample == 8 )
	{
		// PCM 8-bit: 1 byte per sample
		total_samples = sample_size;
	}
	else if( bits_per_sample == 16 )
	{
		// PCM 16-bit: 2 bytes per sample
		total_samples = sample_size / 2;
	}
	else
	{
		// Fallback calculation
		total_samples = (int)((float)sample_size / (((float)bits_per_sample / 8) * (float)channels));
	}

	if( channels < 1 || channels > 2 )
	{
		return -1;
	}
	total_samples /= channels;
	if( sample_count )
	{
		if( sample_count > (uint32_t)total_samples )
		{
			return -1;
		}
		total_samples = sample_count;
	}

	// Handle large sounds with in-memory streaming
    if(total_samples > AICA_MAX_SAMPLES)
	{
		if(!full_wav_data || full_wav_size == 0) {
			debugf("AudioEngine_LoadFromWaveInfo: Sound %s too large for SFX (%d samples > %d), full WAV data required for streaming\n", 
			name, total_samples, AICA_MAX_SAMPLES);
		return -1;
		}

		// Set up in-memory streaming
		std::lock_guard<std::mutex> lk(channel_mtx);
		
		// Find an available stream slot
		int nStream = -1;
		for(int i = 0; i < AUDIO_ENGINE_MAX_STREAMS; i++) {
			if(streams[i].fd == -1 && (!streams[i].is_memory || streams[i].mem_data == nullptr)) {
				nStream = i;
				break;
			}
		}
		if(nStream < 0) {
			debugf("AudioEngine: all registered memory-stream slots are occupied\n");
			return -1;
		}

		// Use the already validated data span, not a bytewise RIFF tag search.
		if( sample_data < full_wav_data || sample_data - full_wav_data > full_wav_size
			|| sample_size > full_wav_size - (sample_data - full_wav_data) )
		{
			return -1;
		}

		// Set up memory stream
		strncpy(streams[nStream].fname, name, 127);
		streams[nStream].fname[127] = '\0';
		streams[nStream].is_memory = true;
		streams[nStream].mem_data = sample_data;
		streams[nStream].mem_size = sample_size;
		streams[nStream].mem_offset = 0;
		streams[nStream].fd = -1;
		streams[nStream].rate = sample_rate;
		streams[nStream].stereo = channels == 2;
		streams[nStream].playing = false;
		streams[nStream].total_samples = total_samples;
		streams[nStream].played_samples = 0;
		streams[nStream].next_is_upper_half = true;
		streams[nStream].first_refill = true;
		streams[nStream].type = bits_per_sample == 16 ? AICA_SM_16BIT : bits_per_sample == 8 ? AICA_SM_8BIT : AICA_SM_ADPCM_LS;
		streams[nStream].file_data_offset = 0; // Not used for memory streams
		streams[nStream].last_tick = ticks;

		debugf("Load large sound as memory stream: %s: stream: %d, freq: %d, chans: %d, total samples: %d\n", 
			name, nStream, sample_rate, channels, total_samples);

		// Load first chunk
		AudioEngine_LoadFirstChunk(nStream);

		return nStream; // Return stream index (not offset)
	}

    int nStream = AudioEngine_GetOpenSFXIndex(name);
    if(nStream < 0) {
        debugf("AudioEngine_LoadFromWaveInfo Error: No Available SFX Slots!\n");
        return -1;
    }

    assert( nStream < AUDIO_ENGINE_MAX_SFX );

	std::lock_guard<std::mutex> lk(channel_mtx);

	// If this slot already has a buffer allocated (reused slot from LRU eviction), free it first
	if(sfx[nStream].nSfx == nStream && sfx[nStream].aica_buffer > 0) {
		// This slot is currently in use with a valid buffer - free it before reusing
		// Stop any playing instance
		int sfx_channel = getSfxChannelIndex(nStream);
		if(sfx_channel >= 0) {
			aica_stop_chn(sfx_channels[sfx_channel].mapped_ch);
			sfx_channels[sfx_channel].sfx_index = -1;
		}
		// Free the old buffer
		snd_mem_free(sfx[nStream].aica_buffer);
		sfx[nStream].aica_buffer = 0;
	}

	strncpy(sfx[nStream].fname, name, 127);
    sfx[nStream].fname[127] = '\0';

	sfx[nStream].rate = sample_rate;
	sfx[nStream].stereo = channels == 2;
	sfx[nStream].nSfx = nStream;
    
	// Determine format - check for ADPCM first (4-bit), then PCM formats
	// ADPCM is pre-encoded in UnAudio.cpp, AudioEngine just needs to use it
	if( bits_per_sample == 4 )
	{
		// 4-bit ADPCM format (pre-encoded in UnAudio.cpp)
		sfx[nStream].type = AICA_SM_ADPCM_LS;
	}
	else if( bits_per_sample == 8 )
	{
		sfx[nStream].type = AICA_SM_8BIT;
	}
	else if( bits_per_sample == 16 )
	{
		sfx[nStream].type = AICA_SM_16BIT;
	}
	else
	{
		// Default to 8-bit PCM instead of ADPCM for unknown bit depths
		sfx[nStream].type = AICA_SM_8BIT;
	}
	
    sfx[nStream].total_samples = total_samples;
	sfx[nStream].last_tick = ticks;

	// Allocate AICA buffer and load sample data
	// Check if sample_size is valid and not too large
	if(sample_size == 0 || sample_size > (AICA_MAX_SAMPLES * 2)) {
		debugf("AudioEngine_LoadFromWaveInfo: Invalid sample_size=%d for %s\n", sample_size, name);
		sfx[nStream].nSfx = -1; // Mark as free
		return -1;
	}
	
	uint32_t upload_size = (sample_size + 31u) & ~31u;
	sfx[nStream].aica_buffer = snd_mem_malloc(upload_size);
	if(sfx[nStream].aica_buffer == 0) {
		debugf("AudioEngine_LoadFromWaveInfo: Failed to allocate AICA buffer for %s (size=%d) - out of sound RAM!\n", name, sample_size);
		sfx[nStream].nSfx = -1; // Mark as free
		return -1;
	}
	
	// Copy sample data directly from memory
	// Ensure we don't overflow sfx_buffer
	if(upload_size > sizeof(sfx_buffer)) {
		debugf("AudioEngine_LoadFromWaveInfo: Sample size %d exceeds buffer size %d for %s\n", sample_size, sizeof(sfx_buffer), name);
		snd_mem_free(sfx[nStream].aica_buffer);
		sfx[nStream].aica_buffer = 0;
		sfx[nStream].nSfx = -1;
		return -1;
	}
	
	// Copy sample data - format-specific handling
	if( sfx[nStream].type == AICA_SM_ADPCM_LS )
	{
		// ADPCM data is already encoded, just copy it
		memcpy(sfx_buffer, sample_data, sample_size);
	}
	else if( sfx[nStream].type == AICA_SM_8BIT )
	{
		// Convert unsigned 8-bit WAV samples (0-255) to signed 8-bit (-128 to 127)
		// WAV 8-bit PCM is unsigned, but AICA expects signed
		uint8_t* src = (uint8_t*)sample_data;
		int8_t* dst = (int8_t*)sfx_buffer;
		for(uint32_t i = 0; i < sample_size; i++)
		{
			dst[i] = (int8_t)((int)src[i] - 128);  // Convert 0-255 to -128 to 127
		}
	}
	else
	{
		// 16-bit samples are already signed, just copy
		memcpy(sfx_buffer, sample_data, sample_size);
	}
	
	memset(sfx_buffer + sample_size, 0, upload_size - sample_size);
	spu_memload_sq(sfx[nStream].aica_buffer, sfx_buffer, upload_size);

    return nStream + AUDIO_ENGINE_MAX_STREAMS; // Offset for SFX
}

int getOpenSfxChannel() {
	for(int i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
		if(sfx_channels[i].sfx_index < 0) {
			return i;
		}
	}

	 // All channels in use - find LRU and free it
	uint32_t last_tick = sfx_channels[0].last_tick;
	int index = 0;
    for(int i = 0; i < AUDIO_ENGINE_MAX_CHANNELS; i++) {
        if(sfx_channels[i].last_tick < last_tick) {
            index = i;
			last_tick = sfx_channels[i].last_tick;
        }
    }

	 // Stop the sound on this channel and free it
	 if(sfx_channels[index].sfx_index >= 0) {
		 debugf("getOpenSfxChannel: Freeing LRU channel %d (SFX %d)\n", index, sfx_channels[index].sfx_index);
		 aica_stop_chn(sfx_channels[index].mapped_ch);
		 sfx_channels[index].sfx_index = -1;
	 }
 
	 return index;
}

void AudioEngine_Play(
	int nStream,
	uint8_t volume,
	uint8_t panl,
	uint8_t panr,
	bool loop,
	uint32_t loop_offset,
	uint32_t loop_end )
{
    if(nStream < AUDIO_ENGINE_MAX_STREAMS) {
        std::lock_guard<std::mutex> lk(channel_mtx);
        if (streams[nStream].playing) {
            return;
        }

        streams[nStream].vol = volume;
		streams[nStream].pan[0] = (streams[nStream].stereo) ? panl : ((panl + panr) >> 1);
		streams[nStream].pan[1] = panr;

		streams[nStream].loop = loop;
		if( streams[nStream].is_memory && loop && loop_offset )
		{
			debugf("AudioEngine: nonzero long ADPCM loop start is unsupported\n");
			return;
		}
		streams[nStream].loop_offset = loop_offset;
		if( streams[nStream].is_memory )
		{
			streams[nStream].mem_offset = 0;
			streams[nStream].played_samples = 0;
			streams[nStream].next_is_upper_half = true;
			streams[nStream].first_refill = true;
			AudioEngine_LoadFirstChunk(nStream);
		}


		streams[nStream].last_tick = ticks;

        debugf("StartPreloadedStreamedFile(%d) - actually starting stream\n", nStream);

        aica_play_chn(
            streams[nStream].mapped_ch[0],
            STREAM_CHANNEL_SAMPLE_COUNT,
            streams[nStream].aica_buffers[0],
            streams[nStream].type,
            streams[nStream].vol,
            streams[nStream].pan[0],
            1,
            streams[nStream].rate
        );
		if(streams[nStream].stereo) {
			aica_play_chn(
				streams[nStream].mapped_ch[1],
				STREAM_CHANNEL_SAMPLE_COUNT,
				streams[nStream].aica_buffers[1],
				streams[nStream].type,
				streams[nStream].vol,
				streams[nStream].pan[1],
				1,
				streams[nStream].rate
			);
		}

        streams[nStream].playing = true;  
    }
    else {
        nStream -= AUDIO_ENGINE_MAX_STREAMS; // SFX Offset

        assert( nStream < AUDIO_ENGINE_MAX_SFX );
        std::lock_guard<std::mutex> lk(channel_mtx);

        sfx[nStream].vol = volume;
		sfx[nStream].last_tick = ticks;
		sfx[nStream].loop = loop;
		sfx[nStream].loop_offset = loop_offset;
		sfx[nStream].pan = (panl + panr) >> 1;
		 sfx[nStream].has_offset = false; // Reset for new playback
		 sfx[nStream].in_hnd_loop = loop; // Hardware owns short-sample looping.

		 // Check if this SFX is already playing on a channel
		 int sfx_channel = getSfxChannelIndex(nStream);
		 if(sfx_channel >= 0) {
			 aica_stop_chn(sfx_channels[sfx_channel].mapped_ch);
		 } else {
			 // Get a new channel
			 sfx_channel = getOpenSfxChannel();
			 if(sfx_channel < 0) {
				 debugf("AudioEngine_Play: No available SFX channels!\n");
				 return;
			 }
			 sfx_channels[sfx_channel].sfx_index = nStream;
		 }

		sfx_channels[sfx_channel].last_tick = ticks;

        aica_play_chn(
            sfx_channels[sfx_channel].mapped_ch,
            loop && loop_end ? loop_end : sfx[nStream].total_samples,
            sfx[nStream].aica_buffer,
            sfx[nStream].type == AICA_SM_ADPCM_LS ? AICA_SM_ADPCM : sfx[nStream].type,
            sfx[nStream].vol,
            sfx[nStream].pan, // PAN
            loop,
            sfx[nStream].rate,
            loop_offset
        );    
    }
}

struct sfx_info * AudioEngine_getSfxInfo(int nStream) {
	assert(nStream < (AUDIO_ENGINE_MAX_STREAMS + AUDIO_ENGINE_MAX_SFX));

	nStream -= AUDIO_ENGINE_MAX_STREAMS;

	return &sfx[nStream];
}

struct stream_info * AudioEngine_getStreamInfo(int nStream) {
	assert(nStream < (AUDIO_ENGINE_MAX_STREAMS));

	return &streams[nStream];
}

 int AudioEngine_GetSfxChannel(int nStream) {
	 if(nStream < AUDIO_ENGINE_MAX_STREAMS)
		 return -1; // Not an SFX
	 
	 nStream -= AUDIO_ENGINE_MAX_STREAMS;
	 int sfx_channel = getSfxChannelIndex(nStream);
	 if(sfx_channel < 0)
		 return -1;
	 
	 return sfx_channels[sfx_channel].mapped_ch;
 }

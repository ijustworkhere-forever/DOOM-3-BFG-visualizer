/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

===========================================================================
*/
#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../snd_local.h"
#include "XA2_Decoder.h"
#include <vector>
#include <cstdio>
#include <cstdarg>

// TEMPORARY diagnostic instrumentation for tracking down why real-world
// wav/mp3 files fail to load -- console/qconsole.log output isn't
// reliably observable in this environment, so this writes straight to an
// absolute path via raw CRT file I/O, same technique already proven for
// the earlier s_mod crash investigation.
static void VisAudioDiagLog( const char * fmt, ... ) {
    FILE * f = fopen( "C:\\DOOM-3-BFG\\audio_diag.log", "a" );
    if ( f == NULL ) {
        return;
    }
    va_list args;
    va_start( args, fmt );
    vfprintf( f, fmt, args );
    va_end( args );
    fflush( f );
    fclose( f );
}
#if defined(__has_include)
#if __has_include(<libavcodec/avcodec.h>)
#include "XA2_FFmpegDecoder.h"
#define ID_HAVE_FFMPEG 1
#endif
#endif
#ifndef ID_HAVE_FFMPEG
#define ID_HAVE_FFMPEG 0
#endif

extern idCVar s_useCompression;
extern idCVar s_noSound;

#define GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( x ) x

const uint32 SOUND_MAGIC_IDMSA = 0x6D7A7274;

extern idCVar sys_lang;

/*
========================
AllocBuffer
========================
*/
static void * AllocBuffer( int size, const char * name ) {
	return Mem_Alloc( size, TAG_AUDIO );
}

/*
========================
FreeBuffer
========================
*/
static void FreeBuffer( void * p ) {
	return Mem_Free( p );
}

/*
========================
idSoundSample_XAudio2::idSoundSample_XAudio2
========================
*/
idSoundSample_XAudio2::idSoundSample_XAudio2() {
	timestamp = FILE_NOT_FOUND_TIMESTAMP;
	loaded = false;
	neverPurge = false;
	levelLoadReferenced = false;

	memset( &format, 0, sizeof( format ) );

	totalBufferSize = 0;

	playBegin = 0;
	playLength = 0;

	lastPlayedTime = 0;
	isStreaming = false;
	m_decoder = NULL;
}

/*
========================
idSoundSample_XAudio2::~idSoundSample_XAudio2
========================
*/
idSoundSample_XAudio2::~idSoundSample_XAudio2() {
	FreeData();
}

/*
========================
idSoundSample_XAudio2::WriteGeneratedSample
========================
*/
void idSoundSample_XAudio2::WriteGeneratedSample( idFile *fileOut ) {
	fileOut->WriteBig( SOUND_MAGIC_IDMSA );
	fileOut->WriteBig( timestamp );
	fileOut->WriteBig( loaded );
	fileOut->WriteBig( playBegin );
	fileOut->WriteBig( playLength );
	idWaveFile::WriteWaveFormatDirect( format, fileOut );
	fileOut->WriteBig( ( int )amplitude.Num() );
	fileOut->Write( amplitude.Ptr(), amplitude.Num() );
	fileOut->WriteBig( totalBufferSize );
	fileOut->WriteBig( ( int )buffers.Num() );
	for ( int i = 0; i < buffers.Num(); i++ ) {
		fileOut->WriteBig( buffers[ i ].numSamples );
		fileOut->WriteBig( buffers[ i ].bufferSize );
		fileOut->Write( buffers[ i ].buffer, buffers[ i ].bufferSize );
	};
}
/*
========================
idSoundSample_XAudio2::WriteAllSamples
========================
*/
void idSoundSample_XAudio2::WriteAllSamples( const idStr &sampleName ) {
	idSoundSample_XAudio2 * samplePC = new idSoundSample_XAudio2();
	{
		idStrStatic< MAX_OSPATH > inName = sampleName;
		inName.Append( ".msadpcm" );
		idStrStatic< MAX_OSPATH > inName2 = sampleName;
		inName2.Append( ".wav" );

		idStrStatic< MAX_OSPATH > outName = "generated/";
		outName.Append( sampleName );
		outName.Append( ".idwav" );

		if ( samplePC->LoadWav( inName ) || samplePC->LoadWav( inName2 ) ) {
			idFile *fileOut = fileSystem->OpenFileWrite( outName, "fs_basepath" );
			samplePC->WriteGeneratedSample( fileOut );
			delete fileOut;
		}
	}
	delete samplePC;
}

/*
========================
idSoundSample_XAudio2::LoadGeneratedSound
========================
*/
bool idSoundSample_XAudio2::LoadGeneratedSample( const idStr &filename ) {
	idFileLocal fileIn( fileSystem->OpenFileReadMemory( filename ) );
	if ( fileIn != NULL ) {
		uint32 magic;
		fileIn->ReadBig( magic );
		fileIn->ReadBig( timestamp );
		fileIn->ReadBig( loaded );
		fileIn->ReadBig( playBegin );
		fileIn->ReadBig( playLength );
		idWaveFile::ReadWaveFormatDirect( format, fileIn );
		int num;
		fileIn->ReadBig( num );
		if ( m_decoder ) { m_decoder->Close(); delete m_decoder; m_decoder = NULL; }
		amplitude.Clear();
		amplitude.SetNum( num );
		fileIn->Read( amplitude.Ptr(), amplitude.Num() );
		fileIn->ReadBig( totalBufferSize );
		fileIn->ReadBig( num );
		buffers.SetNum( num );
		for ( int i = 0; i < num; i++ ) {
			fileIn->ReadBig( buffers[ i ].numSamples );
			fileIn->ReadBig( buffers[ i ].bufferSize );
			buffers[ i ].buffer = AllocBuffer( buffers[ i ].bufferSize, GetName() );
			fileIn->Read( buffers[ i ].buffer, buffers[ i ].bufferSize );
			buffers[ i ].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[ i ].buffer );
		}
		return true;
	}
	return false;
}
/*
========================
idSoundSample_XAudio2::Load
========================
*/
void idSoundSample_XAudio2::LoadResource() {
	VisAudioDiagLog( "LoadResource: name='%s'\n", GetName() );
	FreeData();

	if ( idStr::Icmpn( GetName(), "_default", 8 ) == 0 ) {
		MakeDefault();
		return;
	}

	if ( s_noSound.GetBool() ) {
		MakeDefault();
		return;
	}

	loaded = false;

	for ( int i = 0; i < 2; i++ ) {
		idStrStatic< MAX_OSPATH > sampleName = GetName();
		if ( ( i == 0 ) && !sampleName.Replace( "/vo/", va( "/vo/%s/", sys_lang.GetString() ) ) ) {
			i++;
		}

		idStrStatic< MAX_OSPATH > generatedName = "generated/";
		generatedName.Append( sampleName );
		generatedName.Append( ".idwav" );

		// If the name still carries a media extension, decode it directly.
		if ( sampleName.Find( ".ogg", false ) != -1 || sampleName.Find( ".mp3", false ) != -1 ||
			 sampleName.Find( ".flac", false ) != -1 || sampleName.Find( ".m4a", false ) != -1 ||
			 sampleName.Find( ".aac", false ) != -1 || sampleName.Find( ".opus", false ) != -1 ||
			 sampleName.Find( ".wma", false ) != -1 ) {
			loaded = LoadMedia( sampleName );
		}
		if ( !loaded ) {
			loaded = LoadGeneratedSample( generatedName );
		}
		// VIS: always fall back to the loose .wav - with s_useCompression 1 the
		// stock code only tried .msadpcm and defaulted (beeped) on plain wav files
		if ( !loaded && s_useCompression.GetBool() ) {
			idStrStatic< MAX_OSPATH > compressedName = sampleName;
			compressedName.Append( ".msadpcm" );
			loaded = LoadWav( compressedName );
		}
		if ( !loaded ) {
			idStrStatic< MAX_OSPATH > wavName = sampleName;
			wavName.Append( ".wav" );
			VisAudioDiagLog( "  probing LoadWav('%s')\n", wavName.c_str() );
			loaded = LoadWav( wavName );
			VisAudioDiagLog( "  LoadWav('%s') -> %s\n", wavName.c_str(), loaded ? "OK" : "failed" );
		}
		// VIS: the decl system strips the file extension from the sample name, so a
		// name like "music/song" (from song.mp3) reaches here with no extension.
		// Probe the media extensions - LoadMedia just returns false if the file
		// isn't there - so mp3/flac/etc. decode through FFmpeg.
		if ( !loaded ) {
			// .wav last: catches non-16-bit-PCM wavs that idWaveFile rejects but
			// FFmpeg can decode (24-bit, float, etc.).
			static const char * const mediaExts[] = { ".ogg", ".mp3", ".flac", ".m4a", ".aac", ".opus", ".wma", ".wav" };
			for ( int m = 0; m < 8 && !loaded; m++ ) {
				idStrStatic< MAX_OSPATH > mediaName = sampleName;
				mediaName.Append( mediaExts[m] );
				VisAudioDiagLog( "  probing LoadMedia('%s')\n", mediaName.c_str() );
				loaded = LoadMedia( mediaName );
				VisAudioDiagLog( "  LoadMedia('%s') -> %s\n", mediaName.c_str(), loaded ? "OK" : "failed" );
			}
		}

		VisAudioDiagLog( "LoadResource: sampleName='%s' loaded=%s\n", sampleName.c_str(), loaded ? "true" : "false" );

		if ( loaded ) {
			fileSystem->AddSamplePreload( GetName() );
			WriteAllSamples( GetName() );
			if ( sampleName.Find( "/vo/" ) >= 0 ) {
				for ( int langIndex = 0; langIndex < Sys_NumLangs(); langIndex++ ) {
					const char *lang = Sys_Lang( langIndex );
					if ( idStr::Icmp( lang, ID_LANG_ENGLISH ) == 0 ) {
						continue;
					}
					idStrStatic< MAX_OSPATH > locName = GetName();
					locName.Replace( "/vo/", va( "/vo/%s/", lang ) );
					WriteAllSamples( locName );
				}
			}
			return;
		}
	}

	// make it default if everything else fails
	MakeDefault();
}

	/*
	========================
	idSoundSample_XAudio2::LoadWav
	========================
	*/
	bool idSoundSample_XAudio2::LoadWav( const idStr & filename ) {

		// load the wave
		idWaveFile wave;
		if ( !wave.Open( filename ) ) {
			return false;
		}

		idStrStatic< MAX_OSPATH > sampleName = filename;
		sampleName.SetFileExtension( "amp" );
		LoadAmplitude( sampleName );

		const char * formatError = wave.ReadWaveFormat( format );
		if ( formatError != NULL ) {
			idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), formatError );
			MakeDefault();
			return false;
		}
		timestamp = wave.Timestamp();

		totalBufferSize = wave.SeekToChunk( 'data' );

		if ( format.basic.formatTag == idWaveFile::FORMAT_PCM || format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE ) {

			if ( format.basic.bitsPerSample != 16 ) {
				idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), "Not a 16 bit PCM wav file" );
				MakeDefault();
				return false;
			}

			playBegin = 0;
			playLength = ( totalBufferSize ) / format.basic.blockSize;

			buffers.SetNum( 1 );
			buffers[0].bufferSize = totalBufferSize;
			buffers[0].numSamples = playLength;
			buffers[0].buffer = AllocBuffer( totalBufferSize, GetName() );


			wave.Read( buffers[0].buffer, totalBufferSize );

			if ( format.basic.bitsPerSample == 16 ) {
				idSwap::LittleArray( (short *)buffers[0].buffer, totalBufferSize / sizeof( short ) );
			}

			buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );

		} else if ( format.basic.formatTag == idWaveFile::FORMAT_ADPCM ) {

			playBegin = 0;
			playLength = ( ( totalBufferSize / format.basic.blockSize ) * format.extra.adpcm.samplesPerBlock );

			buffers.SetNum( 1 );
			buffers[0].bufferSize = totalBufferSize;
			buffers[0].numSamples = playLength;
			buffers[0].buffer  = AllocBuffer( totalBufferSize, GetName() );

			wave.Read( buffers[0].buffer, totalBufferSize );

			buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );

		} else if ( format.basic.formatTag == idWaveFile::FORMAT_XMA2 ) {

			if ( format.extra.xma2.blockCount == 0 ) {
				idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), "No data blocks in file" );
				MakeDefault();
				return false;
			}

			int bytesPerBlock = format.extra.xma2.bytesPerBlock;
			assert( format.extra.xma2.blockCount == ALIGN( totalBufferSize, bytesPerBlock ) / bytesPerBlock );
			assert( format.extra.xma2.blockCount * bytesPerBlock >= totalBufferSize );
			assert( format.extra.xma2.blockCount * bytesPerBlock < totalBufferSize + bytesPerBlock );

			buffers.SetNum( format.extra.xma2.blockCount );
			for ( int i = 0; i < buffers.Num(); i++ ) {
				if ( i == buffers.Num() - 1 ) {
					buffers[i].bufferSize = totalBufferSize - ( i * bytesPerBlock );
				} else {
					buffers[i].bufferSize = bytesPerBlock;
				}

				buffers[i].buffer = AllocBuffer( buffers[i].bufferSize, GetName() );
				wave.Read( buffers[i].buffer, buffers[i].bufferSize );
				buffers[i].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[i].buffer );
			}

			int seekTableSize = wave.SeekToChunk( 'seek' );
			if ( seekTableSize != 4 * buffers.Num() ) {
				idLib::Warning( "LoadWav( %s ) : %s", filename.c_str(), "Wrong number of entries in seek table" );
				MakeDefault();
				return false;
			}

			for ( int i = 0; i < buffers.Num(); i++ ) {
				wave.Read( &buffers[i].numSamples, sizeof( buffers[i].numSamples ) );
				idSwap::Big( buffers[i].numSamples );
			}

			playBegin = format.extra.xma2.loopBegin;
			playLength = format.extra.xma2.loopLength;

			if ( buffers[buffers.Num()-1].numSamples < playBegin + playLength ) {
				// This shouldn't happen, but it's not fatal if it does
				playLength = buffers[buffers.Num()-1].numSamples - playBegin;
			} else {
				// Discard samples beyond playLength
				for ( int i = 0; i < buffers.Num(); i++ ) {
					if ( buffers[i].numSamples > playBegin + playLength ) {
						buffers[i].numSamples = playBegin + playLength;
						// Ideally, the following loop should always have 0 iterations because playBegin + playLength ends in the last block already
						// But there is no guarantee for that, so to be safe, discard all buffers beyond this one
						for ( int j = i + 1; j < buffers.Num(); j++ ) {
							FreeBuffer( buffers[j].buffer );
						}
						buffers.SetNum( i + 1 );
						break;
					}
				}
			}

		} else {
			idLib::Warning( "LoadWav( %s ) : Unsupported wave format %d", filename.c_str(), format.basic.formatTag );
			MakeDefault();
			return false;
		}

		wave.Close();

		if ( format.basic.formatTag == idWaveFile::FORMAT_EXTENSIBLE ) {
			// HACK: XAudio2 doesn't really support FORMAT_EXTENSIBLE so we convert it to a basic format after extracting the channel mask
			format.basic.formatTag = format.extra.extensible.subFormat.data1;
		}

		// sanity check...
		assert( buffers[buffers.Num()-1].numSamples == playBegin + playLength );

		return true;
	}



	/*
	========================
	idSoundSample_XAudio2::MakeDefault
	========================
	*/
	void idSoundSample_XAudio2::MakeDefault() {
		FreeData();

		static const int DEFAULT_NUM_SAMPLES = 256;

		timestamp = FILE_NOT_FOUND_TIMESTAMP;
		loaded = true;
		isStreaming = false;
		m_decoder = NULL;

		memset( &format, 0, sizeof( format ) );
		format.basic.formatTag = idWaveFile::FORMAT_PCM;
		format.basic.numChannels = 1;
		format.basic.bitsPerSample = 16;
		format.basic.samplesPerSec = XAUDIO2_MIN_SAMPLE_RATE;
		format.basic.blockSize = format.basic.numChannels * format.basic.bitsPerSample / 8;
		format.basic.avgBytesPerSec = format.basic.samplesPerSec * format.basic.blockSize;

		assert( format.basic.blockSize == 2 );

		totalBufferSize = DEFAULT_NUM_SAMPLES * 2;

		short * defaultBuffer = (short *)AllocBuffer( totalBufferSize, GetName() );
		// VIS: silent default sample instead of the stock square-wave "beep", so a
		// missing/failed sound (e.g. at startup, or an unresolved decl) is silent
		// rather than an annoying tone.
		for ( int i = 0; i < DEFAULT_NUM_SAMPLES; i++ ) {
			defaultBuffer[i] = 0;
		}

		buffers.SetNum( 1 );
		buffers[0].buffer = defaultBuffer;
		buffers[0].bufferSize = totalBufferSize;
		buffers[0].numSamples = DEFAULT_NUM_SAMPLES;
		buffers[0].buffer = GPU_CONVERT_CPU_TO_CPU_CACHED_READONLY_ADDRESS( buffers[0].buffer );

		playBegin = 0;
		playLength = DEFAULT_NUM_SAMPLES;
	}

	/*
	========================
	idSoundSample_XAudio2::FreeData
	========================
	*/
	void idSoundSample_XAudio2::FreeData() {
		if ( buffers.Num() > 0 ) {
			soundSystemLocal.StopVoicesWithSample( (idSoundSample *)this );
			for ( int i = 0; i < buffers.Num(); i++ ) {
				FreeBuffer( buffers[i].buffer );
			}
			buffers.Clear();
		}
		if ( m_decoder ) { m_decoder->Close(); delete m_decoder; m_decoder = NULL; }
	amplitude.Clear();

		timestamp = FILE_NOT_FOUND_TIMESTAMP;
		memset( &format, 0, sizeof( format ) );
		loaded = false;
		totalBufferSize = 0;
		playBegin = 0;
		playLength = 0;
	}

	/*
	========================
	idSoundSample_XAudio2::LoadAmplitude
	========================
	*/
	bool idSoundSample_XAudio2::LoadAmplitude( const idStr & name ) {
		if ( m_decoder ) { m_decoder->Close(); delete m_decoder; m_decoder = NULL; }
	amplitude.Clear();
		idFileLocal f( fileSystem->OpenFileRead( name ) );
		if ( f == NULL ) {
			return false;
		}
		amplitude.SetNum( f->Length() );
		f->Read( amplitude.Ptr(), amplitude.Num() );
		return true;
	}

	/*
	========================
	idSoundSample_XAudio2::GetAmplitude
	========================
	*/
	float idSoundSample_XAudio2::GetAmplitude( int timeMS ) const {
		if ( timeMS < 0 || timeMS > LengthInMsec() ) {
			return 0.0f;
		}
		if ( IsDefault() ) {
			return 1.0f;
		}
		int index = timeMS * 60 / 1000;
		if ( index < 0 || index >= amplitude.Num() ) {
			return 0.0f;
		}
		return (float)amplitude[index] / 255.0f;
	}

	bool idSoundSample_XAudio2::LoadMedia( const idStr & filename ) {
		idFileLocal fileIn( fileSystem->OpenFileRead( filename ) );
		if ( fileIn == NULL ) {
			VisAudioDiagLog( "    LoadMedia: OpenFileRead('%s') -> NULL (file not found via idFileSystem)\n", filename.c_str() );
			return false;
		}
		timestamp = fileIn->Timestamp();
		// FFmpeg opens files through the real OS, not the engine's virtual file
		// system, so hand it the absolute on-disk path where the file was found.
		idStr osPath = fileIn->GetFullPath();
		VisAudioDiagLog( "    LoadMedia: found '%s' -> osPath='%s'\n", filename.c_str(), osPath.c_str() );

#if !ID_HAVE_FFMPEG
		VisAudioDiagLog( "    LoadMedia: ID_HAVE_FFMPEG is NOT defined in this build\n" );
		return false;
#else
		// Fully decode the file into one in-memory PCM16 buffer and play it as a
		// normal (non-streaming) sample. The XAudio2 streaming callback isn't wired
		// for our decoder, so streaming produced silence; a fully-loaded sample uses
		// the same proven path as .wav.
		idFFmpegDecoder dec;
		if ( !dec.Open( osPath.c_str() ) ) {
			VisAudioDiagLog( "    LoadMedia: dec.Open('%s') failed\n", osPath.c_str() );
			return false;
		}
		const int ch = dec.GetChannels();
		const int rate = dec.GetSampleRate();
		if ( ch <= 0 || rate <= 0 ) {
			return false;
		}

		std::vector<short> pcm;
		pcm.reserve( (size_t)rate * ch * 16 );        // ~16s to start; grows as needed
		const int blockFrames = 8192;
		std::vector<float> fbuf( (size_t)blockFrames * ch );
		const size_t maxSamples = (size_t)rate * ch * 60 * 30;   // cap ~30 min
		int frames;
		while ( ( frames = dec.DecodeNext( fbuf.data(), blockFrames ) ) > 0 ) {
			const int n = frames * ch;
			for ( int i = 0; i < n; i++ ) {
				float s = fbuf[i];
				if ( s > 1.0f ) s = 1.0f;
				if ( s < -1.0f ) s = -1.0f;
				pcm.push_back( (short)( s * 32767.0f ) );
			}
			if ( pcm.size() >= maxSamples ) {
				break;
			}
		}
		dec.Close();

		if ( pcm.empty() ) {
			idLib::Warning( "LoadMedia( %s ): decoded 0 samples", filename.c_str() );
			return false;
		}

		const int totalFrames = (int)( pcm.size() / ch );
		format.basic.formatTag = idWaveFile::FORMAT_PCM;
		format.basic.numChannels = ch;
		format.basic.bitsPerSample = 16;
		format.basic.samplesPerSec = rate;
		format.basic.blockSize = ch * 2;
		format.basic.avgBytesPerSec = rate * ch * 2;

		playBegin = 0;
		playLength = totalFrames;
		isStreaming = false;

		totalBufferSize = (int)( pcm.size() * sizeof( short ) );
		buffers.SetNum( 1 );
		buffers[0].bufferSize = totalBufferSize;
		buffers[0].numSamples = totalFrames;
		buffers[0].buffer = AllocBuffer( totalBufferSize, GetName() );
		memcpy( buffers[0].buffer, pcm.data(), totalBufferSize );

		idLib::Printf( "FFmpeg: loaded %.1fs (%d frames, %d Hz, %d ch)\n",
			(float)totalFrames / (float)rate, totalFrames, rate, ch );

		loaded = true;
		return true;
#endif
	}

// PRD FR-E5 / M4: MIDI input via the standard Windows multimedia API.
// See win_midi.h for the design notes (threading, why the header stays
// windows.h-free).
#pragma hdrstop
#include "../../idlib/precompiled.h"

#pragma comment(lib, "winmm.lib")

#include <windows.h>
#include <mmsystem.h>
#include <string.h>

#include "win_midi.h"

idMidiInput g_midiInput;

idMidiInput::idMidiInput() : m_handle( nullptr ) {
	memset( m_cc, 0, sizeof( m_cc ) );
	memset( m_noteVel, 0, sizeof( m_noteVel ) );
	memset( m_noteOn, 0, sizeof( m_noteOn ) );
}

idMidiInput::~idMidiInput() {
	Close();
}

int idMidiInput::NumDevices() {
	return (int)midiInGetNumDevs();
}

const char * idMidiInput::GetDeviceName( int index ) {
	static char name[MAXPNAMELEN];
	MIDIINCAPS caps;
	if ( midiInGetDevCaps( (UINT_PTR)index, &caps, sizeof( caps ) ) != MMSYSERR_NOERROR ) {
		return "<unknown>";
	}
	strncpy( name, caps.szPname, sizeof( name ) - 1 );
	name[ sizeof( name ) - 1 ] = '\0';
	return name;
}

// Runs on the MIDI driver's own callback thread (real-time context) --
// only does simple fixed-array writes via idMidiInput::On*, no allocation
// or blocking calls, matching winmm's documented callback constraints.
static void CALLBACK MidiInProc( HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2 ) {
	if ( wMsg != MIM_DATA ) {
		return;
	}
	idMidiInput * self = (idMidiInput *)dwInstance;
	if ( self == nullptr ) {
		return;
	}
	const DWORD raw = (DWORD)dwParam1;
	const int status  = raw & 0xFF;
	const int data1   = ( raw >> 8 )  & 0xFF;
	const int data2   = ( raw >> 16 ) & 0xFF;
	const int channel = status & 0x0F;
	const int type    = status & 0xF0;
	if ( type == 0x90 ) {          // note on (velocity 0 is a note-off per the MIDI spec)
		if ( data2 == 0 ) {
			self->OnNoteOff( channel, data1 );
		} else {
			self->OnNoteOn( channel, data1, data2 );
		}
	} else if ( type == 0x80 ) {   // note off
		self->OnNoteOff( channel, data1 );
	} else if ( type == 0xB0 ) {   // control change
		self->OnControlChange( channel, data1, data2 );
	}
}

bool idMidiInput::Open( int deviceIndex ) {
	Close();
	if ( midiInGetNumDevs() == 0 ) {
		idLib::Printf( "idMidiInput::Open: no MIDI input devices present\n" );
		return false;
	}
	const UINT dev = ( deviceIndex < 0 || deviceIndex >= (int)midiInGetNumDevs() ) ? 0 : (UINT)deviceIndex;
	HMIDIIN handle = nullptr;
	const MMRESULT result = midiInOpen( &handle, dev, (DWORD_PTR)MidiInProc, (DWORD_PTR)this, CALLBACK_FUNCTION );
	if ( result != MMSYSERR_NOERROR ) {
		idLib::Warning( "idMidiInput::Open: midiInOpen failed (device %u, error %d)", dev, result );
		return false;
	}
	midiInStart( handle );
	m_handle = handle;
	idLib::Printf( "idMidiInput: opened device %u ('%s')\n", dev, GetDeviceName( dev ) );
	return true;
}

void idMidiInput::Close() {
	if ( m_handle != nullptr ) {
		HMIDIIN handle = (HMIDIIN)m_handle;
		midiInStop( handle );
		midiInClose( handle );
		m_handle = nullptr;
	}
}

void idMidiInput::OnNoteOn( int channel, int note, int velocity ) {
	if ( channel < 0 || channel >= 16 || note < 0 || note >= 128 ) {
		return;
	}
	m_noteVel[channel][note] = velocity / 127.0f;
	m_noteOn[channel][note] = true;
}

void idMidiInput::OnNoteOff( int channel, int note ) {
	if ( channel < 0 || channel >= 16 || note < 0 || note >= 128 ) {
		return;
	}
	m_noteOn[channel][note] = false;
}

void idMidiInput::OnControlChange( int channel, int ccNumber, int value ) {
	if ( channel < 0 || channel >= 16 || ccNumber < 0 || ccNumber >= 128 ) {
		return;
	}
	m_cc[channel][ccNumber] = value / 127.0f;
}

float idMidiInput::GetCC( int channel, int ccNumber ) const {
	if ( channel < 0 || channel >= 16 || ccNumber < 0 || ccNumber >= 128 ) {
		return 0.0f;
	}
	return m_cc[channel][ccNumber];
}

float idMidiInput::GetNoteVelocity( int channel, int note ) const {
	if ( channel < 0 || channel >= 16 || note < 0 || note >= 128 ) {
		return 0.0f;
	}
	return m_noteVel[channel][note];
}

bool idMidiInput::GetNoteOn( int channel, int note ) const {
	if ( channel < 0 || channel >= 16 || note < 0 || note >= 128 ) {
		return false;
	}
	return m_noteOn[channel][note];
}

// PRD FR-E5's other half: MIDI OUT (symmetric counterpart of idMidiInput
// above). See win_midi.h for the design notes.
idMidiOutput g_midiOutput;

idMidiOutput::idMidiOutput() : m_handle( nullptr ) {
}

idMidiOutput::~idMidiOutput() {
	Close();
}

int idMidiOutput::NumDevices() {
	return (int)midiOutGetNumDevs();
}

const char * idMidiOutput::GetDeviceName( int index ) {
	static char name[MAXPNAMELEN];
	MIDIOUTCAPS caps;
	if ( midiOutGetDevCaps( (UINT_PTR)index, &caps, sizeof( caps ) ) != MMSYSERR_NOERROR ) {
		return "<unknown>";
	}
	strncpy( name, caps.szPname, sizeof( name ) - 1 );
	name[ sizeof( name ) - 1 ] = '\0';
	return name;
}

bool idMidiOutput::Open( int deviceIndex ) {
	Close();
	if ( midiOutGetNumDevs() == 0 ) {
		idLib::Printf( "idMidiOutput::Open: no MIDI output devices present\n" );
		return false;
	}
	const UINT dev = ( deviceIndex < 0 || deviceIndex >= (int)midiOutGetNumDevs() ) ? 0 : (UINT)deviceIndex;
	HMIDIOUT handle = nullptr;
	const MMRESULT result = midiOutOpen( &handle, dev, 0, 0, CALLBACK_NULL );
	if ( result != MMSYSERR_NOERROR ) {
		idLib::Warning( "idMidiOutput::Open: midiOutOpen failed (device %u, error %d)", dev, result );
		return false;
	}
	m_handle = handle;
	idLib::Printf( "idMidiOutput: opened device %u ('%s')\n", dev, GetDeviceName( dev ) );
	return true;
}

void idMidiOutput::Close() {
	if ( m_handle != nullptr ) {
		HMIDIOUT handle = (HMIDIOUT)m_handle;
		midiOutClose( handle );
		m_handle = nullptr;
	}
}

void idMidiOutput::SendShortMsg( unsigned char status, unsigned char data1, unsigned char data2 ) {
	if ( m_handle == nullptr ) {
		return;
	}
	const DWORD msg = (DWORD)status | ( (DWORD)data1 << 8 ) | ( (DWORD)data2 << 16 );
	midiOutShortMsg( (HMIDIOUT)m_handle, msg );
}

void idMidiOutput::SendNoteOn( int channel, int note, float velocity ) {
	if ( channel < 0 || channel >= 16 || note < 0 || note >= 128 ) {
		return;
	}
	const int vel = idMath::ClampInt( 1, 127, (int)( velocity * 127.0f ) );   // 0 velocity is a note-off, not a quiet note-on
	SendShortMsg( (unsigned char)( 0x90 | channel ), (unsigned char)note, (unsigned char)vel );
}

void idMidiOutput::SendNoteOff( int channel, int note ) {
	if ( channel < 0 || channel >= 16 || note < 0 || note >= 128 ) {
		return;
	}
	SendShortMsg( (unsigned char)( 0x80 | channel ), (unsigned char)note, 0 );
}

void idMidiOutput::SendCC( int channel, int ccNumber, float value ) {
	if ( channel < 0 || channel >= 16 || ccNumber < 0 || ccNumber >= 128 ) {
		return;
	}
	const int v = idMath::ClampInt( 0, 127, (int)( value * 127.0f ) );
	SendShortMsg( (unsigned char)( 0xB0 | channel ), (unsigned char)ccNumber, (unsigned char)v );
}

void idMidiOutput::SendClockPulse() {
	SendShortMsg( 0xF8, 0, 0 );
}

void idMidiOutput::SendStart() {
	SendShortMsg( 0xFA, 0, 0 );
}

void idMidiOutput::SendStop() {
	SendShortMsg( 0xFC, 0, 0 );
}

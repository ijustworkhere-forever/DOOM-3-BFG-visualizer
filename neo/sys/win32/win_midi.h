#ifndef __WIN_MIDI_H__
#define __WIN_MIDI_H__

/*
================================================
idMidiInput

PRD FR-E5 (MIDI I/O), M4 first slice: MIDI input via the standard Windows
multimedia API (winmm midiIn*), no external SDK -- see
plans/PRD-zge-milkdrop-visualizer.md FR-E5 and
plans/PRD-implementation-status.md M4 for the full scope/rationale.

All Windows-specific types (HMIDIIN, DWORD_PTR, the MidiInProc callback)
stay inside win_midi.cpp; this header only exposes a plain C++ interface so
cross-platform-intended code (visualizer_manager.cpp) can include it
without pulling in <windows.h>.

Threading: the OnNoteOn/OnNoteOff/OnControlChange setters are called from
the MIDI driver's own callback thread (a real-time context -- they do only
simple array writes, no allocation/locking); Get* accessors are called from
the main thread. Single-float/bool reads and writes are the same
lightweight-safety assumption this codebase already relies on elsewhere
(e.g. the s_mod[] modulation cache in visualizer_manager.cpp) -- no new
synchronization primitive introduced for this.
================================================
*/
class idMidiInput {
public:
	idMidiInput();
	~idMidiInput();

	static int			NumDevices();
	static const char *	GetDeviceName( int index );   // static buffer, valid until the next call

	bool				Open( int deviceIndex );   // -1 or out-of-range -> device 0 (first available)
	void				Close();
	bool				IsOpen() const { return m_handle != nullptr; }

	// last-received values, 0..1 normalized from the 7-bit MIDI range.
	float				GetCC( int channel, int ccNumber ) const;
	float				GetNoteVelocity( int channel, int note ) const;
	bool				GetNoteOn( int channel, int note ) const;

	// called by the file-static MidiInProc callback in win_midi.cpp -- public
	// only because a C callback can't invoke a private member function;
	// not part of the intended external API.
	void				OnNoteOn( int channel, int note, int velocity );
	void				OnNoteOff( int channel, int note );
	void				OnControlChange( int channel, int ccNumber, int value );

private:
	void *	m_handle;   // HMIDIIN, kept opaque so this header stays windows.h-free
	float	m_cc[16][128];
	float	m_noteVel[16][128];
	bool	m_noteOn[16][128];
};

extern idMidiInput g_midiInput;

/*
================================================
idMidiOutput

PRD FR-E5's other half: MIDI OUT via winmm midiOut* (the symmetric
counterpart of idMidiInput above -- same device-model, same
windows.h-free-header discipline). Sends short messages only (note on/off,
control change, and raw MIDI clock/timing bytes) -- no SysEx.

All calls happen from the main thread (visualizer_manager.cpp's Frame()),
no callback thread involved on the output side, so no special threading
discipline is needed here beyond what any other main-thread-only class in
this codebase already assumes.
================================================
*/
class idMidiOutput {
public:
	idMidiOutput();
	~idMidiOutput();

	static int			NumDevices();
	static const char *	GetDeviceName( int index );   // static buffer, valid until the next call

	bool				Open( int deviceIndex );   // -1 or out-of-range -> device 0 (first available)
	void				Close();
	bool				IsOpen() const { return m_handle != nullptr; }

	// value 0..1, normalized to the 7-bit MIDI range on send.
	void				SendNoteOn( int channel, int note, float velocity );
	void				SendNoteOff( int channel, int note );
	void				SendCC( int channel, int ccNumber, float value );

	// MIDI realtime bytes (0xF8 clock / 0xFA start / 0xFC stop), no channel
	// or data bytes -- see visualizer_manager.cpp's vis_midiClockRate for
	// how the 24-pulses-per-quarter-note clock is paced from the BPM estimate.
	void				SendClockPulse();
	void				SendStart();
	void				SendStop();

private:
	void	SendShortMsg( unsigned char status, unsigned char data1, unsigned char data2 );

	void *	m_handle;   // HMIDIOUT, kept opaque so this header stays windows.h-free
};

extern idMidiOutput g_midiOutput;

#endif // __WIN_MIDI_H__

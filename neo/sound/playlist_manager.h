#ifndef __PLAYLIST_MANAGER_H__
#define __PLAYLIST_MANAGER_H__

#include "../idlib/precompiled.h"

struct idPlaylistEntry {
    idStr path;
    idStr title;
};

/*
================================================
idPlaylistManager

Ordered track list with a play cursor. Loads either a single audio file or an
.m3u-style playlist (one path per line, '#' comments/EXTINF ignored). Paths are
engine-relative (searched via idFileSystem) unless they contain a drive/absolute
prefix, in which case they are passed through for explicit-OS-path playback.
================================================
*/
class idPlaylistManager {
public:
    idPlaylistManager();

    // Replaces the current list. Accepts a playlist file (.m3u/.m3u8/.pls-lite)
    // or a single audio file path. Returns number of tracks loaded.
    int             LoadPlaylist( const char * path );

    void            Clear();
    void            AddTrack( const char * path, const char * title = NULL );

    // returns NULL when the list is empty
    const char *    GetCurrentTrack() const;
    int             GetCurrentIndex() const { return m_currentIndex; }
    bool            SetCurrentIndex( int index );

    void            PlayNext();
    void            PlayPrev();

    int             GetNumTracks() const { return m_tracks.Num(); }
    const idPlaylistEntry & GetTrack( int index ) const;

private:
    idList<idPlaylistEntry> m_tracks;
    int                     m_currentIndex;
};

#endif // __PLAYLIST_MANAGER_H__

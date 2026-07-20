#include "../idlib/precompiled.h"
#pragma hdrstop
#include "playlist_manager.h"

static bool IsPlaylistFile( const char * path ) {
    idStr ext;
    idStr( path ).ExtractFileExtension( ext );
    return ( ext.Icmp( "m3u" ) == 0 ) || ( ext.Icmp( "m3u8" ) == 0 ) || ( ext.Icmp( "pls" ) == 0 );
}

idPlaylistManager::idPlaylistManager() :
    m_currentIndex( 0 ) {
}

void idPlaylistManager::Clear() {
    m_tracks.Clear();
    m_currentIndex = 0;
}

void idPlaylistManager::AddTrack( const char * path, const char * title ) {
    if ( path == NULL || path[0] == '\0' ) {
        return;
    }
    idPlaylistEntry & entry = m_tracks.Alloc();
    entry.path = path;
    if ( title != NULL && title[0] != '\0' ) {
        entry.title = title;
    } else {
        entry.title = path;
        entry.title.StripPath();
        entry.title.StripFileExtension();
    }
}

int idPlaylistManager::LoadPlaylist( const char * path ) {
    Clear();
    if ( path == NULL || path[0] == '\0' ) {
        return 0;
    }

    if ( !IsPlaylistFile( path ) ) {
        AddTrack( path );
        return m_tracks.Num();
    }

    void * buffer = NULL;
    int len = fileSystem->ReadFile( path, &buffer, NULL );
    if ( len <= 0 || buffer == NULL ) {
        idLib::Warning( "idPlaylistManager: couldn't read playlist '%s'", path );
        return 0;
    }

    idStr text( (const char *)buffer, 0, len );
    fileSystem->FreeFile( buffer );

    idStr pendingTitle;
    int lineStart = 0;
    for ( int i = 0; i <= text.Length(); i++ ) {
        if ( i < text.Length() && text[i] != '\n' && text[i] != '\r' ) {
            continue;
        }
        idStr line = text.Mid( lineStart, i - lineStart );
        lineStart = i + 1;
        line.StripLeading( ' ' );
        line.StripLeading( '\t' );
        line.StripTrailingWhitespace();
        if ( line.IsEmpty() ) {
            continue;
        }
        if ( line[0] == '#' ) {
            // keep #EXTINF display titles: "#EXTINF:123,Artist - Title"
            if ( line.Icmpn( "#EXTINF", 7 ) == 0 ) {
                int comma = line.Find( ',' );
                if ( comma >= 0 ) {
                    pendingTitle = line.Mid( comma + 1, line.Length() - comma - 1 );
                }
            }
            continue;
        }
        line.BackSlashesToSlashes();
        AddTrack( line, pendingTitle.c_str() );
        pendingTitle.Clear();
    }

    return m_tracks.Num();
}

const char * idPlaylistManager::GetCurrentTrack() const {
    if ( m_tracks.Num() > 0 && m_currentIndex >= 0 && m_currentIndex < m_tracks.Num() ) {
        return m_tracks[m_currentIndex].path.c_str();
    }
    return NULL;
}

bool idPlaylistManager::SetCurrentIndex( int index ) {
    if ( index < 0 || index >= m_tracks.Num() ) {
        return false;
    }
    m_currentIndex = index;
    return true;
}

void idPlaylistManager::PlayNext() {
    if ( m_tracks.Num() > 0 ) {
        m_currentIndex = ( m_currentIndex + 1 ) % m_tracks.Num();
    }
}

void idPlaylistManager::PlayPrev() {
    if ( m_tracks.Num() > 0 ) {
        m_currentIndex = ( m_currentIndex + m_tracks.Num() - 1 ) % m_tracks.Num();
    }
}

const idPlaylistEntry & idPlaylistManager::GetTrack( int index ) const {
    static idPlaylistEntry empty;
    if ( index >= 0 && index < m_tracks.Num() ) {
        return m_tracks[index];
    }
    return empty;
}

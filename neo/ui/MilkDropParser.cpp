#include "../idlib/precompiled.h"
#pragma hdrstop
#include "MilkDropParser.h"

#include <string>
#include <sstream>
#include <cstdlib>

static void TrimInPlace( std::string & s ) {
    const char * ws = " \t\r\n";
    size_t first = s.find_first_not_of( ws );
    if ( first == std::string::npos ) {
        s.clear();
        return;
    }
    size_t last = s.find_last_not_of( ws );
    s = s.substr( first, last - first + 1 );
}

// reads lines until one equals END; returns them joined with '\n'
static std::string ReadBlock( std::istringstream & in ) {
    std::string block, line;
    while ( std::getline( in, line ) ) {
        TrimInPlace( line );
        if ( line == "END" ) {
            break;
        }
        block += line;
        block += "\n";
    }
    return block;
}

bool MilkDropParser::Parse( const char * filePath, VisualizerPreset & outPreset ) {
    if ( filePath == NULL || filePath[0] == '\0' ) {
        return false;
    }

    void * buffer = NULL;
    int len = fileSystem->ReadFile( filePath, &buffer, NULL );
    if ( len <= 0 || buffer == NULL ) {
        idLib::Warning( "MilkDropParser: couldn't read preset '%s'", filePath );
        return false;
    }

    const bool ok = ParseText( (const char *)buffer, len, outPreset );
    fileSystem->FreeFile( buffer );
    return ok;
}

bool MilkDropParser::ParseText( const char * text, int length, VisualizerPreset & outPreset ) {
    outPreset.name = "";
    outPreset.vertexShader.clear();
    outPreset.fragmentShader.clear();
    outPreset.parameters.clear();

    std::istringstream in( std::string( text, (size_t)length ) );
    std::string line, section;

    while ( std::getline( in, line ) ) {
        TrimInPlace( line );
        if ( line.empty() || line[0] == '#' ) {
            continue;
        }

        if ( line.front() == '[' && line.back() == ']' ) {
            section = line.substr( 1, line.length() - 2 );
            continue;
        }

        size_t colon = line.find( ':' );
        if ( colon == std::string::npos ) {
            continue;
        }
        std::string key = line.substr( 0, colon );
        std::string value = line.substr( colon + 1 );
        TrimInPlace( key );
        TrimInPlace( value );

        if ( section == "effect" ) {
            if ( key == "vertex_shader" || key == "vertex_shader_path" ) {
                outPreset.vertexShader = value.empty() ? ReadBlock( in ) : value;
            } else if ( key == "fragment_shader" || key == "fragment_shader_path" ) {
                outPreset.fragmentShader = value.empty() ? ReadBlock( in ) : value;
            } else if ( key == "shader" ) {
                outPreset.vertexShader = value;
                outPreset.fragmentShader = value;
            }
        } else if ( section == "preset" ) {
            if ( key == "name" ) {
                outPreset.name = value.c_str();
            }
        } else if ( section == "param" ) {
            // key is expected to be "param <index>"
            size_t space = key.find_last_of( ' ' );
            if ( space != std::string::npos ) {
                const int index = atoi( key.substr( space + 1 ).c_str() );
                outPreset.parameters[index] = (float)atof( value.c_str() );
            }
        }
    }

    return !outPreset.name.IsEmpty();
}

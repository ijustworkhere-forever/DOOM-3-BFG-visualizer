#pragma hdrstop
#include "../idlib/precompiled.h"
#include "MilkPreset.h"

// A single numbered fragment ("per_frame_3=...", "wave_1_per_point7=...")
// collected during the scan, joined in numeric (not file) order afterward.
struct milkFrag_t {
	int		n;
	idStr	text;
};

static void MilkFragAdd( idList<milkFrag_t> & frags, int n, const char * text ) {
	milkFrag_t f;
	f.n = n;
	f.text = text;
	frags.Append( f );
}

// Join fragments in ascending numeric order with "\n" between them. The
// separator matters: some fragments are bare "//" comments with no
// trailing ';' (verified in real presets, e.g. per_frame_1000=// comment),
// and the vendored EEL2 lexer (Scanner.l) only ends a "//" comment at the
// next '\n' -- concatenating with no separator would let a comment-only
// fragment swallow everything after it. A '\n' is also safe for the
// opposite case (one expression split across numbered fragments, e.g.
// per_frame_1=ib_r=0.7+0.4* / per_frame_2=   sin(3*time);) since the same
// lexer treats '\n' as insignificant whitespace outside a comment.
static idStr MilkFragJoin( idList<milkFrag_t> & frags ) {
	// small N (tens at most per preset) -- plain insertion sort by n.
	for ( int i = 1; i < frags.Num(); i++ ) {
		milkFrag_t key = frags[i];
		int j = i - 1;
		while ( j >= 0 && frags[j].n > key.n ) {
			frags[j + 1] = frags[j];
			j--;
		}
		frags[j + 1] = key;
	}
	idStr out;
	for ( int i = 0; i < frags.Num(); i++ ) {
		if ( !out.IsEmpty() ) {
			out += "\n";
		}
		out += frags[i].text;
	}
	return out;
}

// Temporary per-index accumulator used only while scanning; folded into the
// final milkWaveCode_t/milkShapeCode_t (plain joined idStr, no scratch
// state) once the whole file has been read.
struct milkWaveTemp_t {
	int					index;
	idDict				params;
	idList<milkFrag_t>	perFrame;
	idList<milkFrag_t>	perPoint;
};
struct milkShapeTemp_t {
	int					index;
	idDict				params;
	idList<milkFrag_t>	perFrame;
};

static milkWaveTemp_t * MilkFindOrAddWave( idList<milkWaveTemp_t> & waves, int index ) {
	for ( int i = 0; i < waves.Num(); i++ ) {
		if ( waves[i].index == index ) {
			return &waves[i];
		}
	}
	milkWaveTemp_t w;
	w.index = index;
	waves.Append( w );
	return &waves[waves.Num() - 1];
}

static milkShapeTemp_t * MilkFindOrAddShape( idList<milkShapeTemp_t> & shapes, int index ) {
	for ( int i = 0; i < shapes.Num(); i++ ) {
		if ( shapes[i].index == index ) {
			return &shapes[i];
		}
	}
	milkShapeTemp_t s;
	s.index = index;
	shapes.Append( s );
	return &shapes[shapes.Num() - 1];
}

// Parses a leading run of digits at the start of `s`, writing the value to
// `outNum` and the remainder (everything after the digits) to `outRest`.
// Returns false if `s` doesn't start with at least one digit.
static bool MilkParseLeadingInt( const char * s, int & outNum, idStr & outRest ) {
	int i = 0;
	int val = 0;
	while ( s[i] >= '0' && s[i] <= '9' ) {
		val = val * 10 + ( s[i] - '0' );
		i++;
	}
	if ( i == 0 ) {
		return false;
	}
	outNum = val;
	outRest = s + i;
	return true;
}

idMilkPreset::idMilkPreset() {
}

void idMilkPreset::Clear() {
	m_name.Clear();
	m_params.Clear();
	m_perFrameInit.Clear();
	m_perFrame.Clear();
	m_perPixel.Clear();
	m_warpShader.Clear();
	m_compShader.Clear();
	m_waves.Clear();
	m_shapes.Clear();
}

bool idMilkPreset::Load( const char * vfsPath ) {
	Clear();

	void * bufferVoid = NULL;
	const int len = fileSystem->ReadFile( vfsPath, &bufferVoid, NULL );
	if ( len <= 0 || bufferVoid == NULL ) {
		idLib::Warning( "idMilkPreset::Load: couldn't read '%s'", vfsPath );
		return false;
	}
	// ReadFile always null-terminates at buf[len] (see FileSystem.cpp), so a
	// plain C-string wrap is safe for well-formed text presets.
	idStr text = (const char *)bufferVoid;
	fileSystem->FreeFile( bufferVoid );

	m_name = vfsPath;
	m_name.StripPath();
	m_name.StripFileExtension();

	idList<milkFrag_t> perFrameInitFrags, perFrameFrags, perPixelFrags, warpFrags, compFrags;
	idList<milkWaveTemp_t> waves;
	idList<milkShapeTemp_t> shapes;

	int lineStart = 0;
	const int textLen = text.Length();
	for ( int pos = 0; pos <= textLen; pos++ ) {
		if ( pos != textLen && text[pos] != '\n' ) {
			continue;
		}
		idStr line = text.Mid( lineStart, pos - lineStart );
		lineStart = pos + 1;
		line.StripTrailing( '\r' );
		// interleaved space/tab indentation (e.g. "\t \tkey=...") needs both
		// stripped in alternation, not just once each -- a single pass of
		// StripLeading(' ') then StripLeading('\t') stops as soon as it hits
		// the OTHER whitespace char, leaving some behind.
		int prevLen;
		do {
			prevLen = line.Length();
			line.StripLeading( ' ' );
			line.StripLeading( '\t' );
		} while ( line.Length() != prevLen );
		line.StripTrailingWhitespace();

		if ( line.IsEmpty() || line[0] == '[' ) {
			continue;	// blank line, or the "[presetNN]" section header
		}
		if ( line.Length() >= 2 && line[0] == '/' && line[1] == '/' ) {
			continue;	// bare top-level comment line (not a key=value pair)
		}

		const int eq = line.Find( '=' );
		if ( eq < 0 ) {
			continue;	// not a key=value line -- tolerate and skip (NFR-3)
		}
		idStr key = line.Left( eq );
		idStr value = line.Mid( eq + 1, line.Length() - eq - 1 );
		const char * k = key.c_str();
		int num;
		idStr rest;

		if ( idStr::Icmpn( k, "per_frame_init_", 15 ) == 0 && MilkParseLeadingInt( k + 15, num, rest ) && rest.IsEmpty() ) {
			MilkFragAdd( perFrameInitFrags, num, value.c_str() );
		} else if ( idStr::Icmpn( k, "per_frame_", 10 ) == 0 && MilkParseLeadingInt( k + 10, num, rest ) && rest.IsEmpty() ) {
			MilkFragAdd( perFrameFrags, num, value.c_str() );
		} else if ( idStr::Icmpn( k, "per_pixel_", 10 ) == 0 && MilkParseLeadingInt( k + 10, num, rest ) && rest.IsEmpty() ) {
			MilkFragAdd( perPixelFrags, num, value.c_str() );
		} else if ( idStr::Icmpn( k, "warp_", 5 ) == 0 && MilkParseLeadingInt( k + 5, num, rest ) && rest.IsEmpty() ) {
			idStr v = value;
			v.StripLeading( '`' );		// real files prefix shader-body lines with a backtick
			MilkFragAdd( warpFrags, num, v.c_str() );
		} else if ( idStr::Icmpn( k, "comp_", 5 ) == 0 && MilkParseLeadingInt( k + 5, num, rest ) && rest.IsEmpty() ) {
			idStr v = value;
			v.StripLeading( '`' );
			MilkFragAdd( compFrags, num, v.c_str() );
		} else if ( idStr::Icmpn( k, "wavecode_", 9 ) == 0 && MilkParseLeadingInt( k + 9, num, rest ) && !rest.IsEmpty() && rest[0] == '_' ) {
			MilkFindOrAddWave( waves, num )->params.Set( rest.c_str() + 1, value.c_str() );
		} else if ( idStr::Icmpn( k, "wave_", 5 ) == 0 && MilkParseLeadingInt( k + 5, num, rest ) && rest.Length() > 1 && rest[0] == '_' ) {
			const char * sub = rest.c_str() + 1;	// "per_frame3" / "per_point12"
			int fragNum;
			idStr fragRest;
			if ( idStr::Icmpn( sub, "per_frame", 9 ) == 0 && MilkParseLeadingInt( sub + 9, fragNum, fragRest ) && fragRest.IsEmpty() ) {
				MilkFragAdd( MilkFindOrAddWave( waves, num )->perFrame, fragNum, value.c_str() );
			} else if ( idStr::Icmpn( sub, "per_point", 9 ) == 0 && MilkParseLeadingInt( sub + 9, fragNum, fragRest ) && fragRest.IsEmpty() ) {
				MilkFragAdd( MilkFindOrAddWave( waves, num )->perPoint, fragNum, value.c_str() );
			}
			// else: a top-level "wave_r"/"wave_g"/"wave_x"/... scalar that
			// happens to start with "wave_" but isn't "wave_<N>_..." at all
			// (MilkParseLeadingInt already rejected non-numeric, so this
			// branch is only reached for "wave_<N>_something-unrecognized" --
			// harmless to drop, NFR-3).
		} else if ( idStr::Icmpn( k, "shapecode_", 10 ) == 0 && MilkParseLeadingInt( k + 10, num, rest ) && !rest.IsEmpty() && rest[0] == '_' ) {
			MilkFindOrAddShape( shapes, num )->params.Set( rest.c_str() + 1, value.c_str() );
		} else if ( idStr::Icmpn( k, "shape_", 6 ) == 0 && MilkParseLeadingInt( k + 6, num, rest ) && rest.Length() > 1 && rest[0] == '_' ) {
			const char * sub = rest.c_str() + 1;
			int fragNum;
			idStr fragRest;
			if ( idStr::Icmpn( sub, "per_frame", 9 ) == 0 && MilkParseLeadingInt( sub + 9, fragNum, fragRest ) && fragRest.IsEmpty() ) {
				MilkFragAdd( MilkFindOrAddShape( shapes, num )->perFrame, fragNum, value.c_str() );
			}
		} else {
			// generic scalar builtin: fDecay, zoom, rot, warp, fWarpScale,
			// ob_*/ib_*, wave_r/g/b/a/x/y, sx/sy/cx/cy/dx/dy, nWaveMode, ...
			m_params.Set( k, value.c_str() );
		}
	}

	m_perFrameInit = MilkFragJoin( perFrameInitFrags );
	m_perFrame = MilkFragJoin( perFrameFrags );
	m_perPixel = MilkFragJoin( perPixelFrags );
	m_warpShader = MilkFragJoin( warpFrags );
	m_compShader = MilkFragJoin( compFrags );

	for ( int i = 0; i < waves.Num(); i++ ) {
		milkWaveCode_t w;
		w.index = waves[i].index;
		w.params = waves[i].params;
		w.perFrame = MilkFragJoin( waves[i].perFrame );
		w.perPoint = MilkFragJoin( waves[i].perPoint );
		m_waves.Append( w );
	}
	for ( int i = 0; i < shapes.Num(); i++ ) {
		milkShapeCode_t s;
		s.index = shapes[i].index;
		s.params = shapes[i].params;
		s.perFrame = MilkFragJoin( shapes[i].perFrame );
		m_shapes.Append( s );
	}

	return true;
}

void idMilkPreset::PrintSummary() const {
	idLib::Printf( "milk preset '%s'\n", m_name.c_str() );
	idLib::Printf( "  scalar params: %d\n", m_params.GetNumKeyVals() );
	idLib::Printf( "  per_frame_init: %d chars\n", m_perFrameInit.Length() );
	idLib::Printf( "  per_frame     : %d chars\n", m_perFrame.Length() );
	idLib::Printf( "  per_pixel     : %d chars\n", m_perPixel.Length() );
	idLib::Printf( "  warp shader   : %d chars\n", m_warpShader.Length() );
	idLib::Printf( "  comp shader   : %d chars\n", m_compShader.Length() );
	idLib::Printf( "  waves: %d, shapes: %d\n", m_waves.Num(), m_shapes.Num() );
	for ( int i = 0; i < m_waves.Num(); i++ ) {
		idLib::Printf( "    wave %d: %d params, perFrame %d chars, perPoint %d chars\n",
			m_waves[i].index, m_waves[i].params.GetNumKeyVals(),
			m_waves[i].perFrame.Length(), m_waves[i].perPoint.Length() );
	}
	for ( int i = 0; i < m_shapes.Num(); i++ ) {
		idLib::Printf( "    shape %d: %d params, perFrame %d chars\n",
			m_shapes[i].index, m_shapes[i].params.GetNumKeyVals(), m_shapes[i].perFrame.Length() );
	}
}

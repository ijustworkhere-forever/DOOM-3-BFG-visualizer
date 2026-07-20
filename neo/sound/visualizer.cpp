#include "../idlib/precompiled.h"
#pragma hdrstop
#include "visualizer.h"
#include "visualizer_data.h"
#include <algorithm>
#include <cmath>

Visualizer::Visualizer() {
    for ( int i = 0; i < 64; i++ ) {
        m_barHeights[i] = 0.0f;
    }
}

Visualizer::~Visualizer() {}

void Visualizer::Update() {
    // map the 512 valid magnitude bins onto 64 bars (bins FFT_SIZE/2..FFT_SIZE-1
    // are the mirrored half and always empty - don't sample them)
    const int binsPerBar = ( FFT_SIZE / 2 ) / 64;
    for ( int i = 0; i < 64; i++ ) {
        float peak = 0.0f;
        for ( int b = 0; b < binsPerBar; b++ ) {
            peak = std::max( peak, g_audioAnalyzer.GetSmoothedMagnitude( i * binsPerBar + b ) );
        }
        m_barHeights[i] += ( peak - m_barHeights[i] ) * m_smoothing;
    }
}

float Visualizer::GetBarHeight( int barIndex ) const {
    if ( barIndex >= 0 && barIndex < 64 ) {
        return m_barHeights[barIndex];
    }
    return 0.0f;
}

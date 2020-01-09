/*
 * This program source code file is part of KICAD, a free EDA CAD application.
 *
 * Copyright (C) 2012 Torsten Hueter, torstenhtr <at> gmx.de
 * Copyright (C) 2013 CERN
 * @author Maciej Suminski <maciej.suminski@cern.ch>
 * Copyright (C) 2016 Kicad Developers, see change_log.txt for contributors.
 *
 * Stroke font class
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <gal/stroke_font.h>
#include <gal/graphics_abstraction_layer.h>
#include <math/util.h>      // for KiROUND
#include <wx/string.h>
#include <gr_text.h>


using namespace KIGFX;

const double STROKE_FONT::INTERLINE_PITCH_RATIO = 1.61;
const double STROKE_FONT::OVERBAR_POSITION_FACTOR = 1.22;
const double STROKE_FONT::BOLD_FACTOR = 1.3;
const double STROKE_FONT::STROKE_FONT_SCALE = 1.0 / 21.0;
const double STROKE_FONT::ITALIC_TILT = 1.0 / 8;


GLYPH_LIST*         g_newStrokeFontGlyphs = nullptr;     ///< Glyph list
std::vector<BOX2D>* g_newStrokeFontGlyphBoundingBoxes;   ///< Bounding boxes of the glyphs


STROKE_FONT::STROKE_FONT( GAL* aGal ) :
    m_gal( aGal ), m_glyphs( nullptr ), m_glyphBoundingBoxes( nullptr )
{
}


bool STROKE_FONT::LoadNewStrokeFont( const char* const aNewStrokeFont[], int aNewStrokeFontSize )
{
    if( g_newStrokeFontGlyphs )
    {
        m_glyphs = g_newStrokeFontGlyphs;
        m_glyphBoundingBoxes = g_newStrokeFontGlyphBoundingBoxes;
        return true;
    }

    g_newStrokeFontGlyphs = new GLYPH_LIST;
    g_newStrokeFontGlyphs->reserve( aNewStrokeFontSize );

    g_newStrokeFontGlyphBoundingBoxes = new std::vector<BOX2D>;
    g_newStrokeFontGlyphBoundingBoxes->reserve( aNewStrokeFontSize );

    for( int j = 0; j < aNewStrokeFontSize; j++ )
    {
        GLYPH    glyph;
        double   glyphStartX = 0.0;
        double   glyphEndX = 0.0;
        double   glyphWidth = 0.0;

        std::vector<VECTOR2D>* pointList = nullptr;

        int strokes = 0;
        int i = 0;

        while( aNewStrokeFont[j][i] )
        {

            if( aNewStrokeFont[j][i] == ' ' && aNewStrokeFont[j][i+1] == 'R' )
                strokes++;

            i += 2;
        }

        glyph.reserve( strokes + 1 );

        i = 0;

        while( aNewStrokeFont[j][i] )
        {
            VECTOR2D    point( 0.0, 0.0 );
            char        coordinate[2] = { 0, };

            for( int k : { 0, 1 } )
                coordinate[k] = aNewStrokeFont[j][i + k];

            if( i < 2 )
            {
                // The first two values contain the width of the char
                glyphStartX = ( coordinate[0] - 'R' ) * STROKE_FONT_SCALE;
                glyphEndX   = ( coordinate[1] - 'R' ) * STROKE_FONT_SCALE;
                glyphWidth  = glyphEndX - glyphStartX;
            }
            else if( ( coordinate[0] == ' ' ) && ( coordinate[1] == 'R' ) )
            {
                if( pointList )
                    pointList->shrink_to_fit();

                // Raise pen
                pointList = nullptr;
            }
            else
            {
                // In stroke font, coordinates values are coded as <value> + 'R',
                // <value> is an ASCII char.
                // therefore every coordinate description of the Hershey format has an offset,
                // it has to be subtracted
                // Note:
                //  * the stroke coordinates are stored in reduced form (-1.0 to +1.0),
                //    and the actual size is stroke coordinate * glyph size
                //  * a few shapes have a height slightly bigger than 1.0 ( like '{' '[' )
                point.x = (double) ( coordinate[0] - 'R' ) * STROKE_FONT_SCALE - glyphStartX;
                #define FONT_OFFSET -10
                // FONT_OFFSET is here for historical reasons, due to the way the stroke font
                // was built. It allows shapes coordinates like W M ... to be >= 0
                // Only shapes like j y have coordinates < 0
                point.y = (double) ( coordinate[1] - 'R' + FONT_OFFSET ) * STROKE_FONT_SCALE;

                if( !pointList )
                {
                    glyph.emplace_back();
                    pointList = &glyph.back();
                }

                pointList->push_back( point );
            }

            i += 2;
        }

        if( pointList )
            pointList->shrink_to_fit();

        // Compute the bounding box of the glyph
        g_newStrokeFontGlyphBoundingBoxes->emplace_back( computeBoundingBox( glyph, glyphWidth ) );
        g_newStrokeFontGlyphs->push_back( glyph );
    }

    m_glyphs = g_newStrokeFontGlyphs;
    m_glyphBoundingBoxes = g_newStrokeFontGlyphBoundingBoxes;
    return true;
}


// Static function:
double STROKE_FONT::GetInterline( double aGlyphHeight )
{
    // Do not add the glyph thickness to the interline.  This makes bold text line-spacing
    // different from normal text, which is poor typography.
    return ( aGlyphHeight * INTERLINE_PITCH_RATIO );
}


BOX2D STROKE_FONT::computeBoundingBox( const GLYPH& aGLYPH, double aGlyphWidth ) const
{
    VECTOR2D min( 0, 0 );
    VECTOR2D max( aGlyphWidth, 0 );

    for( const std::vector<VECTOR2D>& pointList : aGLYPH )
    {
        for( const VECTOR2D& point : pointList )
        {
            min.y = std::min( min.y, point.y );
            max.y = std::max( max.y, point.y );
        }
    }

    return BOX2D( min, max - min );
}


void STROKE_FONT::Draw( const UTF8& aText, const VECTOR2D& aPosition, double aRotationAngle,
                        int markupFlags )
{
    if( aText.empty() )
        return;

    // Context needs to be saved before any transformations
    m_gal->Save();

    m_gal->Translate( aPosition );
    m_gal->Rotate( -aRotationAngle );

    // Single line height
    int lineHeight = KiROUND( GetInterline( m_gal->GetGlyphSize().y ) );
    int lineCount = linesCount( aText );
    const VECTOR2D& glyphSize = m_gal->GetGlyphSize();

    // align the 1st line of text
    switch( m_gal->GetVerticalJustify() )
    {
    case GR_TEXT_VJUSTIFY_TOP:
        m_gal->Translate( VECTOR2D( 0, glyphSize.y ) );
        break;

    case GR_TEXT_VJUSTIFY_CENTER:
        m_gal->Translate( VECTOR2D( 0, glyphSize.y / 2.0 ) );
        break;

    case GR_TEXT_VJUSTIFY_BOTTOM:
        break;

    default:
        break;
    }

    if( lineCount > 1 )
    {
        switch( m_gal->GetVerticalJustify() )
        {
        case GR_TEXT_VJUSTIFY_TOP:
            break;

        case GR_TEXT_VJUSTIFY_CENTER:
            m_gal->Translate( VECTOR2D(0, -( lineCount - 1 ) * lineHeight / 2) );
            break;

        case GR_TEXT_VJUSTIFY_BOTTOM:
            m_gal->Translate( VECTOR2D(0, -( lineCount - 1 ) * lineHeight ) );
            break;
        }
    }

    m_gal->SetIsStroke( true );
    //m_gal->SetIsFill( false );

    if( m_gal->IsFontBold() )
        m_gal->SetLineWidth( m_gal->GetLineWidth() * BOLD_FACTOR );

    // Split multiline strings into separate ones and draw them line by line
    size_t  begin = 0;
    size_t  newlinePos = aText.find( '\n' );

    while( newlinePos != aText.npos )
    {
        size_t length = newlinePos - begin;

        drawSingleLineText( aText.substr( begin, length ), markupFlags );
        m_gal->Translate( VECTOR2D( 0.0, lineHeight ) );

        begin = newlinePos + 1;
        newlinePos = aText.find( '\n', begin );
    }

    // Draw the last (or the only one) line
    if( !aText.empty() )
        drawSingleLineText( aText.substr( begin ), markupFlags );

    m_gal->Restore();
}


void STROKE_FONT::drawSingleLineText( const UTF8& aText, int markupFlags )
{
    double      xOffset;
    double      yOffset;
    VECTOR2D    baseGlyphSize( m_gal->GetGlyphSize() );
    double      overbar_italic_comp = computeOverbarVerticalPosition() * ITALIC_TILT;

    if( m_gal->IsTextMirrored() )
        overbar_italic_comp = -overbar_italic_comp;

    // Compute the text size
    VECTOR2D textSize = computeTextLineSize( aText, markupFlags );
    double half_thickness = m_gal->GetLineWidth()/2;

    // Context needs to be saved before any transformations
    m_gal->Save();

    // First adjust: the text X position is corrected by half_thickness
    // because when the text with thickness is draw, its full size is textSize,
    // but the position of lines is half_thickness to textSize - half_thickness
    // so we must translate the coordinates by half_thickness on the X axis
    // to place the text inside the 0 to textSize X area.
    m_gal->Translate( VECTOR2D( half_thickness, 0 ) );

    // Adjust the text position to the given horizontal justification
    switch( m_gal->GetHorizontalJustify() )
    {
    case GR_TEXT_HJUSTIFY_CENTER:
        m_gal->Translate( VECTOR2D( -textSize.x / 2.0, 0 ) );
        break;

    case GR_TEXT_HJUSTIFY_RIGHT:
        if( !m_gal->IsTextMirrored() )
            m_gal->Translate( VECTOR2D( -textSize.x, 0 ) );
        break;

    case GR_TEXT_HJUSTIFY_LEFT:
        if( m_gal->IsTextMirrored() )
            m_gal->Translate( VECTOR2D( -textSize.x, 0 ) );
        break;

    default:
        break;
    }

    if( m_gal->IsTextMirrored() )
    {
        // In case of mirrored text invert the X scale of points and their X direction
        // (m_glyphSize.x) and start drawing from the position where text normally should end
        // (textSize.x)
        xOffset = textSize.x - m_gal->GetLineWidth();
        baseGlyphSize.x = -baseGlyphSize.x;
    }
    else
    {
        xOffset = 0.0;
    }

    // The overbar is indented inward at the beginning of an italicized section, but
    // must not be indented on subsequent letters to ensure that the bar segments
    // overlap.
    bool     last_had_overbar = false;
    bool     in_overbar = false;
    VECTOR2D glyphSize = baseGlyphSize;

    yOffset = 0;

    for( UTF8::uni_iter chIt = aText.ubegin(), end = aText.uend(); chIt < end; ++chIt )
    {
        // Handle tabs as locked to the nearest 4th column (counting in spaces)
        // The choice of spaces is somewhat arbitrary but sufficient for aligning text
        if( *chIt == '\t' )
        {
            double space = glyphSize.x * m_glyphBoundingBoxes->at( 0 ).GetEnd().x;

            // We align to the 4th column (fmod) but only need to account for 3 of
            // the four spaces here with the extra.  This ensures that we have at
            // least 1 space for the \t character
            double addlSpace = 3.0 * space - std::fmod( xOffset, 4.0 * space );

            // Add the remaining space (between 0 and 3 spaces)
            // The fourth space is added by the 'dd' character
            xOffset += addlSpace;

            glyphSize = baseGlyphSize;
            yOffset = 0;
        }
        else if( *chIt == '~' )
        {
            if( ++chIt == end )
                break;

            if( *chIt == '~' )
            {
                // double ~ is really a ~ so go ahead and process the second one

                // so what's a triple ~?  It could be a real ~ followed by an overbar, or
                // it could be an overbar followed by a real ~.  The old algorithm did the
                // later so we will too....
                auto tempIt = chIt;

                if( ++tempIt < end && *tempIt == '~' )
                {
                    // eat the first two, toggle overbar, and then process the third
                    ++chIt;
                    in_overbar = !in_overbar;
                }
            }
            else
            {
                in_overbar = !in_overbar;
            }
        }
        else if( *chIt == '^' && ( markupFlags & ENABLE_SUPERSCRIPT_MARKUP ) )
        {
            if( ++chIt == end )
                break;

            if( *chIt == '^' )
            {
                // double ^ is really a ^ so go ahead and process the second one
            }
            else
            {
                // single ^ starts a superscript
                glyphSize = baseGlyphSize * 0.8;
                yOffset = -baseGlyphSize.y * 0.3;
            }
        }
        else if( *chIt == '#' && ( markupFlags & ENABLE_SUBSCRIPT_MARKUP ) )
        {
            if( ++chIt == end )
                break;

            if( *chIt == '#' )
            {
                // double # is really a # so go ahead and process the second one
            }
            else
            {
                // single _ starts a subscript
                glyphSize = baseGlyphSize * 0.8;
                yOffset = baseGlyphSize.y * 0.1;
            }
        }
        else if( *chIt == ' ' )
        {
            // space ends a super- or subscript
            glyphSize = baseGlyphSize;
            yOffset = 0;
        }

        // Index into bounding boxes table
        int dd = (signed) *chIt - ' ';

        if( dd >= (int) m_glyphBoundingBoxes->size() || dd < 0 )
        {
            int substitute = *chIt == '\t' ? ' ' : '?';
            dd = substitute - ' ';
        }

        const GLYPH& glyph = m_glyphs->at( dd );
        const BOX2D& bbox  = m_glyphBoundingBoxes->at( dd );

        if( in_overbar )
        {
            double overbar_start_x = xOffset;
            double overbar_start_y = - computeOverbarVerticalPosition();
            double overbar_end_x = xOffset + glyphSize.x * bbox.GetEnd().x;
            double overbar_end_y = overbar_start_y;

            if( !last_had_overbar )
            {
                if( m_gal->IsFontItalic() )
                    overbar_start_x += overbar_italic_comp;

                last_had_overbar = true;
            }

            VECTOR2D startOverbar( overbar_start_x, overbar_start_y );
            VECTOR2D endOverbar( overbar_end_x, overbar_end_y );

            m_gal->DrawLine( startOverbar, endOverbar );
        }
        else
        {
            last_had_overbar = false;
        }

        for( const std::vector<VECTOR2D>& ptList : glyph )
        {
            std::deque<VECTOR2D> ptListScaled;

            for( const VECTOR2D& pt : ptList )
            {
                VECTOR2D scaledPt( pt.x * glyphSize.x + xOffset, pt.y * glyphSize.y + yOffset );

                if( m_gal->IsFontItalic() )
                {
                    // FIXME should be done other way - referring to the lowest Y value of point
                    // because now italic fonts are translated a bit
                    if( m_gal->IsTextMirrored() )
                        scaledPt.x += scaledPt.y * STROKE_FONT::ITALIC_TILT;
                    else
                        scaledPt.x -= scaledPt.y * STROKE_FONT::ITALIC_TILT;
                }

                ptListScaled.push_back( scaledPt );
            }

            m_gal->DrawPolyline( ptListScaled );
        }

        xOffset += glyphSize.x * bbox.GetEnd().x;
    }

    m_gal->Restore();
}


double STROKE_FONT::ComputeOverbarVerticalPosition( double aGlyphHeight, double aGlyphThickness ) const
{
    // Static method.
    // Compute the Y position of the overbar. This is the distance between
    // the text base line and the overbar axis.
    return aGlyphHeight * OVERBAR_POSITION_FACTOR + aGlyphThickness;
}


double STROKE_FONT::computeOverbarVerticalPosition() const
{
    // Compute the Y position of the overbar. This is the distance between
    // the text base line and the overbar axis.
    return ComputeOverbarVerticalPosition( m_gal->GetGlyphSize().y, m_gal->GetLineWidth() );
}


VECTOR2D STROKE_FONT::computeTextLineSize( const UTF8& aText, int aMarkupFlags ) const
{
    return ComputeStringBoundaryLimits( aText, m_gal->GetGlyphSize(), m_gal->GetLineWidth(),
                                        aMarkupFlags );
}


VECTOR2D STROKE_FONT::ComputeStringBoundaryLimits( const UTF8& aText, const VECTOR2D& aGlyphSize,
                                                   double aGlyphThickness, int markupFlags ) const
{
    VECTOR2D string_bbox;
    int line_count = 1;
    double maxX = 0.0, curX = 0.0;

    double curScale = 1.0;
    bool   in_overbar = false;

    for( UTF8::uni_iter it = aText.ubegin(), end = aText.uend(); it < end; ++it )
    {
        if( *it == '\n' )
        {
            curX = 0.0;
            maxX = std::max( maxX, curX );
            ++line_count;
            continue;
        }

        // Handle tabs as locked to the nearest 4th column (counting in spaces)
        // The choice of spaces is somewhat arbitrary but sufficient for aligning text
        if( *it == '\t' )
        {
            double spaces = m_glyphBoundingBoxes->at( 0 ).GetEnd().x;
            double addlSpace = 3.0 * spaces - std::fmod( curX, 4.0 * spaces );

            // Add the remaining space (between 0 and 3 spaces)
            curX += addlSpace;

            // Tab ends a super- or subscript
            curScale = 1.0;
        }
        else if( *it == '~' )
        {
            if( ++it == end )
                break;

            if( *it == '~' )
            {
                // double ~ is really a ~ so go ahead and process the second one

                // so what's a triple ~?  It could be a real ~ followed by an overbar, or
                // it could be an overbar followed by a real ~.  The old algorithm did the
                // later so we will too....
                auto tempIt = it;

                if( ++tempIt < end && *tempIt == '~' )
                {
                    // eat the first two, toggle overbar, and then process the third
                    ++it;
                    in_overbar = !in_overbar;
                }
            }
            else
            {
                // single ~ toggles overbar
                in_overbar = !in_overbar;
            }
        }
        else if( *it == '^' && ( markupFlags & ENABLE_SUPERSCRIPT_MARKUP ) )
        {
            if( ++it == end )
                break;

            if( *it == '^' )
            {
                // double ^ is really a ^ so go ahead and process the second one
            }
            else
            {
                // single ^ starts a superscript
                curScale = 0.8;
            }
        }
        else if( *it == '#' && ( markupFlags & ENABLE_SUBSCRIPT_MARKUP ) )
        {
            if( ++it == end )
                break;

            if( *it == '#' )
            {
                // double # is really a # so go ahead and process the second one
            }
            else
            {
                // single _ starts a subscript
                curScale = 0.8;
            }
        }
        else if( *it == ' ' )
        {
            // space ends a super- or subscript
            curScale = 1.0;
        }

        // Index in the bounding boxes table
        int dd = (signed) *it - ' ';

        if( dd >= (int) m_glyphBoundingBoxes->size() || dd < 0 )
        {
            int substitute = *it == '\t' ? ' ' : '?';
            dd = substitute - ' ';
        }

        const BOX2D& box = m_glyphBoundingBoxes->at( dd );
        curX += box.GetEnd().x * curScale;
    }

    string_bbox.x = std::max( maxX, curX ) * aGlyphSize.x;
    string_bbox.x += aGlyphThickness;
    string_bbox.y = line_count * GetInterline( aGlyphSize.y );

    // For italic correction, take in account italic tilt
    if( m_gal->IsFontItalic() )
        string_bbox.x += string_bbox.y * STROKE_FONT::ITALIC_TILT;

    return string_bbox;
}

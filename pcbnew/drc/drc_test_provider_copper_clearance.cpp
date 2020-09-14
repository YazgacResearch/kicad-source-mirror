/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004-2020 KiCad Developers.
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

#include <common.h>
#include <class_board.h>
#include <class_drawsegment.h>
#include <class_pad.h>

#include <geometry/polygon_test_point_inside.h>
#include <geometry/seg.h>
#include <geometry/shape_poly_set.h>
#include <geometry/shape_rect.h>
#include <geometry/shape_segment.h>

#include <drc/drc_engine.h>
#include <drc/drc.h>
#include <drc/drc_item.h>
#include <drc/drc_rule.h>
#include <drc/drc_test_provider_clearance_base.h>

/*
    Copper clearance test. Checks all copper items (pads, vias, tracks, drawings, zones) for their electrical clearance.
    Errors generated:
    - DRCE_CLEARANCE
    - DRCE_TRACKS_CROSSING
    - DRCE_ZONES_INTERSECT
    - DRCE_SHORTING_ITEMS

    TODO: improve zone clearance check (super slow)
*/

class DRC_TEST_PROVIDER_COPPER_CLEARANCE : public DRC_TEST_PROVIDER_CLEARANCE_BASE
{
public:
    DRC_TEST_PROVIDER_COPPER_CLEARANCE () :
            DRC_TEST_PROVIDER_CLEARANCE_BASE()
    {
    }

    virtual ~DRC_TEST_PROVIDER_COPPER_CLEARANCE()
    {
    }

    virtual bool Run() override;

    virtual const wxString GetName() const override
    {
        return "clearance";
    };

    virtual const wxString GetDescription() const override
    {
        return "Tests copper item clearance";
    }

    virtual std::set<DRC_CONSTRAINT_TYPE_T> GetConstraintTypes() const override;

    int GetNumPhases() const override;

private:
    void testPadClearances();

    void testTrackClearances();

    void testCopperTextAndGraphics();

    void testZones();

    void testCopperDrawItem( BOARD_ITEM* aItem );

    void doTrackDrc( TRACK* aRefSeg, PCB_LAYER_ID aLayer, TRACKS::iterator aStartIt,
                     TRACKS::iterator aEndIt );

    void doPadToPadsDrc( D_PAD* aRefPad, D_PAD** aStart, D_PAD** aEnd, int x_limit );
};


bool DRC_TEST_PROVIDER_COPPER_CLEARANCE::Run()
{
    m_board = m_drcEngine->GetBoard();
    DRC_CONSTRAINT worstClearanceConstraint;

    if( m_drcEngine->QueryWorstConstraint( DRC_CONSTRAINT_TYPE_CLEARANCE,
                                           worstClearanceConstraint, DRCCQ_LARGEST_MINIMUM ) )
    {
        m_largestClearance = worstClearanceConstraint.GetValue().Min();
    }
    else
    {
        reportAux( "No Clearance constraints found..." );
        return false;
    }

    reportAux( "Worst clearance : %d nm", m_largestClearance );

    reportStage( _( "Pad clerances..." ));
    testPadClearances();

    reportStage( _( "Track/via clerances..." ));
    testTrackClearances();

    reportStage( _( "Copper drawing/text clerances..." ));
    testCopperTextAndGraphics();

    reportStage( _( "Zone clearances..." ));
    testZones();

    reportRuleStatistics();

    return true;
}

void DRC_TEST_PROVIDER_COPPER_CLEARANCE::testCopperTextAndGraphics()
{
    // Test copper items for clearance violations with vias, tracks and pads

    for( BOARD_ITEM* brdItem : m_board->Drawings() )
    {
        if( IsCopperLayer( brdItem->GetLayer() ) )
            testCopperDrawItem( brdItem );
    }

    for( MODULE* module : m_board->Modules() )
    {
        TEXTE_MODULE& ref = module->Reference();
        TEXTE_MODULE& val = module->Value();

        if( ref.IsVisible() && IsCopperLayer( ref.GetLayer() ) )
            testCopperDrawItem( &ref );

        if( val.IsVisible() && IsCopperLayer( val.GetLayer() ) )
            testCopperDrawItem( &val );

        if( module->IsNetTie() )
            continue;

        for( BOARD_ITEM* item : module->GraphicalItems() )
        {
            if( IsCopperLayer( item->GetLayer() ) )
            {
                if( item->Type() == PCB_MODULE_TEXT_T && ( (TEXTE_MODULE*) item )->IsVisible() )
                    testCopperDrawItem( item );
                else if( item->Type() == PCB_MODULE_EDGE_T )
                    testCopperDrawItem( item );
            }
        }
    }
}


void DRC_TEST_PROVIDER_COPPER_CLEARANCE::testCopperDrawItem( BOARD_ITEM* aItem )
{
    EDA_RECT               bbox;
    std::shared_ptr<SHAPE> itemShape;
    DRAWSEGMENT*           drawItem = dynamic_cast<DRAWSEGMENT*>( aItem );
    EDA_TEXT*              textItem = dynamic_cast<EDA_TEXT*>( aItem );
    PCB_LAYER_ID           layer = aItem->GetLayer();

    if( drawItem )
    {
        bbox = drawItem->GetBoundingBox();
        itemShape = drawItem->GetEffectiveShape();
    }
    else if( textItem )
    {
        bbox = textItem->GetTextBox();
        itemShape = textItem->GetEffectiveTextShape();
    }
    else
    {
        wxFAIL_MSG( "unknown item type in testCopperDrawItem()" );
        return;
    }

    SHAPE_RECT bboxShape( bbox.GetX(), bbox.GetY(), bbox.GetWidth(), bbox.GetHeight() );

    // Test tracks and vias
    for( TRACK* track : m_board->Tracks() )
    {
        if( !track->IsOnLayer( aItem->GetLayer() ) )
            continue;

        auto    constraint = m_drcEngine->EvalRulesForItems( DRC_CONSTRAINT_TYPE_CLEARANCE,
                                                             aItem, track, layer );
        int     minClearance = constraint.GetValue().Min();
        int     actual = INT_MAX;
        wxPoint pos;

        accountCheck( constraint );

        SHAPE_SEGMENT trackSeg( track->GetStart(), track->GetEnd(), track->GetWidth() );

        // Fast test to detect a track segment candidate inside the text bounding box
        if( !bboxShape.Collide( &trackSeg, 0 ) )
            continue;

        if( !itemShape->Collide( &trackSeg, minClearance, &actual ) )
            continue;

        pos = (wxPoint) itemShape->Centre();

        if( actual < INT_MAX )
        {
            std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_CLEARANCE );

            m_msg.Printf( drcItem->GetErrorText() + _( " (%s clearance %s; actual %s)" ),
                          constraint.GetName(),
                          MessageTextFromValue( userUnits(), minClearance, true ),
                          MessageTextFromValue( userUnits(), std::max( 0, actual ), true ) );

            drcItem->SetErrorMessage( m_msg );
            drcItem->SetItems( track, aItem );
            drcItem->SetViolatingRule( constraint.GetParentRule() );

            reportViolation( drcItem, pos );
        }
    }

    // Test pads
    for( D_PAD* pad : m_board->GetPads() )
    {
        if( !pad->IsOnLayer( layer ) )
            continue;

        // Graphic items are allowed to act as net-ties within their own footprint
        if( drawItem && pad->GetParent() == drawItem->GetParent() )
            continue;

        auto constraint = m_drcEngine->EvalRulesForItems( DRC_CONSTRAINT_TYPE_CLEARANCE,
                                                          aItem, pad, layer );
        int  minClearance = constraint.GetValue().Min();
        int  actual;

        accountCheck( constraint );


        // Fast test to detect a pad candidate inside the text bounding box
        // Finer test (time consuming) is made only for pads near the text.
        int bb_radius = pad->GetBoundingRadius() + minClearance;

        if( !bboxShape.Collide( SEG( pad->GetPosition(), pad->GetPosition() ), bb_radius ) )
            continue;

        if( !pad->GetEffectiveShape()->Collide( itemShape.get(), minClearance, &actual ) )
            continue;

        std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_CLEARANCE );

        m_msg.Printf( drcItem->GetErrorText() + _( " (%s clearance %s; actual %s)" ),
                      constraint.GetName(),
                      MessageTextFromValue( userUnits(), minClearance, true ),
                      MessageTextFromValue( userUnits(), actual, true ) );

        drcItem->SetErrorMessage( m_msg );
        drcItem->SetItems( pad, aItem );
        drcItem->SetViolatingRule( constraint.GetParentRule() );

        reportViolation( drcItem, pad->GetPosition());
    }
}


void DRC_TEST_PROVIDER_COPPER_CLEARANCE::testTrackClearances()
{
    const int delta = 500;  // This is the number of tests between 2 calls to the progress bar
    int       count = m_board->Tracks().size();

    reportProgress( 0.0 );
    reportAux( "Testing %d tracks...", count );

    int ii = 0;

    for( auto seg_it = m_board->Tracks().begin(); seg_it != m_board->Tracks().end(); seg_it++ )
    {
        if( (ii % delta) == 0)
            reportProgress((double) ii / (double) m_board->Tracks().size());

        ii++;

        // Test segment against tracks and pads, optionally against copper zones
        for( PCB_LAYER_ID layer : (*seg_it)->GetLayerSet().Seq() )
        {
            doTrackDrc( *seg_it, layer, seg_it + 1, m_board->Tracks().end() );
        }
    }
}

void DRC_TEST_PROVIDER_COPPER_CLEARANCE::doTrackDrc( TRACK* aRefSeg, PCB_LAYER_ID aLayer,
                                                     TRACKS::iterator aStartIt,
                                                     TRACKS::iterator aEndIt )
{
    BOARD_DESIGN_SETTINGS&  bds = m_board->GetDesignSettings();

    SHAPE_SEGMENT refSeg( aRefSeg->GetStart(), aRefSeg->GetEnd(), aRefSeg->GetWidth() );
    EDA_RECT      refSegBB = aRefSeg->GetBoundingBox();
    int           refSegWidth = aRefSeg->GetWidth();

    /******************************************/
    /* Phase 1 : test DRC track to pads :     */
    /******************************************/

    // Compute the min distance to pads
    for( MODULE* mod : m_board->Modules() )
    {
        if( m_drcEngine->IsErrorLimitExceeded( DRCE_CLEARANCE ) )
            break;

        // Don't preflight at the module level.  Getting a module's bounding box goes
        // through all its pads anyway (so it's no faster), and also all its drawings
        // (so it's in fact slower).

        for( D_PAD* pad : mod->Pads() )
        {
            if( m_drcEngine->IsErrorLimitExceeded( DRCE_CLEARANCE ) )
                break;

            // Preflight based on bounding boxes.
            EDA_RECT inflatedBB = refSegBB;
            inflatedBB.Inflate( pad->GetBoundingRadius() + m_largestClearance );

            if( !inflatedBB.Contains( pad->GetPosition() ) )
                continue;

            /// Skip checking pad copper when it has been removed
            if( !pad->IsOnLayer( aLayer ) )
                continue;

            // No need to check pads with the same net as the refSeg.
            if( pad->GetNetCode() && aRefSeg->GetNetCode() == pad->GetNetCode() )
                continue;

            auto constraint = m_drcEngine->EvalRulesForItems( DRC_CONSTRAINT_TYPE_CLEARANCE,
                                                              aRefSeg, pad, aLayer );
            int  minClearance = constraint.GetValue().Min();
            int  actual;

            accountCheck( constraint );

            const std::shared_ptr<SHAPE>& padShape = pad->GetEffectiveShape();

            if( padShape->Collide( &refSeg, minClearance - bds.GetDRCEpsilon(), &actual ) )
            {
                std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_CLEARANCE );

                m_msg.Printf( drcItem->GetErrorText() + _( " (%s clearance %s; actual %s)" ),
                              constraint.GetName(),
                              MessageTextFromValue( userUnits(), minClearance, true ),
                              MessageTextFromValue( userUnits(), actual, true ) );

                drcItem->SetErrorMessage( m_msg );
                drcItem->SetItems( aRefSeg, pad );
                drcItem->SetViolatingRule( constraint.GetParentRule() );

                reportViolation( drcItem, pad->GetPosition());
            }
        }
    }

    /***********************************************/
    /* Phase 2: test DRC with other track segments */
    /***********************************************/

    // Test the reference segment with other track segments
    for( auto it = aStartIt; it != aEndIt; it++ )
    {
        if( m_drcEngine->IsErrorLimitExceeded( DRCE_CLEARANCE ) )
            break;

        TRACK* track = *it;

        // No problem if segments have the same net code:
        if( aRefSeg->GetNetCode() == track->GetNetCode() )
            continue;

        if( !track->GetLayerSet().test( aLayer ) )
            continue;

        // Preflight based on worst-case inflated bounding boxes:
        EDA_RECT trackBB = track->GetBoundingBox();
        trackBB.Inflate( m_largestClearance );

        if( !trackBB.Intersects( refSegBB ) )
            continue;

        auto          constraint = m_drcEngine->EvalRulesForItems( DRC_CONSTRAINT_TYPE_CLEARANCE,
                                                                   aRefSeg, track, aLayer );
        int           minClearance = constraint.GetValue().Min();
        int           actual;
        SHAPE_SEGMENT trackSeg( track->GetStart(), track->GetEnd(), track->GetWidth() );

        accountCheck( constraint );

        /// Check to see if the via has a pad on this layer
        if( track->Type() == PCB_VIA_T )
        {
            VIA* via = static_cast<VIA*>( track );

            if( !via->IsPadOnLayer( aLayer ) )
                trackSeg.SetWidth( via->GetDrillValue() );
        }

        // Check two tracks crossing first as it reports a DRCE without distances
        if( OPT_VECTOR2I intersection = refSeg.GetSeg().Intersect( trackSeg.GetSeg() ) )
        {
            std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_TRACKS_CROSSING );
            drcItem->SetItems( aRefSeg, track );
            drcItem->SetViolatingRule( constraint.GetParentRule() );

            reportViolation( drcItem, (wxPoint) intersection.get());
        }
        else if( refSeg.Collide( &trackSeg, minClearance - bds.GetDRCEpsilon(), &actual ) )
        {
            wxPoint   pos = getLocation( aRefSeg, trackSeg.GetSeg() );
            std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_CLEARANCE );

            m_msg.Printf( drcItem->GetErrorText() + _( " (%s clearance %s; actual %s)" ),
                          constraint.GetName(),
                          MessageTextFromValue( userUnits(), minClearance, true ),
                          MessageTextFromValue( userUnits(), actual, true ) );

            drcItem->SetErrorMessage( m_msg );
            drcItem->SetItems( aRefSeg, track );
            drcItem->SetViolatingRule( constraint.GetParentRule() );

            reportViolation( drcItem, pos );

            if( !m_drcEngine->GetReportAllTrackErrors() )
                break;
        }
    }

    /***************************************/
    /* Phase 3: test DRC with copper zones */
    /***************************************/
    // Can be *very* time consuming.

    if( m_drcEngine->GetTestTracksAgainstZones() )
    {
        SEG testSeg( aRefSeg->GetStart(), aRefSeg->GetEnd() );

        for( ZONE_CONTAINER* zone : m_board->Zones() )
        {
            if( m_drcEngine->IsErrorLimitExceeded( DRCE_CLEARANCE ) )
                break;

            if( !zone->GetLayerSet().test( aLayer ) || zone->GetIsKeepout() )
                continue;

            if( zone->GetNetCode() && zone->GetNetCode() == aRefSeg->GetNetCode() )
                continue;

            if( zone->GetFilledPolysList( aLayer ).IsEmpty() )
                continue;

            auto constraint = m_drcEngine->EvalRulesForItems( DRC_CONSTRAINT_TYPE_CLEARANCE,
                                                              aRefSeg, zone, aLayer );
            int  minClearance = constraint.GetValue().Min();
            int  halfWidth = refSegWidth / 2;
            int  allowedDist  = minClearance + halfWidth - bds.GetDRCEpsilon();
            int  actual;

            accountCheck( constraint );

            if( zone->GetFilledPolysList( aLayer ).Collide( testSeg, allowedDist, &actual ) )
            {
                actual = std::max( 0, actual - halfWidth );
                std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_CLEARANCE );

                m_msg.Printf( drcItem->GetErrorText() + _( " (%s clearance %s; actual %s)" ),
                              constraint.GetName(),
                              MessageTextFromValue( userUnits(), minClearance, true ),
                              MessageTextFromValue( userUnits(), actual, true ) );

                drcItem->SetErrorMessage( m_msg );
                drcItem->SetItems( aRefSeg, zone );
                drcItem->SetViolatingRule( constraint.GetParentRule() );

                reportViolation( drcItem, getLocation( aLayer, aRefSeg, zone ));
            }
        }
    }
}


void DRC_TEST_PROVIDER_COPPER_CLEARANCE::testPadClearances( )
{
    std::vector<D_PAD*> sortedPads;

    m_board->GetSortedPadListByXthenYCoord( sortedPads );

    reportAux( "Testing %d pads...", sortedPads.size());

    if( sortedPads.empty() )
        return;

    // find the max size of the pads (used to stop the pad-to-pad tests)
    int max_size = 0;

    for( D_PAD* pad : sortedPads )
    {
        // GetBoundingRadius() is the radius of the minimum sized circle fully containing the pad
        int radius = pad->GetBoundingRadius();

        if( radius > max_size )
            max_size = radius;
    }

    // Better to be fast than accurate; this keeps us from having to look up / calculate the
    // actual clearances
    max_size += m_largestClearance;

    // Upper limit of pad list (limit not included)
    D_PAD** listEnd = &sortedPads[0] + sortedPads.size();

    int ii = 0;

    // Test the pads
    for( D_PAD* pad : sortedPads )
    {
        if( ii % 100 == 0 )
            reportProgress((double) ii / (double) sortedPads.size());

        ii++;
        int x_limit = pad->GetPosition().x + pad->GetBoundingRadius() + max_size;

        doPadToPadsDrc( pad, &pad, listEnd, x_limit );
    }
}

void DRC_TEST_PROVIDER_COPPER_CLEARANCE::doPadToPadsDrc( D_PAD* aRefPad, D_PAD** aStart,
                                                         D_PAD** aEnd, int x_limit )
{
    const static LSET all_cu = LSET::AllCuMask();
    const BOARD_DESIGN_SETTINGS& bds = m_board->GetDesignSettings();

    LSET layerMask = aRefPad->GetLayerSet() & all_cu;

    for( D_PAD** pad_list = aStart;  pad_list<aEnd;  ++pad_list )
    {
        bool exceedClearance = m_drcEngine->IsErrorLimitExceeded( DRCE_CLEARANCE );
        bool exceedShorting = m_drcEngine->IsErrorLimitExceeded( DRCE_CLEARANCE );

        if( exceedClearance && exceedShorting )
            return;

        D_PAD* pad = *pad_list;

        if( pad == aRefPad )
            continue;

        // We can stop the test when pad->GetPosition().x > x_limit
        // because the list is sorted by X values
        if( pad->GetPosition().x > x_limit )
            break;

        // The pad must be in a net (i.e pt_pad->GetNet() != 0 ),
        // But no problem if pads have the same netcode (same net)
        if( pad->GetNetCode() && ( aRefPad->GetNetCode() == pad->GetNetCode() ) )
            continue;

        // If pads are equivalent (ie: from the same footprint with the same pad number)...
        if( pad->GetParent() == aRefPad->GetParent() && pad->PadNameEqual( aRefPad ) )
        {
            // ...and have nets, then they must be the same net
            if( pad->GetNetCode() && aRefPad->GetNetCode()
                    && pad->GetNetCode() != aRefPad->GetNetCode()
                    && !exceedShorting )
            {
                std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_SHORTING_ITEMS );

                m_msg.Printf( drcItem->GetErrorText() + _( " (nets %s and %s)" ),
                              pad->GetNetname(), aRefPad->GetNetname() );

                drcItem->SetErrorMessage( m_msg );
                drcItem->SetItems( pad, aRefPad );

                reportViolation( drcItem, aRefPad->GetPosition());
            }

            continue;
        }

        // if either pad has no drill and is only on technical layers, not a clearance violation
        if( ( ( pad->GetLayerSet() & layerMask ) == 0 && !pad->GetDrillSize().x ) ||
            ( ( aRefPad->GetLayerSet() & layerMask ) == 0 && !aRefPad->GetDrillSize().x ) )
        {
            continue;
        }

        for( PCB_LAYER_ID layer : aRefPad->GetLayerSet().Seq() )
        {
            if( exceedClearance )
                break;

            auto constraint = m_drcEngine->EvalRulesForItems( DRC_CONSTRAINT_TYPE_CLEARANCE,
                                                              aRefPad, pad, layer );
            int  minClearance = constraint.GetValue().Min();
            int  clearanceAllowed = minClearance - bds.GetDRCEpsilon();
            int  actual;

            accountCheck( constraint );

            std::shared_ptr<SHAPE> refPadShape = aRefPad->GetEffectiveShape();

            if( refPadShape->Collide( pad->GetEffectiveShape().get(), clearanceAllowed, &actual ) )
            {
                std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_CLEARANCE );

                m_msg.Printf( drcItem->GetErrorText() + _( " (%s clearance %s; actual %s)" ),
                              constraint.GetName(),
                              MessageTextFromValue( userUnits(), minClearance, true ),
                              MessageTextFromValue( userUnits(), actual, true ) );

                drcItem->SetErrorMessage( m_msg );
                drcItem->SetItems( aRefPad, pad );
                drcItem->SetViolatingRule( constraint.GetParentRule() );

                reportViolation( drcItem, aRefPad->GetPosition());
                break;
            }
        }
    }
}


void DRC_TEST_PROVIDER_COPPER_CLEARANCE::testZones()
{
    // Test copper areas for valid netcodes -> fixme, goes to connectivity checks

    std::vector<SHAPE_POLY_SET> smoothed_polys;
    smoothed_polys.resize( m_board->GetAreaCount() );

    for( int ii = 0; ii < m_board->GetAreaCount(); ii++ )
    {
        ZONE_CONTAINER* zoneRef = m_board->GetArea( ii );

        zoneRef->BuildSmoothedPoly( smoothed_polys[ii], zoneRef->GetLayer() );
    }

    // iterate through all areas
    for( int ia = 0; ia < m_board->GetAreaCount(); ia++ )
    {
        ZONE_CONTAINER* zoneRef = m_board->GetArea( ia );

        if( !zoneRef->IsOnCopperLayer() )
            continue;

        // If we are testing a single zone, then iterate through all other zones
        // Otherwise, we have already tested the zone combination
        for( int ia2 = ia + 1; ia2 < m_board->GetAreaCount(); ia2++ )
        {
            ZONE_CONTAINER* zoneToTest = m_board->GetArea( ia2 );

            if( zoneRef == zoneToTest )
                continue;

            // test for same layer
            if( zoneRef->GetLayer() != zoneToTest->GetLayer() )
                continue;

            // Test for same net
            if( zoneRef->GetNetCode() == zoneToTest->GetNetCode() && zoneRef->GetNetCode() >= 0 )
                continue;

            // test for different priorities
            if( zoneRef->GetPriority() != zoneToTest->GetPriority() )
                continue;

            // test for different types
            if( zoneRef->GetIsKeepout() != zoneToTest->GetIsKeepout() )
                continue;

            // Examine a candidate zone: compare zoneToTest to zoneRef

            // Get clearance used in zone to zone test.
            auto constraint = m_drcEngine->EvalRulesForItems( DRC_CONSTRAINT_TYPE_CLEARANCE,
                                                              zoneRef, zoneToTest );
            int  zone2zoneClearance = constraint.GetValue().Min();

            accountCheck( constraint );

            // Keepout areas have no clearance, so set zone2zoneClearance to 1
            // ( zone2zoneClearance = 0  can create problems in test functions)
            if( zoneRef->GetIsKeepout() ) // fixme: really?
                zone2zoneClearance = 1;

            // test for some corners of zoneRef inside zoneToTest
            for( auto iterator = smoothed_polys[ia].IterateWithHoles(); iterator; iterator++ )
            {
                VECTOR2I currentVertex = *iterator;
                wxPoint pt( currentVertex.x, currentVertex.y );

                if( smoothed_polys[ia2].Contains( currentVertex ) )
                {
                    std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_ZONES_INTERSECT );
                    drcItem->SetItems( zoneRef, zoneToTest );
                    drcItem->SetViolatingRule( constraint.GetParentRule() );

                    reportViolation( drcItem, pt );
                }
            }

            // test for some corners of zoneToTest inside zoneRef
            for( auto iterator = smoothed_polys[ia2].IterateWithHoles(); iterator; iterator++ )
            {
                VECTOR2I currentVertex = *iterator;
                wxPoint pt( currentVertex.x, currentVertex.y );

                if( smoothed_polys[ia].Contains( currentVertex ) )
                {
                    std::shared_ptr<DRC_ITEM> drcItem = DRC_ITEM::Create( DRCE_ZONES_INTERSECT );
                    drcItem->SetItems( zoneToTest, zoneRef );
                    drcItem->SetViolatingRule( constraint.GetParentRule() );

                    reportViolation( drcItem, pt );
                }
            }

            // Iterate through all the segments of refSmoothedPoly
            std::map<wxPoint, int> conflictPoints;

            for( auto refIt = smoothed_polys[ia].IterateSegmentsWithHoles(); refIt; refIt++ )
            {
                // Build ref segment
                SEG refSegment = *refIt;

                // Iterate through all the segments in smoothed_polys[ia2]
                for( auto testIt = smoothed_polys[ia2].IterateSegmentsWithHoles(); testIt; testIt++ )
                {
                    // Build test segment
                    SEG testSegment = *testIt;
                    wxPoint pt;

                    int ax1, ay1, ax2, ay2;
                    ax1 = refSegment.A.x;
                    ay1 = refSegment.A.y;
                    ax2 = refSegment.B.x;
                    ay2 = refSegment.B.y;

                    int bx1, by1, bx2, by2;
                    bx1 = testSegment.A.x;
                    by1 = testSegment.A.y;
                    bx2 = testSegment.B.x;
                    by2 = testSegment.B.y;

                    int d = GetClearanceBetweenSegments( bx1, by1, bx2, by2,
                                                         0,
                                                         ax1, ay1, ax2, ay2,
                                                         0,
                                                         zone2zoneClearance,
                                                         &pt.x, &pt.y );

                    if( d < zone2zoneClearance )
                    {
                        if( conflictPoints.count( pt ) )
                            conflictPoints[ pt ] = std::min( conflictPoints[ pt ], d );
                        else
                            conflictPoints[ pt ] = d;
                    }
                }
            }

            for( const std::pair<const wxPoint, int>& conflict : conflictPoints )
            {
                int       actual = conflict.second;
                std::shared_ptr<DRC_ITEM> drcItem;

                if( actual <= 0 )
                {
                    drcItem = DRC_ITEM::Create( DRCE_ZONES_INTERSECT );
                }
                else
                {
                    drcItem = DRC_ITEM::Create( DRCE_CLEARANCE );

                    m_msg.Printf( drcItem->GetErrorText() + _( " (%s clearance %s; actual %s)" ),
                                  constraint.GetName(),
                                  MessageTextFromValue( userUnits(), zone2zoneClearance, true ),
                                  MessageTextFromValue( userUnits(), conflict.second, true ) );

                    drcItem->SetErrorMessage( m_msg );
                }

                drcItem->SetItems( zoneRef, zoneToTest );
                drcItem->SetViolatingRule( constraint.GetParentRule() );

                reportViolation( drcItem, conflict.first );
            }
        }
    }
}


int DRC_TEST_PROVIDER_COPPER_CLEARANCE::GetNumPhases() const
{
    return 4;
}


std::set<DRC_CONSTRAINT_TYPE_T> DRC_TEST_PROVIDER_COPPER_CLEARANCE::GetConstraintTypes() const
{
    return { DRC_CONSTRAINT_TYPE_CLEARANCE };
}


namespace detail
{
    static DRC_REGISTER_TEST_PROVIDER<DRC_TEST_PROVIDER_COPPER_CLEARANCE> dummy;
}

/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2014-2017 CERN
 * Copyright (C) 2014-2020 KiCad Developers, see AUTHORS.txt for contributors.
 * @author Tomasz Włostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
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

#include <thread>
#include <algorithm>
#include <future>

#include <advanced_config.h>
#include <class_board.h>
#include <class_zone.h>
#include <class_module.h>
#include <class_edge_mod.h>
#include <class_drawsegment.h>
#include <class_pcb_text.h>
#include <class_pcb_target.h>
#include <class_track.h>
#include <connectivity/connectivity_data.h>
#include <convert_basic_shapes_to_polygon.h>
#include <board_commit.h>
#include <widgets/progress_reporter.h>
#include <geometry/shape_poly_set.h>
#include <geometry/shape_file_io.h>
#include <geometry/convex_hull.h>
#include <geometry/geometry_utils.h>
#include <confirm.h>
#include <convert_to_biu.h>
#include <math/util.h>      // for KiROUND
#include <convert_basic_shapes_to_polygon.h>

#include "zone_filler.h"

class PROGRESS_REPORTER_HIDER
{
public:
    PROGRESS_REPORTER_HIDER( WX_PROGRESS_REPORTER* aReporter )
    {
        m_reporter = aReporter;

        if( aReporter )
            aReporter->Hide();
    }

    ~PROGRESS_REPORTER_HIDER()
    {
        if( m_reporter )
            m_reporter->Show();
    }

private:
    WX_PROGRESS_REPORTER* m_reporter;
};


static const double s_RoundPadThermalSpokeAngle = 450;
static const bool s_DumpZonesWhenFilling = false;


ZONE_FILLER::ZONE_FILLER(  BOARD* aBoard, COMMIT* aCommit ) :
    m_board( aBoard ),
    m_brdOutlinesValid( false ),
    m_commit( aCommit ),
    m_progressReporter( nullptr ),
    m_high_def( 9 ),
    m_low_def( 6 )
{
}


ZONE_FILLER::~ZONE_FILLER()
{
}


void ZONE_FILLER::InstallNewProgressReporter( wxWindow* aParent, const wxString& aTitle,
                                              int aNumPhases )
{
    m_uniqueReporter = std::make_unique<WX_PROGRESS_REPORTER>( aParent, aTitle, aNumPhases );
    m_progressReporter = m_uniqueReporter.get();
}


bool ZONE_FILLER::Fill( const std::vector<ZONE_CONTAINER*>& aZones, bool aCheck )
{
    std::vector<std::pair<ZONE_CONTAINER*, PCB_LAYER_ID>> toFill;
    std::vector<CN_ZONE_ISOLATED_ISLAND_LIST> islandsList;

    auto connectivity = m_board->GetConnectivity();
    bool filledPolyWithOutline = not m_board->GetDesignSettings().m_ZoneUseNoOutlineInFill;

    std::unique_lock<std::mutex> lock( connectivity->GetLock(), std::try_to_lock );

    if( !lock )
        return false;

    if( m_progressReporter )
    {
        m_progressReporter->Report( aCheck ? _( "Checking zone fills..." )
                                           : _( "Building zone fills..." ) );
        m_progressReporter->SetMaxProgress( aZones.size() );
    }

    // The board outlines is used to clip solid areas inside the board (when outlines are valid)
    m_boardOutline.RemoveAllContours();
    m_brdOutlinesValid = m_board->GetBoardPolygonOutlines( m_boardOutline );

    // Update the bounding box shape caches in the pads to prevent multi-threaded rebuilds
    for( auto module : m_board->Modules() )
    {
        for( auto pad : module->Pads() )
        {
            if( pad->IsDirty() )
                pad->BuildEffectiveShapes();
        }
    }

    for( ZONE_CONTAINER* zone : aZones )
    {
        // Keepout zones are not filled
        if( zone->GetIsKeepout() )
            continue;

        m_commit->Modify( zone );

        // calculate the hash value for filled areas. it will be used later
        // to know if the current filled areas are up to date
        for( PCB_LAYER_ID layer : zone->GetLayerSet().Seq() )
        {
            zone->BuildHashValue( layer );

            // Add the zone to the list of zones to test or refill
            toFill.emplace_back( std::make_pair( zone, layer ) );
        }

        islandsList.emplace_back( CN_ZONE_ISOLATED_ISLAND_LIST( zone ) );

        // Remove existing fill first to prevent drawing invalid polygons
        // on some platforms
        zone->UnFill();
    }

    std::atomic<size_t> nextItem( 0 );
    size_t parallelThreadCount = std::min<size_t>( std::thread::hardware_concurrency(),
                                                   aZones.size() );
    std::vector<std::future<size_t>> returns( parallelThreadCount );

    auto fill_lambda =
            [&]( PROGRESS_REPORTER* aReporter ) -> size_t
            {
                size_t num = 0;

                for( size_t i = nextItem++; i < toFill.size(); i = nextItem++ )
                {
                    PCB_LAYER_ID    layer = toFill[i].second;
                    ZONE_CONTAINER* zone  = toFill[i].first;

                    zone->SetFilledPolysUseThickness( filledPolyWithOutline );

                    SHAPE_POLY_SET rawPolys, finalPolys;
                    fillSingleZone( zone, layer, rawPolys, finalPolys );

                    std::unique_lock<std::mutex> zoneLock( zone->GetLock() );

                    zone->SetRawPolysList( layer, rawPolys );
                    zone->SetFilledPolysList( layer, finalPolys );
                    zone->SetIsFilled( true );

                    if( m_progressReporter )
                    {
                        m_progressReporter->AdvanceProgress();

                        if( m_progressReporter->IsCancelled() )
                            break;
                    }

                    num++;
                }

                return num;
            };

    if( parallelThreadCount <= 1 )
        fill_lambda( m_progressReporter );
    else
    {
        for( size_t ii = 0; ii < parallelThreadCount; ++ii )
            returns[ii] = std::async( std::launch::async, fill_lambda, m_progressReporter );

        for( size_t ii = 0; ii < parallelThreadCount; ++ii )
        {
            // Here we balance returns with a 100ms timeout to allow UI updating
            std::future_status status;
            do
            {
                if( m_progressReporter )
                    m_progressReporter->KeepRefreshing();

                status = returns[ii].wait_for( std::chrono::milliseconds( 100 ) );
            } while( status != std::future_status::ready );
        }
    }

    // Now update the connectivity to check for copper islands
    if( m_progressReporter )
    {
        if( m_progressReporter->IsCancelled() )
            return false;

        m_progressReporter->AdvancePhase();
        m_progressReporter->Report( _( "Removing insulated copper islands..." ) );
        m_progressReporter->KeepRefreshing();
    }

    connectivity->SetProgressReporter( m_progressReporter );
    connectivity->FindIsolatedCopperIslands( islandsList );
    connectivity->SetProgressReporter( nullptr );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return false;

    // Now remove insulated copper islands and islands outside the board edge
    bool outOfDate = false;

    for( auto& zone : islandsList )
    {
        for( PCB_LAYER_ID layer : zone.m_zone->GetLayerSet().Seq() )
        {
            if( !zone.m_islands.count( layer ) )
                continue;

            std::vector<int>& islands = zone.m_islands.at( layer );

            std::sort( islands.begin(), islands.end(), std::greater<int>() );
            SHAPE_POLY_SET poly = zone.m_zone->GetFilledPolysList( layer );

            long long int       minArea = zone.m_zone->GetMinIslandArea();
            ISLAND_REMOVAL_MODE mode    = zone.m_zone->GetIslandRemovalMode();

            // Remove solid areas outside the board cutouts and the insulated islands
            // only zones with net code > 0 can have insulated islands by definition
            if( zone.m_zone->GetNetCode() > 0 )
            {
                // solid areas outside the board cutouts are also removed, because they are usually
                // insulated islands
                for( auto idx : islands )
                {
                    if( mode == ISLAND_REMOVAL_MODE::ALWAYS
                            || ( mode == ISLAND_REMOVAL_MODE::AREA
                                    && poly.Outline( idx ).Area() < minArea )
                            || !m_boardOutline.Contains( poly.Polygon( idx ).front().CPoint( 0 ) ) )
                        poly.DeletePolygon( idx );
                    else
                        zone.m_zone->SetIsIsland( layer, idx );
                }
            }
            // Zones with no net can have areas outside the board cutouts.
            // By definition, Zones with no net have no isolated island
            // (in fact all filled areas are isolated islands)
            // but they can have some areas outside the board cutouts.
            // A filled area outside the board cutouts has all points outside cutouts,
            // so we only need to check one point for each filled polygon.
            // Note also non copper zones are already clipped
            else if( m_brdOutlinesValid && zone.m_zone->IsOnCopperLayer() )
            {
                for( int idx = 0; idx < poly.OutlineCount(); )
                {
                    if( poly.Polygon( idx ).empty()
                            || !m_boardOutline.Contains( poly.Polygon( idx ).front().CPoint( 0 ) ) )
                    {
                        poly.DeletePolygon( idx );
                    }
                    else
                        idx++;
                }
            }

            zone.m_zone->SetFilledPolysList( layer, poly );
            zone.m_zone->CalculateFilledArea();

            if( aCheck && zone.m_zone->GetHashValue( layer ) != poly.GetHash() )
                outOfDate = true;

            if( m_progressReporter && m_progressReporter->IsCancelled() )
                return false;
        }
    }

    if( aCheck && outOfDate )
    {
        PROGRESS_REPORTER_HIDER raii( m_progressReporter );
        KIDIALOG dlg( m_progressReporter->GetParent(),
                      _( "Zone fills are out-of-date. Refill?" ),
                      _( "Confirmation" ), wxOK | wxCANCEL | wxICON_WARNING );
        dlg.SetOKCancelLabels( _( "Refill" ), _( "Continue without Refill" ) );
        dlg.DoNotShowCheckbox( __FILE__, __LINE__ );

        if( dlg.ShowModal() == wxID_CANCEL )
            return false;
    }

    if( m_progressReporter )
    {
        m_progressReporter->AdvancePhase();
        m_progressReporter->Report( _( "Performing polygon fills..." ) );
        m_progressReporter->SetMaxProgress( toFill.size() );
    }

    nextItem = 0;

    auto tri_lambda =
            [&]( PROGRESS_REPORTER* aReporter ) -> size_t
            {
                size_t num = 0;

                for( size_t i = nextItem++; i < islandsList.size(); i = nextItem++ )
                {
                    islandsList[i].m_zone->CacheTriangulation();
                    num++;

                    if( m_progressReporter )
                    {
                        m_progressReporter->AdvanceProgress();

                        if( m_progressReporter->IsCancelled() )
                            break;
                    }
                }

                return num;
            };

    if( parallelThreadCount <= 1 )
        tri_lambda( m_progressReporter );
    else
    {
        for( size_t ii = 0; ii < parallelThreadCount; ++ii )
            returns[ii] = std::async( std::launch::async, tri_lambda, m_progressReporter );

        for( size_t ii = 0; ii < parallelThreadCount; ++ii )
        {
            // Here we balance returns with a 100ms timeout to allow UI updating
            std::future_status status;
            do
            {
                if( m_progressReporter )
                {
                    m_progressReporter->KeepRefreshing();

                    if( m_progressReporter->IsCancelled() )
                        break;
                }

                status = returns[ii].wait_for( std::chrono::milliseconds( 100 ) );
            } while( status != std::future_status::ready );
        }
    }

    if( m_progressReporter )
    {
        if( m_progressReporter->IsCancelled() )
            return false;

        m_progressReporter->AdvancePhase();
        m_progressReporter->Report( _( "Committing changes..." ) );
        m_progressReporter->KeepRefreshing();
    }

    connectivity->SetProgressReporter( nullptr );
    return true;
}


/**
 * Return true if the given pad has a thermal connection with the given zone.
 */
bool hasThermalConnection( D_PAD* pad, const ZONE_CONTAINER* aZone )
{
    // Rejects non-standard pads with tht-only thermal reliefs
    if( aZone->GetPadConnection( pad ) == ZONE_CONNECTION::THT_THERMAL
            && pad->GetAttribute() != PAD_ATTRIB_STANDARD )
    {
        return false;
    }

    if( aZone->GetPadConnection( pad ) != ZONE_CONNECTION::THERMAL
            && aZone->GetPadConnection( pad ) != ZONE_CONNECTION::THT_THERMAL )
    {
        return false;
    }

    if( pad->GetNetCode() != aZone->GetNetCode() || pad->GetNetCode() <= 0 )
        return false;

    EDA_RECT item_boundingbox = pad->GetBoundingBox();
    int thermalGap = aZone->GetThermalReliefGap( pad );
    item_boundingbox.Inflate( thermalGap, thermalGap );

    return item_boundingbox.Intersects( aZone->GetBoundingBox() );
}


/**
 * Setup aDummyPad to have the same size and shape of aPad's hole.  This allows us to create
 * thermal reliefs and clearances for holes using the pad code.
 */
static void setupDummyPadForHole( const D_PAD* aPad, D_PAD& aDummyPad )
{
    aDummyPad.SetNetCode( aPad->GetNetCode() );
    aDummyPad.SetSize( aPad->GetDrillSize() );
    aDummyPad.SetOrientation( aPad->GetOrientation() );
    aDummyPad.SetShape( aPad->GetDrillShape() == PAD_DRILL_SHAPE_OBLONG ? PAD_SHAPE_OVAL
                                                                        : PAD_SHAPE_CIRCLE );
    aDummyPad.SetPosition( aPad->GetPosition() );
}


/**
 * Add a knockout for a pad.  The knockout is 'aGap' larger than the pad (which might be
 * either the thermal clearance or the electrical clearance).
 */
void ZONE_FILLER::addKnockout( D_PAD* aPad, int aGap, SHAPE_POLY_SET& aHoles )
{
    if( aPad->GetShape() == PAD_SHAPE_CUSTOM )
    {
        SHAPE_POLY_SET poly;
        aPad->TransformShapeWithClearanceToPolygon( poly, aGap, m_high_def );

        // the pad shape in zone can be its convex hull or the shape itself
        if( aPad->GetCustomShapeInZoneOpt() == CUST_PAD_SHAPE_IN_ZONE_CONVEXHULL )
        {
            std::vector<wxPoint> convex_hull;
            BuildConvexHull( convex_hull, poly );

            aHoles.NewOutline();

            for( const wxPoint& pt : convex_hull )
                aHoles.Append( pt );
        }
        else
            aHoles.Append( poly );
    }
    else
    {
        // Optimizing polygon vertex count: the high definition is used for round
        // and oval pads (pads with large arcs) but low def for other shapes (with
        // small arcs)
        if( aPad->GetShape() == PAD_SHAPE_CIRCLE || aPad->GetShape() == PAD_SHAPE_OVAL ||
          ( aPad->GetShape() == PAD_SHAPE_ROUNDRECT && aPad->GetRoundRectRadiusRatio() > 0.4 ) )
            aPad->TransformShapeWithClearanceToPolygon( aHoles, aGap, m_high_def );
        else
            aPad->TransformShapeWithClearanceToPolygon( aHoles, aGap, m_low_def );
    }
}


/**
 * Add a knockout for a graphic item.  The knockout is 'aGap' larger than the item (which
 * might be either the electrical clearance or the board edge clearance).
 */
void ZONE_FILLER::addKnockout( BOARD_ITEM* aItem, int aGap, bool aIgnoreLineWidth,
                               SHAPE_POLY_SET& aHoles )
{
    switch( aItem->Type() )
    {
    case PCB_LINE_T:
    {
        DRAWSEGMENT* seg = (DRAWSEGMENT*) aItem;
        seg->TransformShapeWithClearanceToPolygon( aHoles, aGap, m_high_def, aIgnoreLineWidth );
        break;
    }
    case PCB_TEXT_T:
    {
        TEXTE_PCB* text = (TEXTE_PCB*) aItem;
        text->TransformBoundingBoxWithClearanceToPolygon( &aHoles, aGap );
        break;
    }
    case PCB_MODULE_EDGE_T:
    {
        EDGE_MODULE* edge = (EDGE_MODULE*) aItem;
        edge->TransformShapeWithClearanceToPolygon( aHoles, aGap, m_high_def, aIgnoreLineWidth );
        break;
    }
    case PCB_MODULE_TEXT_T:
    {
        TEXTE_MODULE* text = (TEXTE_MODULE*) aItem;

        if( text->IsVisible() )
            text->TransformBoundingBoxWithClearanceToPolygon( &aHoles, aGap );

        break;
    }
    default:
        break;
    }
}


/**
 * Removes thermal reliefs from the shape for any pads connected to the zone.  Does NOT add
 * in spokes, which must be done later.
 */
void ZONE_FILLER::knockoutThermalReliefs( const ZONE_CONTAINER* aZone, PCB_LAYER_ID aLayer,
                                          SHAPE_POLY_SET& aFill )
{
    SHAPE_POLY_SET holes;

    // Use a dummy pad to calculate relief when a pad has a hole but is not on the zone's
    // copper layer.  The dummy pad has the size and shape of the original pad's hole. We have
    // to give it a parent because some functions expect a non-null parent to find clearance
    // data, etc.
    MODULE  dummymodule( m_board );
    D_PAD   dummypad( &dummymodule );

    for( auto module : m_board->Modules() )
    {
        for( auto pad : module->Pads() )
        {
            if( !hasThermalConnection( pad, aZone ) )
                continue;

            // If the pad isn't on the current layer but has a hole, knock out a thermal relief
            // for the hole.
            if( !pad->IsOnLayer( aLayer ) )
            {
                if( pad->GetDrillSize().x == 0 && pad->GetDrillSize().y == 0 )
                    continue;

                setupDummyPadForHole( pad, dummypad );
                pad = &dummypad;
            }

            addKnockout( pad, aZone->GetThermalReliefGap( pad ), holes );
        }
    }

    holes.Simplify( SHAPE_POLY_SET::PM_FAST );
    aFill.BooleanSubtract( holes, SHAPE_POLY_SET::PM_FAST );
}


/**
 * Removes clearance from the shape for copper items which share the zone's layer but are
 * not connected to it.
 */
void ZONE_FILLER::buildCopperItemClearances( const ZONE_CONTAINER* aZone, PCB_LAYER_ID aLayer,
                                             SHAPE_POLY_SET& aHoles )
{
    static DRAWSEGMENT dummyEdge;
    dummyEdge.SetLayer( Edge_Cuts );

    // a small extra clearance to be sure actual track clearance is not smaller
    // than requested clearance due to many approximations in calculations,
    // like arc to segment approx, rounding issues...
    // 2 microns are a good value
    int extra_margin = Millimeter2iu( ADVANCED_CFG::GetCfg().m_extraClearance );

    BOARD_DESIGN_SETTINGS& bds = m_board->GetDesignSettings();
    int                    zone_clearance = aZone->GetLocalClearance();
    EDA_RECT               zone_boundingbox = aZone->GetBoundingBox();

    // items outside the zone bounding box are skipped, so it needs to be inflated by
    // the largest clearance value found in the netclasses and rules
    int biggest_clearance = std::max( zone_clearance, bds.GetBiggestClearanceValue() );
    zone_boundingbox.Inflate( biggest_clearance + extra_margin );

    // Use a dummy pad to calculate hole clearance when a pad has a hole but is not on the
    // zone's copper layer.  The dummy pad has the size and shape of the original pad's hole.
    // We have to give it a parent because some functions expect a non-null parent to find
    // clearance data, etc.
    MODULE  dummymodule( m_board );
    D_PAD   dummypad( &dummymodule );

    // Add non-connected pad clearances
    //
    for( MODULE* module : m_board->Modules() )
    {
        for( D_PAD* pad : module->Pads() )
        {
            if( !pad->IsPadOnLayer( aLayer ) )
            {
                if( pad->GetDrillSize().x == 0 && pad->GetDrillSize().y == 0 )
                    continue;

                setupDummyPadForHole( pad, dummypad );
                pad = &dummypad;
            }

            if( pad->GetNetCode() != aZone->GetNetCode() || pad->GetNetCode() <= 0
                    || aZone->GetPadConnection( pad ) == ZONE_CONNECTION::NONE )
            {
                if( pad->GetBoundingBox().Intersects( zone_boundingbox ) )
                {
                    int gap;

                    // for pads having the same netcode as the zone, the net clearance has no
                    // meaning so use the greater of the zone clearance and the thermal relief
                    if( pad->GetNetCode() > 0 && pad->GetNetCode() == aZone->GetNetCode() )
                        gap = std::max( zone_clearance, aZone->GetThermalReliefGap( pad ) );
                    else
                        gap = aZone->GetClearance( aLayer, pad );

                    addKnockout( pad, gap, aHoles );
                }
            }
        }
    }

    // Add non-connected track clearances
    //
    for( TRACK* track : m_board->Tracks() )
    {
        if( !track->IsOnLayer( aLayer ) )
            continue;

        if( track->GetNetCode() == aZone->GetNetCode()  && ( aZone->GetNetCode() != 0) )
            continue;

        if( track->GetBoundingBox().Intersects( zone_boundingbox ) )
        {
            int gap = aZone->GetClearance( aLayer, track ) + extra_margin;

            if( track->Type() == PCB_VIA_T )
            {
                VIA* via = static_cast<VIA*>( track );

                if( !via->IsPadOnLayer( aLayer ) )
                {
                    TransformCircleToPolygon( aHoles, via->GetPosition(),
                            ( via->GetDrillValue() + 1 ) / 2 + gap, m_low_def );
                }
                else
                {
                    via->TransformShapeWithClearanceToPolygon( aHoles, gap, m_low_def );
                }
            }
            else
            {
                track->TransformShapeWithClearanceToPolygon( aHoles, gap, m_low_def );
            }
        }
    }

    // Add graphic item clearances.  They are by definition unconnected, and have no clearance
    // definitions of their own.
    //
    auto doGraphicItem =
            [&]( BOARD_ITEM* aItem )
            {
                // A item on the Edge_Cuts is always seen as on any layer:
                if( !aItem->IsOnLayer( aLayer ) && !aItem->IsOnLayer( Edge_Cuts ) )
                    return;

                if( aItem->GetBoundingBox().Intersects( zone_boundingbox ) )
                {
                    bool ignoreLineWidth = aItem->IsOnLayer( Edge_Cuts );
                    int  gap = aZone->GetClearance( aLayer, aItem );

                    addKnockout( aItem, gap, ignoreLineWidth, aHoles );
                }
            };

    for( MODULE* module : m_board->Modules() )
    {
        doGraphicItem( &module->Reference() );
        doGraphicItem( &module->Value() );

        for( BOARD_ITEM* item : module->GraphicalItems() )
            doGraphicItem( item );
    }

    for( BOARD_ITEM* item : m_board->Drawings() )
        doGraphicItem( item );

    // Add zones outlines having an higher priority and keepout
    //
    for( ZONE_CONTAINER* zone : m_board->GetZoneList( true ) )
    {

        // If the zones share no common layers
        if( !zone->GetLayerSet().test( aLayer ) )
            continue;

        if( !zone->GetIsKeepout() && zone->GetPriority() <= aZone->GetPriority() )
            continue;

        if( zone->GetIsKeepout() && !zone->GetDoNotAllowCopperPour() )
            continue;

        // A higher priority zone or keepout area is found: remove this area
        EDA_RECT item_boundingbox = zone->GetBoundingBox();

        if( item_boundingbox.Intersects( zone_boundingbox ) )
        {
            // Add the zone outline area.  Don't use any clearance for keepouts, or for zones
            // with the same net (they will be connected but will honor their own clearance,
            // thermal connections, etc.).
            int  gap = 0;

            if( !zone->GetIsKeepout() && aZone->GetNetCode() != zone->GetNetCode() )
                gap = aZone->GetClearance( aLayer, zone );

            zone->TransformOutlinesShapeWithClearanceToPolygon( aHoles, gap );
        }
    }

    aHoles.Simplify( SHAPE_POLY_SET::PM_FAST );
}


/**
 * 1 - Creates the main zone outline using a correction to shrink the resulting area by
 *     m_ZoneMinThickness / 2.  The result is areas with a margin of m_ZoneMinThickness / 2
 *     so that when drawing outline with segments having a thickness of m_ZoneMinThickness the
 *     outlines will match exactly the initial outlines
 * 2 - Knocks out thermal reliefs around thermally-connected pads
 * 3 - Builds a set of thermal spoke for the whole zone
 * 4 - Knocks out unconnected copper items, deleting any affected spokes
 * 5 - Removes unconnected copper islands, deleting any affected spokes
 * 6 - Adds in the remaining spokes
 */
void ZONE_FILLER::computeRawFilledArea( const ZONE_CONTAINER* aZone, PCB_LAYER_ID aLayer,
                                        const SHAPE_POLY_SET& aSmoothedOutline,
                                        SHAPE_POLY_SET& aRawPolys,
                                        SHAPE_POLY_SET& aFinalPolys )
{
    m_high_def = m_board->GetDesignSettings().m_MaxError;
    m_low_def = std::min( ARC_LOW_DEF, int( m_high_def*1.5 ) );   // Reasonable value

    // Features which are min_width should survive pruning; features that are *less* than
    // min_width should not.  Therefore we subtract epsilon from the min_width when
    // deflating/inflating.
    int half_min_width = aZone->GetMinThickness() / 2;
    int epsilon = Millimeter2iu( 0.001 );
    int numSegs = std::max( GetArcToSegmentCount( half_min_width, m_high_def, 360.0 ), 6 );

    // solid polygons are deflated and inflated during calculations.
    // Polygons deflate usually do not create issues.
    // Polygons inflate is a tricky transform, because it can create excessively long and narrow 'spikes'
    // especially for acute angles.
    // But in very case, the inflate transform caannot create bigger shapes than initial shapes.
    // so the corner strategy is very important.
    // The best is SHAPE_POLY_SET::ROUND_ALL_CORNERS.
    // unfortunately, it creates a lot of small segments.
    // SHAPE_POLY_SET::ALLOW_ACUTE_CORNERS is not acceptable
    // So for intermediate transforms, we use CHAMFER_ALL_CORNERS.
    // For final transform, we use ROUND_ALL_CORNERS
    SHAPE_POLY_SET::CORNER_STRATEGY intermediatecornerStrategy = SHAPE_POLY_SET::CHAMFER_ALL_CORNERS;
    SHAPE_POLY_SET::CORNER_STRATEGY finalcornerStrategy = SHAPE_POLY_SET::ROUND_ALL_CORNERS;

    std::deque<SHAPE_LINE_CHAIN> thermalSpokes;
    SHAPE_POLY_SET clearanceHoles;

    std::unique_ptr<SHAPE_FILE_IO> dumper( new SHAPE_FILE_IO(
                    s_DumpZonesWhenFilling ? "zones_dump.txt" : "", SHAPE_FILE_IO::IOM_APPEND ) );

    aRawPolys = aSmoothedOutline;

    if( s_DumpZonesWhenFilling )
        dumper->BeginGroup( "clipper-zone" );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    knockoutThermalReliefs( aZone, aLayer, aRawPolys );

    if( s_DumpZonesWhenFilling )
        dumper->Write( &aRawPolys, "solid-areas-minus-thermal-reliefs" );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    buildCopperItemClearances( aZone, aLayer, clearanceHoles );

    if( s_DumpZonesWhenFilling )
        dumper->Write( &aRawPolys, "clearance holes" );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    buildThermalSpokes( aZone, aLayer, thermalSpokes );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    // Create a temporary zone that we can hit-test spoke-ends against.  It's only temporary
    // because the "real" subtract-clearance-holes has to be done after the spokes are added.
    static const bool USE_BBOX_CACHES = true;
    SHAPE_POLY_SET testAreas = aRawPolys;
    testAreas.BooleanSubtract( clearanceHoles, SHAPE_POLY_SET::PM_FAST );

    // Prune features that don't meet minimum-width criteria
    if( half_min_width - epsilon > epsilon )
    {
        testAreas.Deflate( half_min_width - epsilon, numSegs, intermediatecornerStrategy );
        testAreas.Inflate( half_min_width - epsilon, numSegs, intermediatecornerStrategy );
    }

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    // Spoke-end-testing is hugely expensive so we generate cached bounding-boxes to speed
    // things up a bit.
    testAreas.BuildBBoxCaches();
    int interval = 0;

    for( const SHAPE_LINE_CHAIN& spoke : thermalSpokes )
    {
        const VECTOR2I& testPt = spoke.CPoint( 3 );

        // Hit-test against zone body
        if( testAreas.Contains( testPt, -1, 1, USE_BBOX_CACHES ) )
        {
            aRawPolys.AddOutline( spoke );
            continue;
        }

        if( interval++ > 400 )
        {
            if( m_progressReporter && m_progressReporter->IsCancelled() )
                return;

            interval = 0;
        }

        // Hit-test against other spokes
        for( const SHAPE_LINE_CHAIN& other : thermalSpokes )
        {
            if( &other != &spoke && other.PointInside( testPt, 1, USE_BBOX_CACHES  ) )
            {
                aRawPolys.AddOutline( spoke );
                break;
            }
        }
    }

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    // Ensure previous changes (adding thermal stubs) do not add
    // filled areas outside the zone boundary
    aRawPolys.BooleanIntersection( aSmoothedOutline, SHAPE_POLY_SET::PM_FAST );
    aRawPolys.Simplify( SHAPE_POLY_SET::PM_FAST );

    if( s_DumpZonesWhenFilling )
        dumper->Write( &aRawPolys, "solid-areas-with-thermal-spokes" );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    aRawPolys.BooleanSubtract( clearanceHoles, SHAPE_POLY_SET::PM_FAST );
    // Prune features that don't meet minimum-width criteria
    if( half_min_width - epsilon > epsilon )
        aRawPolys.Deflate( half_min_width - epsilon, numSegs, intermediatecornerStrategy );

    if( s_DumpZonesWhenFilling )
        dumper->Write( &aRawPolys, "solid-areas-before-hatching" );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    // Now remove the non filled areas due to the hatch pattern
    if( aZone->GetFillMode() == ZONE_FILL_MODE::HATCH_PATTERN )
        addHatchFillTypeOnZone( aZone, aLayer, aRawPolys );

    if( s_DumpZonesWhenFilling )
        dumper->Write( &aRawPolys, "solid-areas-after-hatching" );

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return;

    // Re-inflate after pruning of areas that don't meet minimum-width criteria
    if( aZone->GetFilledPolysUseThickness() )
    {
        // If we're stroking the zone with a min_width stroke then this will naturally
        // inflate the zone by half_min_width
    }
    else if( half_min_width - epsilon > epsilon )
    {
        aRawPolys.Simplify( SHAPE_POLY_SET::PM_FAST );
        aRawPolys.Inflate( half_min_width - epsilon, numSegs, finalcornerStrategy );

        // If we've deflated/inflated by something near our corner radius then we will have
        // ended up with too-sharp corners.  Apply outline smoothing again.
        if( aZone->GetMinThickness() > (int)aZone->GetCornerRadius() )
            aRawPolys.BooleanIntersection( aSmoothedOutline, SHAPE_POLY_SET::PM_FAST );
    }

    aRawPolys.Fracture( SHAPE_POLY_SET::PM_FAST );

    if( s_DumpZonesWhenFilling )
        dumper->Write( &aRawPolys, "areas_fractured" );

    aFinalPolys = aRawPolys;

    if( s_DumpZonesWhenFilling )
        dumper->EndGroup();
}


/*
 * Build the filled solid areas data from real outlines (stored in m_Poly)
 * The solid areas can be more than one on copper layers, and do not have holes
 * ( holes are linked by overlapping segments to the main outline)
 */
bool ZONE_FILLER::fillSingleZone( ZONE_CONTAINER* aZone, PCB_LAYER_ID aLayer,
                                  SHAPE_POLY_SET& aRawPolys, SHAPE_POLY_SET& aFinalPolys )
{
    SHAPE_POLY_SET smoothedPoly;

    /*
     * convert outlines + holes to outlines without holes (adding extra segments if necessary)
     * m_Poly data is expected normalized, i.e. NormalizeAreaOutlines was used after building
     * this zone
     */
    if ( !aZone->BuildSmoothedPoly( smoothedPoly, aLayer ) )
        return false;

    if( m_progressReporter && m_progressReporter->IsCancelled() )
        return false;

    if( aZone->IsOnCopperLayer() )
    {
        computeRawFilledArea( aZone, aLayer, smoothedPoly, aRawPolys, aFinalPolys );
    }
    else
    {
        // Features which are min_width should survive pruning; features that are *less* than
        // min_width should not.  Therefore we subtract epsilon from the min_width when
        // deflating/inflating.
        int half_min_width = aZone->GetMinThickness() / 2;
        int epsilon = Millimeter2iu( 0.001 );
        int numSegs = std::max( GetArcToSegmentCount( half_min_width, m_high_def, 360.0 ), 6 );

        if( m_brdOutlinesValid )
            smoothedPoly.BooleanIntersection( m_boardOutline, SHAPE_POLY_SET::PM_STRICTLY_SIMPLE );

        smoothedPoly.Deflate( half_min_width/* - epsilon*/, numSegs );

        // Remove the non filled areas due to the hatch pattern
        if( aZone->GetFillMode() == ZONE_FILL_MODE::HATCH_PATTERN )
            addHatchFillTypeOnZone( aZone, aLayer, smoothedPoly );

        // Re-inflate after pruning of areas that don't meet minimum-width criteria
        if( aZone->GetFilledPolysUseThickness() )
        {
            // If we're stroking the zone with a min_width stroke then this will naturally
            // inflate the zone by half_min_width
        }
        else if( half_min_width - epsilon > epsilon )
            smoothedPoly.Deflate( -( half_min_width - epsilon ), numSegs );

        aRawPolys = smoothedPoly;
        aFinalPolys = smoothedPoly;

        aFinalPolys.Fracture( SHAPE_POLY_SET::PM_STRICTLY_SIMPLE );
    }

    aZone->SetNeedRefill( false );
    return true;
}


/**
 * Function buildThermalSpokes
 */
void ZONE_FILLER::buildThermalSpokes( const ZONE_CONTAINER* aZone, PCB_LAYER_ID aLayer,
                                      std::deque<SHAPE_LINE_CHAIN>& aSpokesList )
{
    auto zoneBB = aZone->GetBoundingBox();
    int  zone_clearance = aZone->GetZoneClearance();
    int  biggest_clearance = m_board->GetDesignSettings().GetBiggestClearanceValue();
    biggest_clearance = std::max( biggest_clearance, zone_clearance );
    zoneBB.Inflate( biggest_clearance );

    // Is a point on the boundary of the polygon inside or outside?  This small epsilon lets
    // us avoid the question.
    int epsilon = KiROUND( IU_PER_MM * 0.04 );  // about 1.5 mil

    for( auto module : m_board->Modules() )
    {
        for( auto pad : module->Pads() )
        {
            if( !hasThermalConnection( pad, aZone ) )
                continue;

            // We currently only connect to pads, not pad holes
            if( !pad->IsOnLayer( aLayer ) )
                continue;

            int thermalReliefGap = aZone->GetThermalReliefGap( pad );

            // Calculate thermal bridge half width
            int spoke_w = aZone->GetThermalReliefCopperBridge( pad );
            // Avoid spoke_w bigger than the smaller pad size, because
            // it is not possible to create stubs bigger than the pad.
            // Possible refinement: have a separate size for vertical and horizontal stubs
            spoke_w = std::min( spoke_w, pad->GetSize().x );
            spoke_w = std::min( spoke_w, pad->GetSize().y );

            // Cannot create stubs having a width < zone min thickness
            if( spoke_w <= aZone->GetMinThickness() )
                continue;

            int spoke_half_w = spoke_w / 2;

            // Quick test here to possibly save us some work
            BOX2I itemBB = pad->GetBoundingBox();
            itemBB.Inflate( thermalReliefGap + epsilon );

            if( !( itemBB.Intersects( zoneBB ) ) )
                continue;

            // Thermal spokes consist of segments from the pad center to points just outside
            // the thermal relief.
            //
            // We use the bounding-box to lay out the spokes, but for this to work the
            // bounding box has to be built at the same rotation as the spokes.
            // We have to use a dummy pad to avoid dirtying the cached shapes
            wxPoint shapePos = pad->ShapePos();
            double  padAngle = pad->GetOrientation();
            D_PAD   dummy_pad( *pad );
            dummy_pad.SetOrientation( 0.0 );
            dummy_pad.SetPosition( { 0, 0 } );

            BOX2I reliefBB = dummy_pad.GetBoundingBox();
            reliefBB.Inflate( thermalReliefGap + epsilon );

            // For circle pads, the thermal spoke orientation is 45 deg
            if( pad->GetShape() == PAD_SHAPE_CIRCLE )
                padAngle = s_RoundPadThermalSpokeAngle;

            for( int i = 0; i < 4; i++ )
            {
                SHAPE_LINE_CHAIN spoke;
                switch( i )
                {
                case 0:       // lower stub
                    spoke.Append( +spoke_half_w,       -spoke_half_w );
                    spoke.Append( -spoke_half_w,       -spoke_half_w );
                    spoke.Append( -spoke_half_w,       reliefBB.GetBottom() );
                    spoke.Append( 0,                   reliefBB.GetBottom() );  // test pt
                    spoke.Append( +spoke_half_w,       reliefBB.GetBottom() );
                    break;

                case 1:       // upper stub
                    spoke.Append( +spoke_half_w,       spoke_half_w );
                    spoke.Append( -spoke_half_w,       spoke_half_w );
                    spoke.Append( -spoke_half_w,       reliefBB.GetTop() );
                    spoke.Append( 0,                   reliefBB.GetTop() );     // test pt
                    spoke.Append( +spoke_half_w,       reliefBB.GetTop() );
                    break;

                case 2:       // right stub
                    spoke.Append( -spoke_half_w,       spoke_half_w );
                    spoke.Append( -spoke_half_w,       -spoke_half_w );
                    spoke.Append( reliefBB.GetRight(), -spoke_half_w );
                    spoke.Append( reliefBB.GetRight(), 0 );                     // test pt
                    spoke.Append( reliefBB.GetRight(), spoke_half_w );
                    break;

                case 3:       // left stub
                    spoke.Append( spoke_half_w,        spoke_half_w );
                    spoke.Append( spoke_half_w,        -spoke_half_w );
                    spoke.Append( reliefBB.GetLeft(),  -spoke_half_w );
                    spoke.Append( reliefBB.GetLeft(),  0 );                     // test pt
                    spoke.Append( reliefBB.GetLeft(),  spoke_half_w );
                    break;
                }

                spoke.Rotate( -DECIDEG2RAD( padAngle ) );
                spoke.Move( shapePos );

                spoke.SetClosed( true );
                spoke.GenerateBBoxCache();
                aSpokesList.push_back( std::move( spoke ) );
            }
        }
    }
}


void ZONE_FILLER::addHatchFillTypeOnZone( const ZONE_CONTAINER* aZone, PCB_LAYER_ID aLayer,
                                          SHAPE_POLY_SET& aRawPolys )
{
    // Build grid:

    // obviously line thickness must be > zone min thickness.
    // It can happens if a board file was edited by hand by a python script
    // Use 1 micron margin to be *sure* there is no issue in Gerber files
    // (Gbr file unit = 1 or 10 nm) due to some truncation in coordinates or calculations
    // This margin also avoid problems due to rounding coordinates in next calculations
    // that can create incorrect polygons
    int thickness = std::max( aZone->GetHatchThickness(),
                              aZone->GetMinThickness() + Millimeter2iu( 0.001 ) );

    int linethickness = thickness - aZone->GetMinThickness();
    int gridsize = thickness + aZone->GetHatchGap();
    double orientation = aZone->GetHatchOrientation();

    SHAPE_POLY_SET filledPolys = aRawPolys;
    // Use a area that contains the rotated bbox by orientation,
    // and after rotate the result by -orientation.
    if( orientation != 0.0 )
        filledPolys.Rotate( M_PI/180.0 * orientation, VECTOR2I( 0,0 ) );

    BOX2I bbox = filledPolys.BBox( 0 );

    // Build hole shape
    // the hole size is aZone->GetHatchGap(), but because the outline thickness
    // is aZone->GetMinThickness(), the hole shape size must be larger
    SHAPE_LINE_CHAIN hole_base;
    int hole_size = aZone->GetHatchGap() + aZone->GetMinThickness();
    VECTOR2I corner( 0, 0 );;
    hole_base.Append( corner );
    corner.x += hole_size;
    hole_base.Append( corner );
    corner.y += hole_size;
    hole_base.Append( corner );
    corner.x = 0;
    hole_base.Append( corner );
    hole_base.SetClosed( true );

    // Calculate minimal area of a grid hole.
    // All holes smaller than a threshold will be removed
    double minimal_hole_area = hole_base.Area() * aZone->GetHatchHoleMinArea();

    // Now convert this hole to a smoothed shape:
    if( aZone->GetHatchSmoothingLevel() > 0 )
    {
        // the actual size of chamfer, or rounded corner radius is the half size
        // of the HatchFillTypeGap scaled by aZone->GetHatchSmoothingValue()
        // aZone->GetHatchSmoothingValue() = 1.0 is the max value for the chamfer or the
        // radius of corner (radius = half size of the hole)
        int smooth_value = KiROUND( aZone->GetHatchGap()
                                    * aZone->GetHatchSmoothingValue() / 2 );

        // Minimal optimization:
        // make smoothing only for reasonnable smooth values, to avoid a lot of useless segments
        // and if the smooth value is small, use chamfer even if fillet is requested
        #define SMOOTH_MIN_VAL_MM 0.02
        #define SMOOTH_SMALL_VAL_MM 0.04

        if( smooth_value > Millimeter2iu( SMOOTH_MIN_VAL_MM ) )
        {
            SHAPE_POLY_SET smooth_hole;
            smooth_hole.AddOutline( hole_base );
            int smooth_level = aZone->GetHatchSmoothingLevel();

            if( smooth_value < Millimeter2iu( SMOOTH_SMALL_VAL_MM ) && smooth_level > 1 )
                smooth_level = 1;

            // Use a larger smooth_value to compensate the outline tickness
            // (chamfer is not visible is smooth value < outline thickess)
            smooth_value += aZone->GetMinThickness() / 2;

            // smooth_value cannot be bigger than the half size oh the hole:
            smooth_value = std::min( smooth_value, aZone->GetHatchGap() / 2 );

            // the error to approximate a circle by segments when smoothing corners by a arc
            int error_max = std::max( Millimeter2iu( 0.01 ), smooth_value / 20 );

            switch( smooth_level )
            {
            case 1:
                // Chamfer() uses the distance from a corner to create a end point
                // for the chamfer.
                hole_base = smooth_hole.Chamfer( smooth_value ).Outline( 0 );
                break;

            default:
                if( aZone->GetHatchSmoothingLevel() > 2 )
                    error_max /= 2;    // Force better smoothing

                hole_base = smooth_hole.Fillet( smooth_value, error_max ).Outline( 0 );
                break;

            case 0:
                break;
            };
        }
    }

    // Build holes
    SHAPE_POLY_SET holes;

    for( int xx = 0; ; xx++ )
    {
        int xpos = xx * gridsize;

        if( xpos > bbox.GetWidth() )
            break;

        for( int yy = 0; ; yy++ )
        {
            int ypos = yy * gridsize;

            if( ypos > bbox.GetHeight() )
                break;

            // Generate hole
            SHAPE_LINE_CHAIN hole( hole_base );
            hole.Move( VECTOR2I( xpos, ypos ) );
            holes.AddOutline( hole );
        }
    }

    holes.Move( bbox.GetPosition() );

    // We must buffer holes by at least aZone->GetMinThickness() to guarantee that thermal
    // reliefs can be built (and to give the zone a solid outline).  However, it looks more
    // visually consistent if the buffer width is the same as the hatch width.
    int outline_margin = KiROUND( aZone->GetMinThickness() * 1.1 );

    if( aZone->GetHatchBorderAlgorithm() )
        outline_margin = std::max( outline_margin, aZone->GetHatchThickness() );

    if( outline_margin > linethickness / 2 )
        filledPolys.Deflate( outline_margin - linethickness / 2, 16 );

    holes.BooleanIntersection( filledPolys, SHAPE_POLY_SET::PM_FAST );

    if( orientation != 0.0 )
        holes.Rotate( -M_PI/180.0 * orientation, VECTOR2I( 0,0 ) );

    if( aZone->GetNetCode() != 0 )
    {
        // Vias and pads connected to the zone must not be allowed to become isolated inside
        // one of the holes.  Effectively this means their copper outline needs to be expanded
        // to be at least as wide as the gap so that it is guaranteed to touch at least one
        // edge.
        EDA_RECT       zone_boundingbox = aZone->GetBoundingBox();
        SHAPE_POLY_SET aprons;
        int            min_apron_radius = ( aZone->GetHatchGap() * 10 ) / 19;

        for( TRACK* track : m_board->Tracks() )
        {
            if( track->Type() == PCB_VIA_T )
            {
                VIA* via = static_cast<VIA*>( track );

                if( via->GetNetCode() == aZone->GetNetCode()
                    && via->IsOnLayer( aLayer )
                    && via->GetBoundingBox().Intersects( zone_boundingbox ) )
                {
                    int r = std::max( min_apron_radius,
                                      via->GetDrillValue() / 2 + outline_margin );

                    TransformCircleToPolygon( aprons, via->GetPosition(), r, ARC_HIGH_DEF );
                }
            }
        }

        for( MODULE* module : m_board->Modules() )
        {
            for( D_PAD* pad : module->Pads() )
            {
                if( pad->GetNetCode() == aZone->GetNetCode()
                    && pad->IsOnLayer( aLayer )
                    && pad->GetBoundingBox().Intersects( zone_boundingbox ) )
                {
                    // What we want is to bulk up the pad shape so that the narrowest bit of
                    // copper between the hole and the apron edge is at least outline_margin
                    // wide (and that the apron itself meets min_apron_radius.  But that would
                    // take a lot of code and math, and the following approximation is close
                    // enough.
                    int pad_width = std::min( pad->GetSize().x, pad->GetSize().y );
                    int slot_width = std::min( pad->GetDrillSize().x, pad->GetDrillSize().y );
                    int min_annulus = ( pad_width - slot_width ) / 2;
                    int clearance = std::max( min_apron_radius - pad_width / 2,
                                              outline_margin - min_annulus );

                    clearance = std::max( 0, clearance - linethickness / 2 );
                    pad->TransformShapeWithClearanceToPolygon( aprons, clearance, ARC_HIGH_DEF );
                }
            }
        }

        holes.BooleanSubtract( aprons, SHAPE_POLY_SET::PM_FAST );
    }

    // Now filter truncated holes to avoid small holes in pattern
    // It happens for holes near the zone outline
    for( int ii = 0; ii < holes.OutlineCount(); )
    {
        double area = holes.Outline( ii ).Area();

        if( area < minimal_hole_area ) // The current hole is too small: remove it
            holes.DeletePolygon( ii );
        else
            ++ii;
    }

    // create grid. Use SHAPE_POLY_SET::PM_STRICTLY_SIMPLE to
    // generate strictly simple polygons needed by Gerber files and Fracture()
    aRawPolys.BooleanSubtract( aRawPolys, holes, SHAPE_POLY_SET::PM_STRICTLY_SIMPLE );
}

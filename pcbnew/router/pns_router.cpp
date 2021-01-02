/*
 * KiRouter - a push-and-(sometimes-)shove PCB router
 *
 * Copyright (C) 2013-2014 CERN
 * Copyright (C) 2016 KiCad Developers, see AUTHORS.txt for contributors.
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdio>
#include <memory>
#include <vector>

#include <view/view.h>
#include <view/view_group.h>
#include <gal/graphics_abstraction_layer.h>

#include <pgm_base.h>
#include <settings/settings_manager.h>

#include <pcb_painter.h>
#include <pcbnew_settings.h>

#include <geometry/shape.h>

#include "pns_node.h"
#include "pns_line_placer.h"
#include "pns_line.h"
#include "pns_solid.h"
#include "pns_utils.h"
#include "pns_router.h"
#include "pns_shove.h"
#include "pns_dragger.h"
#include "pns_component_dragger.h"
#include "pns_topology.h"
#include "pns_diff_pair_placer.h"
#include "pns_meander_placer.h"
#include "pns_meander_skew_placer.h"
#include "pns_dp_meander_placer.h"

namespace PNS {

// an ugly singleton for drawing debug items within the router context.
// To be fixed sometime in the future.
static ROUTER* theRouter;

ROUTER::ROUTER()
{
    theRouter = this;

    m_state = IDLE;
    m_mode = PNS_MODE_ROUTE_SINGLE;

    m_logger = new LOGGER;

    // Initialize all other variables:
    m_lastNode = nullptr;
    m_iterLimit = 0;
    m_settings = nullptr;
    m_showInterSteps = false;
    m_snapshotIter = 0;
    m_violation = false;
    m_iface = nullptr;
}


ROUTER* ROUTER::GetInstance()
{
    return theRouter;
}


ROUTER::~ROUTER()
{
    ClearWorld();
    theRouter = nullptr;
    delete m_logger;
}


void ROUTER::SyncWorld()
{
    ClearWorld();

    m_world = std::make_unique<NODE>( );
    m_iface->SyncWorld( m_world.get() );

}

void ROUTER::ClearWorld()
{
    if( m_world )
    {
        m_world->KillChildren();
        m_world.reset();
    }

    m_placer.reset();
}


bool ROUTER::RoutingInProgress() const
{
    return m_state != IDLE;
}


const ITEM_SET ROUTER::QueryHoverItems( const VECTOR2I& aP )
{
    if( m_state == IDLE || m_placer == nullptr )
        return m_world->HitTest( aP );
    else
        return m_placer->CurrentNode()->HitTest( aP );
}

bool ROUTER::StartDragging( const VECTOR2I& aP, ITEM* aItem, int aDragMode )
{
    return StartDragging( aP, ITEM_SET( aItem ), aDragMode );
}


bool ROUTER::StartDragging( const VECTOR2I& aP, ITEM_SET aStartItems, int aDragMode )
{
    if( aStartItems.Empty() )
        return false;

    if( aStartItems.Count( ITEM::SOLID_T ) == aStartItems.Size() )
    {
        m_dragger = std::make_unique<COMPONENT_DRAGGER>( this );
        m_forceMarkObstaclesMode = true;
    }
    else
    {
        if( aDragMode & DM_FREE_ANGLE )
            m_forceMarkObstaclesMode = true;
        else
            m_forceMarkObstaclesMode = false;

        m_dragger = std::make_unique<DRAGGER>( this );
    }

    m_dragger->SetMode( aDragMode );
    m_dragger->SetWorld( m_world.get() );
    m_dragger->SetLogger( m_logger );
    m_dragger->SetDebugDecorator( m_iface->GetDebugDecorator() );

    if( m_dragger->Start( aP, aStartItems ) )
    {
        m_state = DRAG_SEGMENT;
    }
    else
    {
        m_dragger.reset();
        m_state = IDLE;
        return false;
    }

    return true;
}

bool ROUTER::isStartingPointRoutable( const VECTOR2I& aWhere, ITEM* aStartItem, int aLayer )
{
    if( Settings().CanViolateDRC() && Settings().Mode() == RM_MarkObstacles )
        return true;

    ITEM_SET candidates = QueryHoverItems( aWhere );

    for( ITEM* item : candidates.Items() )
    {
        if( !item->IsRoutable() && item->Layers().Overlaps( aLayer ) )
            return false;
    }

    if( m_mode == PNS_MODE_ROUTE_SINGLE && aStartItem )
    {
        VECTOR2I startPoint = aStartItem->Anchor( 0 );
        SEGMENT  dummyStartSeg( SEG( startPoint, startPoint ), aStartItem->Net() );

        dummyStartSeg.SetWidth( m_sizes.TrackWidth() );
        dummyStartSeg.SetLayer( aLayer );

        if( m_world->CheckColliding( &dummyStartSeg, ITEM::ANY_T ) )
            return false;
    }
    else if( m_mode == PNS_MODE_ROUTE_DIFF_PAIR && aStartItem )
    {
        // TODO
    }

    return true;
}

bool ROUTER::StartRouting( const VECTOR2I& aP, ITEM* aStartItem, int aLayer )
{   
    if( ! isStartingPointRoutable( aP, aStartItem, aLayer ) )
    {
        SetFailureReason( _( "The routing start point violates DRC." ) );
        return false;
    }

    m_forceMarkObstaclesMode = false;

    switch( m_mode )
    {
        case PNS_MODE_ROUTE_SINGLE:
            m_placer = std::make_unique<LINE_PLACER>( this );
            break;
        case PNS_MODE_ROUTE_DIFF_PAIR:
            m_placer = std::make_unique<DIFF_PAIR_PLACER>( this );
            break;
        case PNS_MODE_TUNE_SINGLE:
            m_placer = std::make_unique<MEANDER_PLACER>( this );
            break;
        case PNS_MODE_TUNE_DIFF_PAIR:
            m_placer = std::make_unique<DP_MEANDER_PLACER>( this );
            break;
        case PNS_MODE_TUNE_DIFF_PAIR_SKEW:
            m_placer = std::make_unique<MEANDER_SKEW_PLACER>( this );
            break;

        default:
            return false;
    }

    m_placer->UpdateSizes( m_sizes );
    m_placer->SetLayer( aLayer );
    m_placer->SetDebugDecorator( m_iface->GetDebugDecorator() );
    m_placer->SetLogger( m_logger );

    if( m_logger )
        m_logger->Log( LOGGER::EVT_START_ROUTE, aP, aStartItem );

    bool rv = m_placer->Start( aP, aStartItem );

    if( !rv )
        return false;

    m_currentEnd = aP;
    m_state = ROUTE_TRACK;
    return rv;
}


void ROUTER::Move( const VECTOR2I& aP, ITEM* endItem )
{
    m_currentEnd = aP;

    if( m_logger )
        m_logger->Log( LOGGER::EVT_MOVE, aP, endItem );

    switch( m_state )
    {
    case ROUTE_TRACK:
        movePlacing( aP, endItem );
        break;

    case DRAG_SEGMENT:
        moveDragging( aP, endItem );
        break;

    default:
        break;
    }
}


void ROUTER::moveDragging( const VECTOR2I& aP, ITEM* aEndItem )
{
    m_iface->EraseView();

    m_dragger->Drag( aP );
    ITEM_SET dragged = m_dragger->Traces();

    updateView( m_dragger->CurrentNode(), dragged, true );
}


void ROUTER::markViolations( NODE* aNode, ITEM_SET& aCurrent, NODE::ITEM_VECTOR& aRemoved )
{
    auto updateItem =
            [&]( ITEM* currentItem, ITEM* itemToMark )
            {
                std::unique_ptr<ITEM> tmp( itemToMark->Clone() );
                int                   clearance;

                if( itemToMark->Marker() & MK_HOLE )
                    clearance = aNode->GetHoleClearance( currentItem, itemToMark );
                else
                    clearance = aNode->GetClearance( currentItem, itemToMark );

                m_iface->DisplayItem( tmp.get(), -1, clearance );

                // Remove the obstacle itself from the view unless we're just marking its hole
                if( !(itemToMark->Marker() & MK_HOLE ) )
                    aRemoved.push_back( itemToMark );
            };

    for( ITEM* item : aCurrent.Items() )
    {
        NODE::OBSTACLES obstacles;

        aNode->QueryColliding( item, obstacles, ITEM::ANY_T );

        if( item->OfKind( ITEM::LINE_T ) )
        {
            LINE* l = static_cast<LINE*>( item );

            if( l->EndsWithVia() )
            {
                VIA v( l->Via() );
                aNode->QueryColliding( &v, obstacles, ITEM::ANY_T );
            }
        }

        for( OBSTACLE& obs : obstacles )
        {
            obs.m_item->Mark( obs.m_item->Marker() | MK_VIOLATION );
            updateItem( item, obs.m_item );
        }

        if( item->Kind() == ITEM::LINE_T )
        {
            LINE* line = static_cast<LINE*>( item );

            // Show clearance on any blocking obstacles
            if( line->GetBlockingObstacle() )
                updateItem( item, line->GetBlockingObstacle() );
        }
    }
}


void ROUTER::updateView( NODE* aNode, ITEM_SET& aCurrent, bool aDragging )
{
    NODE::ITEM_VECTOR removed, added;
    NODE::OBSTACLES obstacles;

    if( !aNode )
        return;

    if( Settings().Mode() == RM_MarkObstacles || m_forceMarkObstaclesMode )
        markViolations( aNode, aCurrent, removed );

    aNode->GetUpdatedItems( removed, added );

    for( ITEM* item : added )
    {
        int clearance = GetRuleResolver()->Clearance( item, nullptr );
        m_iface->DisplayItem( item, -1, clearance, aDragging );
    }

    for( ITEM* item : removed )
        m_iface->HideItem( item );
}


void ROUTER::UpdateSizes( const SIZES_SETTINGS& aSizes )
{
    m_sizes = aSizes;

    // Change track/via size settings
    if( m_state == ROUTE_TRACK)
        m_placer->UpdateSizes( m_sizes );
}


void ROUTER::movePlacing( const VECTOR2I& aP, ITEM* aEndItem )
{
    m_iface->EraseView();

    m_placer->Move( aP, aEndItem );
    ITEM_SET current = m_placer->Traces();

    for( const ITEM* item : current.CItems() )
    {
        if( !item->OfKind( ITEM::LINE_T ) )
            continue;

        const LINE* l = static_cast<const LINE*>( item );
        int clearance = GetRuleResolver()->Clearance( item, nullptr );

        m_iface->DisplayItem( l, -1, clearance );

        if( l->EndsWithVia() )
        {
            const VIA& via = l->Via();
            int viaClearance = GetRuleResolver()->Clearance( &via, nullptr );
            int holeClearance = GetRuleResolver()->HoleClearance( &via, nullptr );

            if( holeClearance + via.Drill() / 2 > viaClearance + via.Diameter() / 2 )
                viaClearance = holeClearance + via.Drill() / 2 - via.Diameter() / 2;

            m_iface->DisplayItem( &l->Via(), -1, viaClearance );
        }
    }

    //ITEM_SET tmp( &current );

    updateView( m_placer->CurrentNode( true ), current );
}


void ROUTER::CommitRouting( NODE* aNode )
{
    if( m_state == ROUTE_TRACK && !m_placer->HasPlacedAnything() )
        return;

    NODE::ITEM_VECTOR removed, added;

    aNode->GetUpdatedItems( removed, added );

    for( ITEM* item : removed )
        m_iface->RemoveItem( item );

    for( ITEM* item : added )
        m_iface->AddItem( item );

    m_iface->Commit();
    m_world->Commit( aNode );
}


bool ROUTER::FixRoute( const VECTOR2I& aP, ITEM* aEndItem, bool aForceFinish )
{
    bool rv = false;

    if( m_logger )
        m_logger->Log( LOGGER::EVT_FIX, aP, aEndItem );

    switch( m_state )
    {
    case ROUTE_TRACK:
        rv = m_placer->FixRoute( aP, aEndItem, aForceFinish );
        break;

    case DRAG_SEGMENT:
        rv = m_dragger->FixRoute();
        break;

    default:
        break;
    }

    return rv;
}


void ROUTER::UndoLastSegment()
{
    if( !RoutingInProgress() )
        return;

    m_placer->UnfixRoute();
}


void ROUTER::CommitRouting()
{
    if( m_state == ROUTE_TRACK )
        m_placer->CommitPlacement();

    StopRouting();
}


void ROUTER::StopRouting()
{
    // Update the ratsnest with new changes

    if( m_placer )
    {
        std::vector<int> nets;
        m_placer->GetModifiedNets( nets );

        // Update the ratsnest with new changes
        for( int n : nets )
            m_iface->UpdateNet( n );
    }

    if( !RoutingInProgress() )
        return;

    m_placer.reset();
    m_dragger.reset();

    m_iface->EraseView();

    m_state = IDLE;
    m_world->KillChildren();
    m_world->ClearRanks();
}


void ROUTER::FlipPosture()
{
    if( m_state == ROUTE_TRACK )
    {
        m_placer->FlipPosture();
    }
}


bool ROUTER::SwitchLayer( int aLayer )
{
    if( m_state == ROUTE_TRACK )
        return m_placer->SetLayer( aLayer );

    return false;
}


void ROUTER::ToggleViaPlacement()
{
    if( m_state == ROUTE_TRACK )
    {
        bool toggle = !m_placer->IsPlacingVia();
        m_placer->ToggleVia( toggle );
    }
}


const std::vector<int> ROUTER::GetCurrentNets() const
{
    if( m_placer )
        return m_placer->CurrentNets();
    else if( m_dragger )
        return m_dragger->CurrentNets();

    return std::vector<int>();
}


int ROUTER::GetCurrentLayer() const
{
    if( m_placer )
        return m_placer->CurrentLayer();
    else if( m_dragger )
        return m_dragger->CurrentLayer();

    return -1;
}


LOGGER* ROUTER::Logger()
{
    return m_logger;
}


bool ROUTER::IsPlacingVia() const
{
    if( !m_placer )
        return false;

    return m_placer->IsPlacingVia();
}


void ROUTER::ToggleRounded()
{
    CORNER_MODE newMode = CORNER_MODE::MITERED_45;

    switch( m_settings->GetCornerMode() )
    {
    case CORNER_MODE::MITERED_45:
        newMode = CORNER_MODE::ROUNDED_45;
        break;

    default:
        break;
    }

    m_settings->SetCornerMode( newMode );
}


void ROUTER::SetOrthoMode( bool aEnable )
{
    if( !m_placer )
        return;

    m_placer->SetOrthoMode( aEnable );
}


void ROUTER::SetMode( ROUTER_MODE aMode )
{
    m_mode = aMode;
}


void ROUTER::SetInterface( ROUTER_IFACE *aIface )
{
    m_iface = aIface;
}

void ROUTER::BreakSegment( ITEM *aItem, const VECTOR2I& aP )
{
    NODE *node = m_world->Branch();

    LINE_PLACER placer( this );

    if( placer.SplitAdjacentSegments( node, aItem, aP ) )
    {
        CommitRouting( node );
    }
    else
    {
        delete node;
    }

}

}

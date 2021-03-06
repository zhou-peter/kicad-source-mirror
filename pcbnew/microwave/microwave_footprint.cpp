/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2017-2020 KiCad Developers, see AUTHORS.txt for contributors.
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


#include <class_module.h>
#include <confirm.h>
#include <dialog_text_entry.h>
#include <microwave/microwave_tool.h>


MODULE* MICROWAVE_TOOL::createFootprint( MICROWAVE_FOOTPRINT_SHAPE aFootprintShape )
{
    int      oX;
    D_PAD*   pad;
    MODULE*  module;
    wxString msg, cmp_name;
    int      pad_count = 2;
    int      angle     = 0;

    PCB_EDIT_FRAME& editFrame  = *getEditFrame<PCB_EDIT_FRAME>();

    // Ref and value text size (O = use board default value.
    // will be set to a value depending on the footprint size, if possible
    int text_size = 0;

    // Enter the size of the gap or stub
    int gap_size = editFrame.GetDesignSettings().GetCurrentTrackWidth();

    switch( aFootprintShape )
    {
    case MICROWAVE_FOOTPRINT_SHAPE::GAP:
        msg       = _( "Gap Size:" );
        cmp_name  = "muwave_gap";
        text_size = gap_size;
        break;

    case MICROWAVE_FOOTPRINT_SHAPE::STUB:
        msg       = _( "Stub Size:" );
        cmp_name  = "muwave_stub";
        text_size = gap_size;
        pad_count = 2;
        break;

    case MICROWAVE_FOOTPRINT_SHAPE::STUB_ARC:
        msg       = _( "Arc Stub Radius Value:" );
        cmp_name  = "muwave_arcstub";
        pad_count = 1;
        break;

    default:
        msg = wxT( "???" );
        break;
    }

    wxString value = StringFromValue( editFrame.GetUserUnits(), gap_size );
    WX_TEXT_ENTRY_DIALOG dlg( &editFrame, msg, _( "Create microwave module" ), value );

    if( dlg.ShowModal() != wxID_OK )
        return NULL; // cancelled by user

    value    = dlg.GetValue();
    gap_size = ValueFromString( editFrame.GetUserUnits(), value );

    bool abort = false;

    if( aFootprintShape == MICROWAVE_FOOTPRINT_SHAPE::STUB_ARC )
    {
        double            fcoeff = 10.0, fval;
        msg.Printf( wxT( "%3.1f" ), angle / fcoeff );
        WX_TEXT_ENTRY_DIALOG angledlg( &editFrame, _( "Angle in degrees:" ),
                                       _( "Create microwave module" ), msg );

        if( angledlg.ShowModal() != wxID_OK )
            return NULL; // cancelled by user

        msg = angledlg.GetValue();

        if( !msg.ToDouble( &fval ) )
        {
            DisplayError( &editFrame, _( "Incorrect number, abort" ) );
            abort = true;
        }

        angle = std::abs( KiROUND( fval * fcoeff ) );

        if( angle > 1800 )
            angle = 1800;
    }

    if( abort )
        return NULL;

    module = createBaseFootprint( cmp_name, text_size, pad_count );
    auto it = module->Pads().begin();
    pad = *it;

    switch( aFootprintShape )
    {
    case MICROWAVE_FOOTPRINT_SHAPE::GAP:     //Gap :
        oX = -( gap_size + pad->GetSize().x ) / 2;
        pad->SetX0( oX );

        pad->SetX( pad->GetPos0().x + pad->GetPosition().x );

        pad = *( it + 1 );

        pad->SetX0( oX + gap_size + pad->GetSize().x );
        pad->SetX( pad->GetPos0().x + pad->GetPosition().x );
        break;

    case MICROWAVE_FOOTPRINT_SHAPE::STUB:     //Stub :
        pad->SetName( wxT( "1" ) );
        pad = *( it + 1 );
        pad->SetY0( -( gap_size + pad->GetSize().y ) / 2 );
        pad->SetSize( wxSize( pad->GetSize().x, gap_size ) );
        pad->SetY( pad->GetPos0().y + pad->GetPosition().y );
        break;

    case MICROWAVE_FOOTPRINT_SHAPE::STUB_ARC:     // Arc Stub created by a polygonal approach:
    {
        pad->SetShape( PAD_SHAPE_CUSTOM );
        pad->SetAnchorPadShape( PAD_SHAPE_RECT );

        int numPoints = (angle / 50) + 3;     // Note: angles are in 0.1 degrees
        std::vector<wxPoint> polyPoints;
        polyPoints.reserve( numPoints );

        polyPoints.emplace_back( wxPoint( 0, 0 ) );

        int theta = -angle / 2;

        for( int ii = 1; ii < numPoints - 1; ii++ )
        {
            wxPoint pt( 0, -gap_size );
            RotatePoint( &pt.x, &pt.y, theta );
            polyPoints.push_back( pt );

            theta += 50;

            if( theta > angle / 2 )
                theta = angle / 2;
        }

        // Close the polygon:
        polyPoints.push_back( polyPoints[0] );

        pad->AddPrimitivePoly( polyPoints, 0 ); // add a polygonal basic shape
    }
        break;

    default:
        break;
    }

    // Update the module and board
    module->CalculateBoundingBox();
    editFrame.OnModify();

    return module;
}


MODULE* MICROWAVE_TOOL::createBaseFootprint( const wxString& aValue,
                                             int aTextSize, int aPadCount )
{
    PCB_EDIT_FRAME& editFrame = *getEditFrame<PCB_EDIT_FRAME>();

    MODULE* module = editFrame.CreateNewModule( aValue );

    if( aTextSize > 0 )
    {
        module->Reference().SetTextSize( wxSize( aTextSize, aTextSize ) );
        module->Reference().SetTextThickness( aTextSize / 5 );
        module->Value().SetTextSize( wxSize( aTextSize, aTextSize ) );
        module->Value().SetTextThickness( aTextSize / 5 );
    }

    // Create 2 pads used in gaps and stubs.  The gap is between these 2 pads
    // the stub is the pad 2
    wxString Line;
    int pad_num = 1;

    while( aPadCount-- )
    {
        D_PAD* pad = new D_PAD( module );

        module->Add( pad, ADD_MODE::INSERT );

        int tw = editFrame.GetDesignSettings().GetCurrentTrackWidth();
        pad->SetSize( wxSize( tw, tw ) );

        pad->SetPosition( module->GetPosition() );
        pad->SetShape( PAD_SHAPE_RECT );
        pad->SetAttribute( PAD_ATTRIB_SMD );
        pad->SetLayerSet( F_Cu );

        Line.Printf( wxT( "%d" ), pad_num );
        pad->SetName( Line );
        pad_num++;
    }

    return module;
}

/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2020 KiCad Developers, see AUTHORS.txt for contributors.
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

#include <sch_edit_frame.h>
#include <sch_painter.h>
#include <class_libentry.h>
#include <panel_setup_formatting.h>
#include <sch_junction.h>
#include <schematic.h>
#include <schematic_settings.h>


PANEL_SETUP_FORMATTING::PANEL_SETUP_FORMATTING( wxWindow* aWindow, SCH_EDIT_FRAME* aFrame  ) :
        PANEL_SETUP_FORMATTING_BASE( aWindow ),
        m_frame( aFrame ),
        m_textSize( aFrame, m_textSizeLabel, m_textSizeCtrl, m_textSizeUnits, true ),
        m_lineWidth( aFrame, m_lineWidthLabel, m_lineWidthCtrl, m_lineWidthUnits, true ),
        m_pinSymbolSize( aFrame, m_pinSymbolSizeLabel, m_pinSymbolSizeCtrl, m_pinSymbolSizeUnits, true ),
        m_junctionSize( aFrame, m_jctSizeLabel, m_jctSizeCtrl, m_jctSizeUnits, true )
{
}


bool PANEL_SETUP_FORMATTING::TransferDataToWindow()
{
    SCHEMATIC_SETTINGS& settings = m_frame->Schematic().Settings();

    // Reference style one of: "A" ".A" "-A" "_A" ".1" "-1" "_1"
    int refStyleSelection;

    switch( LIB_PART::GetSubpartIdSeparator() )
    {
    default:
    case 0:   refStyleSelection = 0; break;
    case '.': refStyleSelection = LIB_PART::GetSubpartFirstId() == '1' ? 4 : 1; break;
    case '-': refStyleSelection = LIB_PART::GetSubpartFirstId() == '1' ? 5 : 2; break;
    case '_': refStyleSelection = LIB_PART::GetSubpartFirstId() == '1' ? 6 : 3; break;
    }

    m_choiceSeparatorRefId->SetSelection( refStyleSelection );

    m_textSize.SetUnits( EDA_UNITS::MILS );
    m_lineWidth.SetUnits( EDA_UNITS::MILS );
    m_pinSymbolSize.SetUnits( EDA_UNITS::MILS );
    m_junctionSize.SetUnits( EDA_UNITS::MILS );

    m_textSize.SetValue( settings.m_DefaultTextSize );
    m_lineWidth.SetValue( settings.m_DefaultLineWidth );
    m_pinSymbolSize.SetValue( settings.m_PinSymbolSize );
    m_junctionSize.SetValue( settings.m_JunctionSize );

    m_showIntersheetsReferences->SetValue( settings.m_IntersheetsRefShow );
    m_radioFormatStandard->SetValue( !settings.m_IntersheetsRefFormatShort );
    m_radioFormatAbbreviated->SetValue( settings.m_IntersheetsRefFormatShort );
    m_prefixCtrl->ChangeValue( settings.m_IntersheetsRefPrefix );
    m_suffixCtrl->ChangeValue( settings.m_IntersheetsRefSuffix );

    wxString offsetRatio = wxString::Format( "%f", settings.m_TextOffsetRatio * 100.0 );
    m_textOffsetRatioCtrl->SetValue( offsetRatio );

    return true;
}


bool PANEL_SETUP_FORMATTING::TransferDataFromWindow()
{
    SCHEMATIC_SETTINGS& settings = m_frame->Schematic().Settings();

    // Reference style one of: "A" ".A" "-A" "_A" ".1" "-1" "_1"
    int firstRefId, refSeparator;

    switch( m_choiceSeparatorRefId->GetSelection() )
    {
    default:
    case 0: firstRefId = 'A'; refSeparator = 0; break;
    case 1: firstRefId = 'A'; refSeparator = '.'; break;
    case 2: firstRefId = 'A'; refSeparator = '-'; break;
    case 3: firstRefId = 'A'; refSeparator = '_'; break;
    case 4: firstRefId = '1'; refSeparator = '.'; break;
    case 5: firstRefId = '1'; refSeparator = '-'; break;
    case 6: firstRefId = '1'; refSeparator = '_'; break;
    }

    if( refSeparator != LIB_PART::GetSubpartIdSeparator() ||
        firstRefId != LIB_PART::GetSubpartFirstId() )
    {
        LIB_PART::SetSubpartIdNotation( refSeparator, firstRefId );
    }

    settings.m_DefaultTextSize = (int) m_textSize.GetValue();
    settings.m_DefaultLineWidth = (int) m_lineWidth.GetValue();
    settings.m_PinSymbolSize = (int) m_pinSymbolSize.GetValue();
    settings.m_JunctionSize = (int) m_junctionSize.GetValue();

    settings.m_IntersheetsRefShow        = m_showIntersheetsReferences->GetValue();
    settings.m_IntersheetsRefFormatShort = !m_radioFormatStandard->GetValue();
    settings.m_IntersheetsRefPrefix      = m_prefixCtrl->GetValue();
    settings.m_IntersheetsRefSuffix      = m_suffixCtrl->GetValue();

    double dtmp = 0.0;
    wxString msg = m_textOffsetRatioCtrl->GetValue();
    msg.ToDouble( &dtmp );
    settings.m_TextOffsetRatio = dtmp / 100.0;

    m_frame->GetRenderSettings()->SetDefaultPenWidth( settings.m_DefaultLineWidth );
    m_frame->GetRenderSettings()->m_TextOffsetRatio      = settings.m_TextOffsetRatio;
    m_frame->GetRenderSettings()->m_PinSymbolSize        = settings.m_PinSymbolSize;
    m_frame->GetRenderSettings()->m_JunctionSize         = settings.m_JunctionSize;

    m_frame->GetCanvas()->GetView()->MarkDirty();
    m_frame->GetCanvas()->GetView()->UpdateAllItems( KIGFX::REPAINT );
    m_frame->GetCanvas()->Refresh();

    return true;
}


void PANEL_SETUP_FORMATTING::ImportSettingsFrom( SCHEMATIC_SETTINGS& aSettings )
{
    m_textSize.SetValue( aSettings.m_DefaultTextSize );
    m_lineWidth.SetValue( aSettings.m_DefaultLineWidth );
    m_pinSymbolSize.SetValue( aSettings.m_PinSymbolSize );
    m_junctionSize.SetValue( aSettings.m_JunctionSize );

    m_showIntersheetsReferences->SetValue( aSettings.m_IntersheetsRefShow );
    m_radioFormatStandard->SetValue( aSettings.m_IntersheetsRefFormatShort );
    m_radioFormatAbbreviated->SetValue( !aSettings.m_IntersheetsRefFormatShort );
    m_prefixCtrl->ChangeValue( aSettings.m_IntersheetsRefPrefix );
    m_suffixCtrl->ChangeValue( aSettings.m_IntersheetsRefSuffix );

    wxString offsetRatio = wxString::Format( "%f", aSettings.m_TextOffsetRatio * 100.0 );
    m_textOffsetRatioCtrl->SetValue( offsetRatio );
}



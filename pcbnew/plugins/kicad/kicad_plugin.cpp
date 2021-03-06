/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2012 CERN
 * Copyright (C) 1992-2020 KiCad Developers, see AUTHORS.txt for contributors.
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

#include <build_version.h>      // LEGACY_BOARD_FILE_VERSION
#include <wildcards_and_files_ext.h>
#include <advanced_config.h>
#include <base_units.h>
#include <trace_helpers.h>
#include <class_board.h>
#include <class_module.h>
#include <pcb_text.h>
#include <class_dimension.h>
#include <class_track.h>
#include <class_zone.h>
#include <pcb_shape.h>
#include <class_pcb_target.h>
#include <fp_shape.h>
#include <confirm.h>
#include <zones.h>
#include <plugins/kicad/kicad_plugin.h>
#include <plugins/kicad/pcb_parser.h>
#include <pcbnew_settings.h>
#include <boost/ptr_container/ptr_map.hpp>
#include <convert_basic_shapes_to_polygon.h>    // for enum RECT_CHAMFER_POSITIONS definition
#include <kiface_i.h>

using namespace PCB_KEYS_T;


/**
 * Helper class for creating a footprint library cache.
 *
 * The new footprint library design is a file path of individual module files
 * that contain a single module per file.  This class is a helper only for the
 * footprint portion of the PLUGIN API, and only for the #PCB_IO plugin.  It is
 * private to this implementation file so it is not placed into a header.
 */
class FP_CACHE_ITEM
{
    WX_FILENAME             m_filename;
    std::unique_ptr<MODULE> m_module;

public:
    FP_CACHE_ITEM( MODULE* aModule, const WX_FILENAME& aFileName );

    const WX_FILENAME& GetFileName() const { return m_filename; }
    const MODULE*      GetModule()   const { return m_module.get(); }
};


FP_CACHE_ITEM::FP_CACHE_ITEM( MODULE* aModule, const WX_FILENAME& aFileName ) :
    m_filename( aFileName ),
    m_module( aModule )
{ }


typedef boost::ptr_map< wxString, FP_CACHE_ITEM >   MODULE_MAP;
typedef MODULE_MAP::iterator                        MODULE_ITER;
typedef MODULE_MAP::const_iterator                  MODULE_CITER;


class FP_CACHE
{
    PCB_IO*         m_owner;            // Plugin object that owns the cache.
    wxFileName      m_lib_path;         // The path of the library.
    wxString        m_lib_raw_path;     // For quick comparisons.
    MODULE_MAP      m_modules;          // Map of footprint file name per MODULE*.

    bool            m_cache_dirty;      // Stored separately because it's expensive to check
                                        // m_cache_timestamp against all the files.
    long long       m_cache_timestamp;  // A hash of the timestamps for all the footprint
                                        // files.

public:
    FP_CACHE( PCB_IO* aOwner, const wxString& aLibraryPath );

    wxString    GetPath() const { return m_lib_raw_path; }
    bool        IsWritable() const { return m_lib_path.IsOk() && m_lib_path.IsDirWritable(); }
    bool        Exists() const { return m_lib_path.IsOk() && m_lib_path.DirExists(); }
    MODULE_MAP& GetModules() { return m_modules; }

    // Most all functions in this class throw IO_ERROR exceptions.  There are no
    // error codes nor user interface calls from here, nor in any PLUGIN.
    // Catch these exceptions higher up please.

    /**
     * Save the footprint cache or a single module from it to disk
     *
     * @param aModule if set, save only this module, otherwise, save the full library
     */
    void Save( MODULE* aModule = NULL );

    void Load();

    void Remove( const wxString& aFootprintName );

    /**
     * Generate a timestamp representing all source files in the cache (including the
     * parent directory).
     * Timestamps should not be considered ordered.  They either match or they don't.
     */
    static long long GetTimestamp( const wxString& aLibPath );

    /**
     * Return true if the cache is not up-to-date.
     */
    bool IsModified();

    /**
     * Check if \a aPath is the same as the current cache path.
     *
     * This tests paths by converting \a aPath using the native separators.  Internally
     * #FP_CACHE stores the current path using native separators.  This prevents path
     * miscompares on Windows due to the fact that paths can be stored with / instead of \\
     * in the footprint library table.
     *
     * @param aPath is the library path to test against.
     * @return true if \a aPath is the same as the cache path.
     */
    bool IsPath( const wxString& aPath ) const;
};


FP_CACHE::FP_CACHE( PCB_IO* aOwner, const wxString& aLibraryPath )
{
    m_owner = aOwner;
    m_lib_raw_path = aLibraryPath;
    m_lib_path.SetPath( aLibraryPath );
    m_cache_timestamp = 0;
    m_cache_dirty = true;
}


void FP_CACHE::Save( MODULE* aModule )
{
    m_cache_timestamp = 0;

    if( !m_lib_path.DirExists() && !m_lib_path.Mkdir() )
    {
        THROW_IO_ERROR( wxString::Format( _( "Cannot create footprint library path \"%s\"" ),
                                          m_lib_raw_path ) );
    }

    if( !m_lib_path.IsDirWritable() )
    {
        THROW_IO_ERROR( wxString::Format( _( "Footprint library path \"%s\" is read only" ),
                                          m_lib_raw_path ) );
    }

    for( MODULE_ITER it = m_modules.begin();  it != m_modules.end();  ++it )
    {
        if( aModule && aModule != it->second->GetModule() )
            continue;

        WX_FILENAME fn = it->second->GetFileName();

        wxString tempFileName =
#ifdef USE_TMP_FILE
        wxFileName::CreateTempFileName( fn.GetPath() );
#else
        fn.GetFullPath();
#endif
        // Allow file output stream to go out of scope to close the file stream before
        // renaming the file.
        {
            wxLogTrace( traceKicadPcbPlugin, wxT( "Creating temporary library file %s" ),
                        GetChars( tempFileName ) );

            FILE_OUTPUTFORMATTER formatter( tempFileName );

            m_owner->SetOutputFormatter( &formatter );
            m_owner->Format( (BOARD_ITEM*) it->second->GetModule() );
        }

#ifdef USE_TMP_FILE
        wxRemove( fn.GetFullPath() );     // it is not an error if this does not exist

        // Even on Linux you can see an _intermittent_ error when calling wxRename(),
        // and it is fully inexplicable.  See if this dodges the error.
        wxMilliSleep( 250L );

        if( !wxRenameFile( tempFileName, fn.GetFullPath() ) )
        {
            wxString msg = wxString::Format(
                    _( "Cannot rename temporary file \"%s\" to footprint library file \"%s\"" ),
                    GetChars( tempFileName ),
                    GetChars( fn.GetFullPath() )
                    );
            THROW_IO_ERROR( msg );
        }
#endif
        m_cache_timestamp += fn.GetTimestamp();
    }

    m_cache_timestamp += m_lib_path.GetModificationTime().GetValue().GetValue();

    // If we've saved the full cache, we clear the dirty flag.
    if( !aModule )
        m_cache_dirty = false;
}


void FP_CACHE::Load()
{
    m_cache_dirty = false;
    m_cache_timestamp = 0;

    wxDir dir( m_lib_raw_path );

    if( !dir.IsOpened() )
    {
        wxString msg = wxString::Format( _( "Footprint library path '%s' does not exist "
                                            "(or is not a directory)." ),
                                         m_lib_raw_path );
        THROW_IO_ERROR( msg );
    }

    wxString fullName;
    wxString fileSpec = wxT( "*." ) + KiCadFootprintFileExtension;

    // wxFileName construction is egregiously slow.  Construct it once and just swap out
    // the filename thereafter.
    WX_FILENAME fn( m_lib_raw_path, wxT( "dummyName" ) );

    if( dir.GetFirst( &fullName, fileSpec ) )
    {
        wxString cacheError;

        do
        {
            fn.SetFullName( fullName );

            // Queue I/O errors so only files that fail to parse don't get loaded.
            try
            {
                FILE_LINE_READER    reader( fn.GetFullPath() );

                m_owner->m_parser->SetLineReader( &reader );

                MODULE*     footprint = (MODULE*) m_owner->m_parser->Parse();
                wxString    fpName = fn.GetName();

                footprint->SetFPID( LIB_ID( wxEmptyString, fpName ) );
                m_modules.insert( fpName, new FP_CACHE_ITEM( footprint, fn ) );

                m_cache_timestamp += fn.GetTimestamp();
            }
            catch( const IO_ERROR& ioe )
            {
                if( !cacheError.IsEmpty() )
                    cacheError += "\n\n";

                cacheError += ioe.What();
            }
        } while( dir.GetNext( &fullName ) );

        if( !cacheError.IsEmpty() )
            THROW_IO_ERROR( cacheError );
    }
}


void FP_CACHE::Remove( const wxString& aFootprintName )
{
    MODULE_CITER it = m_modules.find( aFootprintName );

    if( it == m_modules.end() )
    {
        wxString msg = wxString::Format( _( "library \"%s\" has no footprint \"%s\" to delete" ),
                                         m_lib_raw_path,
                                         aFootprintName );
        THROW_IO_ERROR( msg );
    }

    // Remove the module from the cache and delete the module file from the library.
    wxString fullPath = it->second->GetFileName().GetFullPath();
    m_modules.erase( aFootprintName );
    wxRemoveFile( fullPath );
}


bool FP_CACHE::IsPath( const wxString& aPath ) const
{
    return aPath == m_lib_raw_path;
}


bool FP_CACHE::IsModified()
{
    m_cache_dirty = m_cache_dirty || GetTimestamp( m_lib_path.GetFullPath() ) != m_cache_timestamp;

    return m_cache_dirty;
}


long long FP_CACHE::GetTimestamp( const wxString& aLibPath )
{
    wxString fileSpec = wxT( "*." ) + KiCadFootprintFileExtension;

    return TimestampDir( aLibPath, fileSpec );
}


void PCB_IO::Save( const wxString& aFileName, BOARD* aBoard, const PROPERTIES* aProperties )
{
    LOCALE_IO   toggle;     // toggles on, then off, the C locale.

    wxString sanityResult = aBoard->GroupsSanityCheck();

    if( sanityResult != wxEmptyString )
    {
        KIDIALOG dlg( nullptr, wxString::Format(
             _( "Please report this bug.  Error validating group structure: %s"
                "\n\nSave anyway?" ), sanityResult ),
                      _( "Internal group data structure corrupt" ),
                      wxOK | wxCANCEL | wxICON_ERROR );
        dlg.SetOKLabel( _( "Save Anyway" ) );

        if( dlg.ShowModal() == wxID_CANCEL )
            return;
    }

    init( aProperties );

    m_board = aBoard;       // after init()

    // Prepare net mapping that assures that net codes saved in a file are consecutive integers
    m_mapping->SetBoard( aBoard );

    FILE_OUTPUTFORMATTER    formatter( aFileName );

    m_out = &formatter;     // no ownership

    m_out->Print( 0, "(kicad_pcb (version %d) (generator pcbnew)\n", SEXPR_BOARD_FILE_VERSION );

    Format( aBoard, 1 );

    m_out->Print( 0, ")\n" );
}


BOARD_ITEM* PCB_IO::Parse( const wxString& aClipboardSourceInput )
{
    std::string input = TO_UTF8( aClipboardSourceInput );

    STRING_LINE_READER  reader( input, wxT( "clipboard" ) );

    m_parser->SetLineReader( &reader );

    try
    {
        return m_parser->Parse();
    }
    catch( const PARSE_ERROR& parse_error )
    {
        if( m_parser->IsTooRecent() )
            throw FUTURE_FORMAT_ERROR( parse_error, m_parser->GetRequiredVersion() );
        else
            throw;
    }
}


void PCB_IO::Format( BOARD_ITEM* aItem, int aNestLevel ) const
{
    LOCALE_IO   toggle;     // public API function, perform anything convenient for caller

    switch( aItem->Type() )
    {
    case PCB_T:
        format( static_cast<BOARD*>( aItem ), aNestLevel );
        break;

    case PCB_DIM_ALIGNED_T:
    case PCB_DIM_CENTER_T:
    case PCB_DIM_ORTHOGONAL_T:
    case PCB_DIM_LEADER_T:
        format( static_cast<DIMENSION*>( aItem ), aNestLevel );
        break;

    case PCB_SHAPE_T:
        format( static_cast<PCB_SHAPE*>( aItem ), aNestLevel );
        break;

    case PCB_FP_SHAPE_T:
        format( static_cast<FP_SHAPE*>( aItem ), aNestLevel );
        break;

    case PCB_TARGET_T:
        format( static_cast<PCB_TARGET*>( aItem ), aNestLevel );
        break;

    case PCB_MODULE_T:
        format( static_cast<MODULE*>( aItem ), aNestLevel );
        break;

    case PCB_PAD_T:
        format( static_cast<D_PAD*>( aItem ), aNestLevel );
        break;

    case PCB_TEXT_T:
        format( static_cast<PCB_TEXT*>( aItem ), aNestLevel );
        break;

    case PCB_FP_TEXT_T:
        format( static_cast<FP_TEXT*>( aItem ), aNestLevel );
        break;

    case PCB_GROUP_T:
        format( static_cast<PCB_GROUP*>( aItem ), aNestLevel );
        break;

    case PCB_TRACE_T:
    case PCB_ARC_T:
    case PCB_VIA_T:
        format( static_cast<TRACK*>( aItem ), aNestLevel );
        break;

    case PCB_FP_ZONE_AREA_T:
    case PCB_ZONE_AREA_T:
        format( static_cast<ZONE_CONTAINER*>( aItem ), aNestLevel );
        break;

    default:
        wxFAIL_MSG( wxT( "Cannot format item " ) + aItem->GetClass() );
    }
}


void PCB_IO::formatLayer( const BOARD_ITEM* aItem ) const
{
    PCB_LAYER_ID layer = aItem->GetLayer();

    m_out->Print( 0, " (layer %s)", m_out->Quotew( LSET::Name( layer ) ).c_str() );
}


void PCB_IO::formatSetup( BOARD* aBoard, int aNestLevel ) const
{
    // Setup
    m_out->Print( aNestLevel, "(setup\n" );

    // Save the board physical stackup structure
    BOARD_STACKUP& stackup = aBoard->GetDesignSettings().GetStackupDescriptor();

    if( aBoard->GetDesignSettings().m_HasStackup )
        stackup.FormatBoardStackup( m_out, aBoard, aNestLevel+1 );

    BOARD_DESIGN_SETTINGS& dsnSettings = aBoard->GetDesignSettings();

    if( dsnSettings.m_AuxOrigin != wxPoint( 0, 0 ) )
        m_out->Print( aNestLevel+1, "(aux_axis_origin %s %s)\n",
                      FormatInternalUnits( dsnSettings.m_AuxOrigin.x ).c_str(),
                      FormatInternalUnits( dsnSettings.m_AuxOrigin.y ).c_str() );

    if( dsnSettings.m_GridOrigin != wxPoint( 0, 0 ) )
        m_out->Print( aNestLevel+1, "(grid_origin %s %s)\n",
                      FormatInternalUnits( dsnSettings.m_GridOrigin.x ).c_str(),
                      FormatInternalUnits( dsnSettings.m_GridOrigin.y ).c_str() );

    aBoard->GetPlotOptions().Format( m_out, aNestLevel+1 );

    m_out->Print( aNestLevel, ")\n\n" );
}


void PCB_IO::formatGeneral( BOARD* aBoard, int aNestLevel ) const
{
    const BOARD_DESIGN_SETTINGS& dsnSettings = aBoard->GetDesignSettings();

    m_out->Print( 0, "\n" );
    m_out->Print( aNestLevel, "(general\n" );
    m_out->Print( aNestLevel+1, "(thickness %s)\n",
                  FormatInternalUnits( dsnSettings.GetBoardThickness() ).c_str() );

    m_out->Print( aNestLevel, ")\n\n" );

    aBoard->GetPageSettings().Format( m_out, aNestLevel, m_ctl );
    aBoard->GetTitleBlock().Format( m_out, aNestLevel, m_ctl );
}


void PCB_IO::formatBoardLayers( BOARD* aBoard, int aNestLevel ) const
{
    m_out->Print( aNestLevel, "(layers\n" );

    // Save only the used copper layers from front to back.

    for( LSEQ cu = aBoard->GetEnabledLayers().CuStack();  cu;  ++cu )
    {
        PCB_LAYER_ID layer = *cu;

        m_out->Print( aNestLevel+1, "(%d %s %s", layer,
                      m_out->Quotew( LSET::Name( layer ) ).c_str(),
                      LAYER::ShowType( aBoard->GetLayerType( layer ) ) );

        if( LSET::Name( layer ) != m_board->GetLayerName( layer ) )
            m_out->Print( 0, " %s", m_out->Quotew( m_board->GetLayerName( layer ) ).c_str() );

        m_out->Print( 0, ")\n" );
    }

    // Save used non-copper layers in the order they are defined.
    // desired sequence for non Cu BOARD layers.
    static const PCB_LAYER_ID non_cu[] =
    {
        B_Adhes,        // 32
        F_Adhes,
        B_Paste,
        F_Paste,
        B_SilkS,
        F_SilkS,
        B_Mask,
        F_Mask,
        Dwgs_User,
        Cmts_User,
        Eco1_User,
        Eco2_User,
        Edge_Cuts,
        Margin,
        B_CrtYd,
        F_CrtYd,
        B_Fab,
        F_Fab,
        User_1,
        User_2,
        User_3,
        User_4,
        User_5,
        User_6,
        User_7,
        User_8,
        User_9
    };

    for( LSEQ seq = aBoard->GetEnabledLayers().Seq( non_cu, arrayDim( non_cu ) );  seq;  ++seq )
    {
        PCB_LAYER_ID layer = *seq;

        m_out->Print( aNestLevel+1, "(%d %s user", layer,
                      m_out->Quotew( LSET::Name( layer ) ).c_str() );

        if( m_board->GetLayerName( layer ) != LSET::Name( layer ) )
            m_out->Print( 0, " %s", m_out->Quotew( m_board->GetLayerName( layer ) ).c_str() );

        m_out->Print( 0, ")\n" );
    }

    m_out->Print( aNestLevel, ")\n\n" );
}


void PCB_IO::formatNetInformation( BOARD* aBoard, int aNestLevel ) const
{
    for( NETINFO_ITEM* net : *m_mapping )
    {
        if( net == nullptr )    // Skip not actually existing nets (orphan nets)
            continue;

        m_out->Print( aNestLevel, "(net %d %s)\n",
                                  m_mapping->Translate( net->GetNet() ),
                                  m_out->Quotew( net->GetNetname() ).c_str() );
    }

    m_out->Print( 0, "\n" );
}


void PCB_IO::formatProperties( BOARD* aBoard, int aNestLevel ) const
{
    for( const std::pair<const wxString, wxString>& prop : aBoard->GetProperties() )
    {
        m_out->Print( aNestLevel, "(property %s %s)\n",
                      m_out->Quotew( prop.first ).c_str(),
                      m_out->Quotew( prop.second ).c_str() );
    }

    m_out->Print( 0, "\n" );
}


void PCB_IO::formatHeader( BOARD* aBoard, int aNestLevel ) const
{
    formatGeneral( aBoard, aNestLevel );
    // Layers list.
    formatBoardLayers( aBoard, aNestLevel );

    // Setup
    formatSetup( aBoard, aNestLevel );

    // Properties
    formatProperties( aBoard, aNestLevel );

    // Save net codes and names
    formatNetInformation( aBoard, aNestLevel );
}


void PCB_IO::format( BOARD* aBoard, int aNestLevel ) const
{
    std::set<BOARD_ITEM*, BOARD_ITEM::ptr_cmp> sorted_modules( aBoard->Modules().begin(),
            aBoard->Modules().end() );
    std::set<BOARD_ITEM*, BOARD_ITEM::ptr_cmp> sorted_drawings( aBoard->Drawings().begin(),
            aBoard->Drawings().end() );
    std::set<TRACK*, TRACK::cmp_tracks> sorted_tracks( aBoard->Tracks().begin(),
            aBoard->Tracks().end() );
    std::set<BOARD_ITEM*, BOARD_ITEM::ptr_cmp> sorted_zones( aBoard->Zones().begin(),
            aBoard->Zones().end() );
    std::set<BOARD_ITEM*, BOARD_ITEM::ptr_cmp> sorted_groups( aBoard->Groups().begin(),
            aBoard->Groups().end() );

    formatHeader( aBoard, aNestLevel );

    // Save the modules.
    for( BOARD_ITEM* module : sorted_modules )
    {
        Format( module, aNestLevel );
        m_out->Print( 0, "\n" );
    }

    // Save the graphical items on the board (not owned by a module)
    for( BOARD_ITEM* item : sorted_drawings )
        Format( item, aNestLevel );

    if( sorted_drawings.size() )
        m_out->Print( 0, "\n" );

    // Do not save MARKER_PCBs, they can be regenerated easily.

    // Save the tracks and vias.
    for( TRACK* track : sorted_tracks )
        Format( track, aNestLevel );

    if( sorted_tracks.size() )
        m_out->Print( 0, "\n" );

    // Save the polygon (which are the newer technology) zones.
    for( auto zone : sorted_zones )
        Format( zone, aNestLevel );

    // Save the groups
    for( BOARD_ITEM* group : sorted_groups )
        Format( group, aNestLevel );
}


void PCB_IO::format( DIMENSION* aDimension, int aNestLevel ) const
{
    ALIGNED_DIMENSION*    aligned = dynamic_cast<ALIGNED_DIMENSION*>( aDimension );
    ORTHOGONAL_DIMENSION* ortho   = dynamic_cast<ORTHOGONAL_DIMENSION*>( aDimension );
    CENTER_DIMENSION*     center  = dynamic_cast<CENTER_DIMENSION*>( aDimension );
    LEADER*               leader  = dynamic_cast<LEADER*>( aDimension );

    m_out->Print( aNestLevel, "(dimension" );

    if( aDimension->Type() == PCB_DIM_ALIGNED_T )
        m_out->Print( 0, " (type aligned)" );
    else if( aDimension->Type() == PCB_DIM_LEADER_T )
        m_out->Print( 0, " (type leader)" );
    else if( aDimension->Type() == PCB_DIM_CENTER_T )
        m_out->Print( 0, " (type center)" );
    else if( aDimension->Type() == PCB_DIM_ORTHOGONAL_T )
        m_out->Print( 0, " (type orthogonal)" );
    else
        wxFAIL_MSG( wxT( "Cannot format unknown dimension type!" ) );

    formatLayer( aDimension );

    m_out->Print( 0, " (tstamp %s)", TO_UTF8( aDimension->m_Uuid.AsString() ) );

    m_out->Print( 0, "\n" );

    m_out->Print( aNestLevel+1, "(pts (xy %s %s) (xy %s %s))\n",
                  FormatInternalUnits( aDimension->GetStart().x ).c_str(),
                  FormatInternalUnits( aDimension->GetStart().y ).c_str(),
                  FormatInternalUnits( aDimension->GetEnd().x ).c_str(),
                  FormatInternalUnits( aDimension->GetEnd().y ).c_str() );

    if( aligned )
        m_out->Print( aNestLevel+1, "(height %s)\n",
                      FormatInternalUnits( aligned->GetHeight() ).c_str() );

    if( ortho )
        m_out->Print( aNestLevel+1, "(orientation %d)\n",
                      static_cast<int>( ortho->GetOrientation() ) );

    if( !center )
    {
        Format( &aDimension->Text(), aNestLevel + 1 );

        m_out->Print( aNestLevel + 1, "(format" );

        if( !aDimension->GetPrefix().IsEmpty() )
            m_out->Print( 0, " (prefix \"%s\")", TO_UTF8( aDimension->GetPrefix() ) );

        if( !aDimension->GetSuffix().IsEmpty() )
            m_out->Print( 0, " (suffix \"%s\")", TO_UTF8( aDimension->GetSuffix() ) );

        m_out->Print( 0, " (units %d) (units_format %d) (precision %d)",
                static_cast<int>( aDimension->GetUnitsMode() ),
                static_cast<int>( aDimension->GetUnitsFormat() ), aDimension->GetPrecision() );

        if( aDimension->GetOverrideTextEnabled() )
            m_out->Print( 0, " (override_value \"%s\")", TO_UTF8( aDimension->GetOverrideText() ) );

        if( aDimension->GetSuppressZeroes() )
            m_out->Print( 0, " suppress_zeroes" );

        m_out->Print( 0, ")\n" );
    }

    m_out->Print( aNestLevel+1, "(style (thickness %s) (arrow_length %s) (text_position_mode %d)",
                  FormatInternalUnits( aDimension->GetLineThickness() ).c_str(),
                  FormatInternalUnits( aDimension->GetArrowLength() ).c_str(),
                  static_cast<int>( aDimension->GetTextPositionMode() ) );

    if( aligned )
    {
        m_out->Print( 0, " (extension_height %s)",
                     FormatInternalUnits( aligned->GetExtensionHeight() ).c_str() );
    }

    if( leader )
        m_out->Print( 0, " (text_frame %d)", static_cast<int>( leader->GetTextFrame() ) );

    m_out->Print( 0, " (extension_offset %s)",
                  FormatInternalUnits( aDimension->GetExtensionOffset() ).c_str() );

    if( aDimension->GetKeepTextAligned() )
        m_out->Print( 0, " keep_text_aligned" );

    m_out->Print( 0, ")\n" );

    m_out->Print( aNestLevel, ")\n" );
}


void PCB_IO::format( PCB_SHAPE* aShape, int aNestLevel ) const
{
    switch( aShape->GetShape() )
    {
    case S_SEGMENT:  // Line
        m_out->Print( aNestLevel, "(gr_line (start %s) (end %s)",
                      FormatInternalUnits( aShape->GetStart() ).c_str(),
                      FormatInternalUnits( aShape->GetEnd() ).c_str() );

        if( aShape->GetAngle() != 0.0 )
            m_out->Print( 0, " (angle %s)", FormatAngle( aShape->GetAngle() ).c_str() );

        break;

    case S_RECT:  // Rectangle
        m_out->Print( aNestLevel, "(gr_rect (start %s) (end %s)",
                      FormatInternalUnits( aShape->GetStart() ).c_str(),
                      FormatInternalUnits( aShape->GetEnd() ).c_str() );
        break;

    case S_CIRCLE:  // Circle
        m_out->Print( aNestLevel, "(gr_circle (center %s) (end %s)",
                      FormatInternalUnits( aShape->GetStart() ).c_str(),
                      FormatInternalUnits( aShape->GetEnd() ).c_str() );
        break;

    case S_ARC:     // Arc
        m_out->Print( aNestLevel, "(gr_arc (start %s) (end %s) (angle %s)",
                      FormatInternalUnits( aShape->GetStart() ).c_str(),
                      FormatInternalUnits( aShape->GetEnd() ).c_str(),
                      FormatAngle( aShape->GetAngle() ).c_str() );
        break;

    case S_POLYGON: // Polygon
        if( aShape->IsPolyShapeValid() )
        {
            SHAPE_POLY_SET& poly = aShape->GetPolyShape();
            SHAPE_LINE_CHAIN& outline = poly.Outline( 0 );
            int pointsCount = outline.PointCount();

            m_out->Print( aNestLevel, "(gr_poly (pts\n" );

            for( int ii = 0; ii < pointsCount;  ++ii )
            {
                int nestLevel = 0;

                if( ii && ( !( ii%4 ) || !ADVANCED_CFG::GetCfg().m_CompactSave ) )   // newline every 4 pts
                {
                    nestLevel = aNestLevel + 1;
                    m_out->Print( 0, "\n" );
                }

                m_out->Print( nestLevel, "%s(xy %s)",
                              nestLevel ? "" : " ", FormatInternalUnits( outline.CPoint( ii ) ).c_str() );
            }

            m_out->Print( 0, ")" );
        }
        else
        {
            wxFAIL_MSG( wxT( "Cannot format invalid polygon." ) );
            return;
        }

        break;

    case S_CURVE:   // Bezier curve
        m_out->Print( aNestLevel, "(gr_curve (pts (xy %s) (xy %s) (xy %s) (xy %s))",
                      FormatInternalUnits( aShape->GetStart() ).c_str(),
                      FormatInternalUnits( aShape->GetBezControl1() ).c_str(),
                      FormatInternalUnits( aShape->GetBezControl2() ).c_str(),
                      FormatInternalUnits( aShape->GetEnd() ).c_str() );
        break;

    default:
        wxFAIL_MSG( "PCB_IO::format cannot format unknown PCB_SHAPE shape:"
                    + PCB_SHAPE_TYPE_T_asString( aShape->GetShape()) );
        return;
    };

    formatLayer( aShape );

    m_out->Print( 0, " (width %s)", FormatInternalUnits( aShape->GetWidth() ).c_str() );

    m_out->Print( 0, " (tstamp %s)", TO_UTF8( aShape->m_Uuid.AsString() ) );

    m_out->Print( 0, ")\n" );
}


void PCB_IO::format( FP_SHAPE* aModuleDrawing, int aNestLevel ) const
{
    switch( aModuleDrawing->GetShape() )
    {
    case S_SEGMENT:  // Line
        m_out->Print( aNestLevel, "(fp_line (start %s) (end %s)",
                      FormatInternalUnits( aModuleDrawing->GetStart0() ).c_str(),
                      FormatInternalUnits( aModuleDrawing->GetEnd0() ).c_str() );
        break;

    case S_RECT:    // Rectangle
        m_out->Print( aNestLevel, "(fp_rect (start %s) (end %s)",
                      FormatInternalUnits( aModuleDrawing->GetStart0() ).c_str(),
                      FormatInternalUnits( aModuleDrawing->GetEnd0() ).c_str() );
        break;

    case S_CIRCLE:  // Circle
        m_out->Print( aNestLevel, "(fp_circle (center %s) (end %s)",
                      FormatInternalUnits( aModuleDrawing->GetStart0() ).c_str(),
                      FormatInternalUnits( aModuleDrawing->GetEnd0() ).c_str() );
        break;

    case S_ARC:     // Arc
        m_out->Print( aNestLevel, "(fp_arc (start %s) (end %s) (angle %s)",
                      FormatInternalUnits( aModuleDrawing->GetStart0() ).c_str(),
                      FormatInternalUnits( aModuleDrawing->GetEnd0() ).c_str(),
                      FormatAngle( aModuleDrawing->GetAngle() ).c_str() );
        break;

    case S_POLYGON: // Polygonal segment
        if( aModuleDrawing->IsPolyShapeValid() )
        {
            SHAPE_POLY_SET& poly = aModuleDrawing->GetPolyShape();
            SHAPE_LINE_CHAIN& outline = poly.Outline( 0 );
            int pointsCount = outline.PointCount();

            m_out->Print( aNestLevel, "(fp_poly (pts" );

            for( int ii = 0; ii < pointsCount;  ++ii )
            {
                int nestLevel = 0;

                if( ii && ( !( ii%4 ) || !ADVANCED_CFG::GetCfg().m_CompactSave ) )   // newline every 4 pts
                {
                    nestLevel = aNestLevel + 1;
                    m_out->Print( 0, "\n" );
                }

                m_out->Print( nestLevel, "%s(xy %s)",
                              nestLevel ? "" : " ", FormatInternalUnits( outline.CPoint( ii ) ).c_str() );
            }

            m_out->Print( 0, ")" );
        }
        else
        {
            wxFAIL_MSG( wxT( "Cannot format invalid polygon." ) );
            return;
        }
        break;

    case S_CURVE:   // Bezier curve
        m_out->Print( aNestLevel, "(fp_curve (pts (xy %s) (xy %s) (xy %s) (xy %s))",
                      FormatInternalUnits( aModuleDrawing->GetStart0() ).c_str(),
                      FormatInternalUnits( aModuleDrawing->GetBezier0_C1() ).c_str(),
                      FormatInternalUnits( aModuleDrawing->GetBezier0_C2() ).c_str(),
                      FormatInternalUnits( aModuleDrawing->GetEnd0() ).c_str() );
        break;

    default:
        wxFAIL_MSG( "PCB_IO::format cannot format unknown FP_SHAPE shape:"
                    + PCB_SHAPE_TYPE_T_asString( aModuleDrawing->GetShape()) );
        return;
    };

    formatLayer( aModuleDrawing );

    m_out->Print( 0, " (width %s)", FormatInternalUnits( aModuleDrawing->GetWidth() ).c_str() );

    m_out->Print( 0, " (tstamp %s)", TO_UTF8( aModuleDrawing->m_Uuid.AsString() ) );

    m_out->Print( 0, ")\n" );
}


void PCB_IO::format( PCB_TARGET* aTarget, int aNestLevel ) const
{
    m_out->Print( aNestLevel, "(target %s (at %s) (size %s)",
                  ( aTarget->GetShape() ) ? "x" : "plus",
                  FormatInternalUnits( aTarget->GetPosition() ).c_str(),
                  FormatInternalUnits( aTarget->GetSize() ).c_str() );

    if( aTarget->GetWidth() != 0 )
        m_out->Print( 0, " (width %s)", FormatInternalUnits( aTarget->GetWidth() ).c_str() );

    formatLayer( aTarget );

    m_out->Print( 0, " (tstamp %s)", TO_UTF8( aTarget->m_Uuid.AsString() ) );

    m_out->Print( 0, ")\n" );
}


void PCB_IO::format( MODULE* aModule, int aNestLevel ) const
{
    if( !( m_ctl & CTL_OMIT_INITIAL_COMMENTS ) )
    {
        const wxArrayString* initial_comments = aModule->GetInitialComments();

        if( initial_comments )
        {
            for( unsigned i=0;  i<initial_comments->GetCount();  ++i )
                m_out->Print( aNestLevel, "%s\n",  TO_UTF8( (*initial_comments)[i] ) );

            m_out->Print( 0, "\n" );    // improve readability?
        }
    }

    if( m_ctl & CTL_OMIT_LIBNAME )
        m_out->Print( aNestLevel, "(module %s",
                      m_out->Quotes( aModule->GetFPID().GetLibItemNameAndRev() ).c_str() );
    else
        m_out->Print( aNestLevel, "(module %s",
                      m_out->Quotes( aModule->GetFPID().Format() ).c_str() );

    if( aModule->IsLocked() )
        m_out->Print( 0, " locked" );

    if( aModule->IsPlaced() )
        m_out->Print( 0, " placed" );

    formatLayer( aModule );

    m_out->Print( 0, " (tedit %lX)", (unsigned long)aModule->GetLastEditTime() );

    if( !( m_ctl & CTL_OMIT_TSTAMPS ) )
        m_out->Print( 0, " (tstamp %s)", TO_UTF8( aModule->m_Uuid.AsString() ) );

    m_out->Print( 0, "\n" );

    if( !( m_ctl & CTL_OMIT_AT ) )
    {
        m_out->Print( aNestLevel+1, "(at %s", FormatInternalUnits( aModule->GetPosition() ).c_str() );

        if( aModule->GetOrientation() != 0.0 )
            m_out->Print( 0, " %s", FormatAngle( aModule->GetOrientation() ).c_str() );

        m_out->Print( 0, ")\n" );
    }

    if( !aModule->GetDescription().IsEmpty() )
        m_out->Print( aNestLevel+1, "(descr %s)\n",
                      m_out->Quotew( aModule->GetDescription() ).c_str() );

    if( !aModule->GetKeywords().IsEmpty() )
        m_out->Print( aNestLevel+1, "(tags %s)\n",
                      m_out->Quotew( aModule->GetKeywords() ).c_str() );

    const std::map<wxString, wxString>& props = aModule->GetProperties();

    for( const std::pair<const wxString, wxString>& prop : props )
    {
        m_out->Print( aNestLevel+1, "(property %s %s)\n",
                      m_out->Quotew( prop.first ).c_str(),
                      m_out->Quotew( prop.second ).c_str() );
    }

    if( !( m_ctl & CTL_OMIT_PATH ) && !aModule->GetPath().empty() )
        m_out->Print( aNestLevel+1, "(path %s)\n",
                      m_out->Quotew( aModule->GetPath().AsString() ).c_str() );

    if( aModule->GetPlacementCost90() != 0 )
        m_out->Print( aNestLevel+1, "(autoplace_cost90 %d)\n", aModule->GetPlacementCost90() );

    if( aModule->GetPlacementCost180() != 0 )
        m_out->Print( aNestLevel+1, "(autoplace_cost180 %d)\n", aModule->GetPlacementCost180() );

    if( aModule->GetLocalSolderMaskMargin() != 0 )
        m_out->Print( aNestLevel+1, "(solder_mask_margin %s)\n",
                      FormatInternalUnits( aModule->GetLocalSolderMaskMargin() ).c_str() );

    if( aModule->GetLocalSolderPasteMargin() != 0 )
        m_out->Print( aNestLevel+1, "(solder_paste_margin %s)\n",
                      FormatInternalUnits( aModule->GetLocalSolderPasteMargin() ).c_str() );

    if( aModule->GetLocalSolderPasteMarginRatio() != 0 )
        m_out->Print( aNestLevel+1, "(solder_paste_ratio %s)\n",
                      Double2Str( aModule->GetLocalSolderPasteMarginRatio() ).c_str() );

    if( aModule->GetLocalClearance() != 0 )
        m_out->Print( aNestLevel+1, "(clearance %s)\n",
                      FormatInternalUnits( aModule->GetLocalClearance() ).c_str() );

    if( aModule->GetZoneConnection() != ZONE_CONNECTION::INHERITED )
        m_out->Print( aNestLevel+1, "(zone_connect %d)\n",
                                    static_cast<int>( aModule->GetZoneConnection() ) );

    if( aModule->GetThermalWidth() != 0 )
        m_out->Print( aNestLevel+1, "(thermal_width %s)\n",
                      FormatInternalUnits( aModule->GetThermalWidth() ).c_str() );

    if( aModule->GetThermalGap() != 0 )
        m_out->Print( aNestLevel+1, "(thermal_gap %s)\n",
                      FormatInternalUnits( aModule->GetThermalGap() ).c_str() );

    // Attributes
    if( aModule->GetAttributes() )
    {
        m_out->Print( aNestLevel+1, "(attr" );

        if( aModule->GetAttributes() & MOD_SMD )
            m_out->Print( 0, " smd" );

        if( aModule->GetAttributes() & MOD_THROUGH_HOLE )
            m_out->Print( 0, " through_hole" );

        if( aModule->GetAttributes() & MOD_BOARD_ONLY )
            m_out->Print( 0, " board_only" );

        if( aModule->GetAttributes() & MOD_EXCLUDE_FROM_POS_FILES )
            m_out->Print( 0, " exclude_from_pos_files" );

        if( aModule->GetAttributes() & MOD_EXCLUDE_FROM_BOM )
            m_out->Print( 0, " exclude_from_bom" );

        m_out->Print( 0, ")\n" );
    }

    Format( (BOARD_ITEM*) &aModule->Reference(), aNestLevel+1 );
    Format( (BOARD_ITEM*) &aModule->Value(), aNestLevel+1 );

    std::set<D_PAD*, MODULE::cmp_pads> sorted_pads( aModule->Pads().begin(),
            aModule->Pads().end() );
    std::set<BOARD_ITEM*, MODULE::cmp_drawings> sorted_drawings( aModule->GraphicalItems().begin(),
            aModule->GraphicalItems().end() );
    std::set<BOARD_ITEM*, BOARD_ITEM::ptr_cmp> sorted_zones( aModule->Zones().begin(),
            aModule->Zones().end() );
    std::set<BOARD_ITEM*, PCB_GROUP::ptr_cmp> sorted_groups( aModule->Groups().begin(),
            aModule->Groups().end() );

    // Save drawing elements.

    for( BOARD_ITEM* gr : sorted_drawings )
        Format( gr, aNestLevel+1 );

    // Save pads.
    for( D_PAD* pad : sorted_pads )
        Format( pad, aNestLevel+1 );

    // Save zones.
    for( BOARD_ITEM* zone : sorted_zones )
        Format( zone, aNestLevel + 1 );

    // Save groups.
    for( BOARD_ITEM* group : sorted_groups )
        Format( group, aNestLevel + 1 );

    // Save 3D info.
    auto bs3D = aModule->Models().begin();
    auto es3D = aModule->Models().end();

    while( bs3D != es3D )
    {
        if( !bs3D->m_Filename.IsEmpty() )
        {
            m_out->Print( aNestLevel+1, "(model %s%s\n",
                          m_out->Quotew( bs3D->m_Filename ).c_str(),
                          bs3D->m_Show ? "" : " hide" );

            if( bs3D->m_Opacity != 1.0 )
                m_out->Print( aNestLevel+2, "(opacity %0.4f)", bs3D->m_Opacity );

            m_out->Print( aNestLevel+2, "(offset (xyz %s %s %s))\n",
                          Double2Str( bs3D->m_Offset.x ).c_str(),
                          Double2Str( bs3D->m_Offset.y ).c_str(),
                          Double2Str( bs3D->m_Offset.z ).c_str() );

            m_out->Print( aNestLevel+2, "(scale (xyz %s %s %s))\n",
                          Double2Str( bs3D->m_Scale.x ).c_str(),
                          Double2Str( bs3D->m_Scale.y ).c_str(),
                          Double2Str( bs3D->m_Scale.z ).c_str() );

            m_out->Print( aNestLevel+2, "(rotate (xyz %s %s %s))\n",
                          Double2Str( bs3D->m_Rotation.x ).c_str(),
                          Double2Str( bs3D->m_Rotation.y ).c_str(),
                          Double2Str( bs3D->m_Rotation.z ).c_str() );

            m_out->Print( aNestLevel+1, ")\n" );
        }
        ++bs3D;
    }

    m_out->Print( aNestLevel, ")\n" );
}


void PCB_IO::formatLayers( LSET aLayerMask, int aNestLevel ) const
{
    std::string  output;

    if( aNestLevel == 0 )
        output += ' ';

    output += "(layers";

    static const LSET cu_all( LSET::AllCuMask() );
    static const LSET fr_bk( 2, B_Cu,       F_Cu );
    static const LSET adhes( 2, B_Adhes,    F_Adhes );
    static const LSET paste( 2, B_Paste,    F_Paste );
    static const LSET silks( 2, B_SilkS,    F_SilkS );
    static const LSET mask(  2, B_Mask,     F_Mask );
    static const LSET crt_yd(2, B_CrtYd,    F_CrtYd );
    static const LSET fab(   2, B_Fab,      F_Fab );

    LSET cu_mask = cu_all;

    // output copper layers first, then non copper

    if( ( aLayerMask & cu_mask ) == cu_mask )
    {
        output += " *.Cu";
        aLayerMask &= ~cu_all;          // clear bits, so they are not output again below
    }
    else if( ( aLayerMask & cu_mask ) == fr_bk )
    {
        output += " F&B.Cu";
        aLayerMask &= ~fr_bk;
    }

    if( ( aLayerMask & adhes ) == adhes )
    {
        output += " *.Adhes";
        aLayerMask &= ~adhes;
    }

    if( ( aLayerMask & paste ) == paste )
    {
        output += " *.Paste";
        aLayerMask &= ~paste;
    }

    if( ( aLayerMask & silks ) == silks )
    {
        output += " *.SilkS";
        aLayerMask &= ~silks;
    }

    if( ( aLayerMask & mask ) == mask )
    {
        output += " *.Mask";
        aLayerMask &= ~mask;
    }

    if( ( aLayerMask & crt_yd ) == crt_yd )
    {
        output += " *.CrtYd";
        aLayerMask &= ~crt_yd;
    }

    if( ( aLayerMask & fab ) == fab )
    {
        output += " *.Fab";
        aLayerMask &= ~fab;
    }

    // output any individual layers not handled in wildcard combos above

    wxString layerName;

    for( LAYER_NUM layer = 0; layer < PCB_LAYER_ID_COUNT; ++layer )
    {
        if( aLayerMask[layer] )
        {
            layerName = LSET::Name( PCB_LAYER_ID( layer ) );
            output += ' ';
            output += m_out->Quotew( layerName );
        }
    }

    m_out->Print( aNestLevel, "%s)", output.c_str() );
}


void PCB_IO::format( D_PAD* aPad, int aNestLevel ) const
{
    const char* shape;

    switch( aPad->GetShape() )
    {
    case PAD_SHAPE_CIRCLE:          shape = "circle";       break;
    case PAD_SHAPE_RECT:            shape = "rect";         break;
    case PAD_SHAPE_OVAL:            shape = "oval";         break;
    case PAD_SHAPE_TRAPEZOID:       shape = "trapezoid";    break;
    case PAD_SHAPE_CHAMFERED_RECT:
    case PAD_SHAPE_ROUNDRECT:       shape = "roundrect";    break;
    case PAD_SHAPE_CUSTOM:          shape = "custom";       break;

    default:
        THROW_IO_ERROR( wxString::Format( _( "unknown pad type: %d"), aPad->GetShape() ) );
    }

    const char* type;

    switch( aPad->GetAttribute() )
    {
    case PAD_ATTRIB_PTH:    type = "thru_hole";      break;
    case PAD_ATTRIB_SMD:    type = "smd";            break;
    case PAD_ATTRIB_CONN:   type = "connect";        break;
    case PAD_ATTRIB_NPTH:   type = "np_thru_hole";   break;

    default:
        THROW_IO_ERROR( wxString::Format( "unknown pad attribute: %d", aPad->GetAttribute() ) );
    }

    const char* property = nullptr;

    switch( aPad->GetProperty() )
    {
    case PAD_PROP_NONE:             break;  // could be also "none"
    case PAD_PROP_BGA:              property = "pad_prop_bga"; break;
    case PAD_PROP_FIDUCIAL_GLBL:    property = "pad_prop_fiducial_glob"; break;
    case PAD_PROP_FIDUCIAL_LOCAL:   property = "pad_prop_fiducial_loc"; break;
    case PAD_PROP_TESTPOINT:        property = "pad_prop_testpoint"; break;
    case PAD_PROP_HEATSINK:         property = "pad_prop_heatsink"; break;
    case PAD_PROP_CASTELLATED:      property = "pad_prop_castellated"; break;

    default:
        THROW_IO_ERROR( wxString::Format( "unknown pad property: %d", aPad->GetProperty() ) );
    }

    m_out->Print( aNestLevel, "(pad %s %s %s",
                  m_out->Quotew( aPad->GetName() ).c_str(),
                  type, shape );
    m_out->Print( 0, " (at %s", FormatInternalUnits( aPad->GetPos0() ).c_str() );

    if( aPad->GetOrientation() != 0.0 )
        m_out->Print( 0, " %s", FormatAngle( aPad->GetOrientation() ).c_str() );

    m_out->Print( 0, ")" );
    m_out->Print( 0, " (size %s)", FormatInternalUnits( aPad->GetSize() ).c_str() );

    if( (aPad->GetDelta().GetWidth()) != 0 || (aPad->GetDelta().GetHeight() != 0 ) )
        m_out->Print( 0, " (rect_delta %s )", FormatInternalUnits( aPad->GetDelta() ).c_str() );

    wxSize sz = aPad->GetDrillSize();
    wxPoint shapeoffset = aPad->GetOffset();

    if( (sz.GetWidth() > 0) || (sz.GetHeight() > 0) ||
        (shapeoffset.x != 0) || (shapeoffset.y != 0) )
    {
        m_out->Print( 0, " (drill" );

        if( aPad->GetDrillShape() == PAD_DRILL_SHAPE_OBLONG )
            m_out->Print( 0, " oval" );

        if( sz.GetWidth() > 0 )
            m_out->Print( 0,  " %s", FormatInternalUnits( sz.GetWidth() ).c_str() );

        if( sz.GetHeight() > 0  && sz.GetWidth() != sz.GetHeight() )
            m_out->Print( 0,  " %s", FormatInternalUnits( sz.GetHeight() ).c_str() );

        if( (shapeoffset.x != 0) || (shapeoffset.y != 0) )
            m_out->Print( 0, " (offset %s)", FormatInternalUnits( aPad->GetOffset() ).c_str() );

        m_out->Print( 0, ")" );
    }

    // Add pad property, if exists.
    if( property )
        m_out->Print( 0, " (property %s)", property );

    formatLayers( aPad->GetLayerSet() );

    if( aPad->GetAttribute() == PAD_ATTRIB_PTH )
    {
        if( aPad->GetRemoveUnconnected() )
        {
            m_out->Print( 0, " (remove_unused_layers)" );

            if( aPad->GetKeepTopBottom() )
                m_out->Print( 0, " (keep_end_layers)" );
        }
    }

    // Output the radius ratio for rounded and chamfered rect pads
    if( aPad->GetShape() == PAD_SHAPE_ROUNDRECT || aPad->GetShape() == PAD_SHAPE_CHAMFERED_RECT)
    {
        m_out->Print( 0,  " (roundrect_rratio %s)",
                      Double2Str( aPad->GetRoundRectRadiusRatio() ).c_str() );
    }

    // Output the chamfer corners for chamfered rect pads
    if( aPad->GetShape() == PAD_SHAPE_CHAMFERED_RECT)
    {
        m_out->Print( 0, "\n" );

        m_out->Print( aNestLevel+1,  "(chamfer_ratio %s)",
                      Double2Str( aPad->GetChamferRectRatio() ).c_str() );

        m_out->Print( 0, " (chamfer" );

        if( ( aPad->GetChamferPositions() & RECT_CHAMFER_TOP_LEFT ) )
            m_out->Print( 0,  " top_left" );

        if( ( aPad->GetChamferPositions() & RECT_CHAMFER_TOP_RIGHT ) )
            m_out->Print( 0,  " top_right" );

        if( ( aPad->GetChamferPositions() & RECT_CHAMFER_BOTTOM_LEFT ) )
            m_out->Print( 0,  " bottom_left" );

        if( ( aPad->GetChamferPositions() & RECT_CHAMFER_BOTTOM_RIGHT ) )
            m_out->Print( 0,  " bottom_right" );

        m_out->Print( 0,  ")" );
    }

    std::string output;

    // Unconnected pad is default net so don't save it.
    if( !( m_ctl & CTL_OMIT_NETS ) && aPad->GetNetCode() != NETINFO_LIST::UNCONNECTED )
        StrPrintf( &output, " (net %d %s)", m_mapping->Translate( aPad->GetNetCode() ),
                   m_out->Quotew( aPad->GetNetname() ).c_str() );

    // Add pinfunction, if exists.
    // Pin function is closely related to nets, so if CTL_OMIT_NETS is set,
    // omit also pin function (for instance when saved from library editor)
    if( !(m_ctl & CTL_OMIT_NETS) && !aPad->GetPinFunction().IsEmpty() )
        StrPrintf( &output, " (pinfunction %s)",
                   m_out->Quotew( aPad->GetPinFunction() ).c_str() );

    if( aPad->GetPadToDieLength() != 0 )
        StrPrintf( &output, " (die_length %s)",
                   FormatInternalUnits( aPad->GetPadToDieLength() ).c_str() );

    if( aPad->GetLocalSolderMaskMargin() != 0 )
        StrPrintf( &output, " (solder_mask_margin %s)",
                   FormatInternalUnits( aPad->GetLocalSolderMaskMargin() ).c_str() );

    if( aPad->GetLocalSolderPasteMargin() != 0 )
        StrPrintf( &output, " (solder_paste_margin %s)",
                   FormatInternalUnits( aPad->GetLocalSolderPasteMargin() ).c_str() );

    if( aPad->GetLocalSolderPasteMarginRatio() != 0 )
        StrPrintf( &output, " (solder_paste_margin_ratio %s)",
                   Double2Str( aPad->GetLocalSolderPasteMarginRatio() ).c_str() );

    if( aPad->GetLocalClearance() != 0 )
        StrPrintf( &output, " (clearance %s)",
                   FormatInternalUnits( aPad->GetLocalClearance() ).c_str() );

    if( aPad->GetEffectiveZoneConnection() != ZONE_CONNECTION::INHERITED )
        StrPrintf( &output, " (zone_connect %d)",
                   static_cast<int>( aPad->GetEffectiveZoneConnection() ) );

    if( aPad->GetThermalSpokeWidth() != 0 )
        StrPrintf( &output, " (thermal_width %s)",
                   FormatInternalUnits( aPad->GetThermalSpokeWidth() ).c_str() );

    if( aPad->GetThermalGap() != 0 )
        StrPrintf( &output, " (thermal_gap %s)",
                   FormatInternalUnits( aPad->GetThermalGap() ).c_str() );

    if( output.size() )
    {
        m_out->Print( 0, "\n" );
        m_out->Print( aNestLevel+1, "%s", output.c_str()+1 );   // +1 skips 1st space on 1st element
    }

    if( aPad->GetShape() == PAD_SHAPE_CUSTOM )
    {
        m_out->Print( 0, "\n");
        m_out->Print( aNestLevel+1, "(options" );

        if( aPad->GetCustomShapeInZoneOpt() == CUST_PAD_SHAPE_IN_ZONE_CONVEXHULL )
            m_out->Print( 0, " (clearance convexhull)" );
        #if 1   // Set to 1 to output the default option
        else
            m_out->Print( 0, " (clearance outline)" );
        #endif

        // Output the anchor pad shape (circle/rect)
        if( aPad->GetAnchorPadShape() == PAD_SHAPE_RECT )
            shape = "rect";
        else
            shape = "circle";

        m_out->Print( 0, " (anchor %s)", shape );

        m_out->Print( 0, ")");  // end of (options ...

        // Output graphic primitive of the pad shape
        m_out->Print( 0, "\n");
        m_out->Print( aNestLevel+1, "(primitives" );

        int nested_level = aNestLevel+2;

        // Output all basic shapes
        for( const std::shared_ptr<PCB_SHAPE>& primitive : aPad->GetPrimitives() )
        {
            m_out->Print( 0, "\n");

            switch( primitive->GetShape() )
            {
            case S_SEGMENT:         // usual segment : line with rounded ends
                m_out->Print( nested_level, "(gr_line (start %s) (end %s) (width %s))",
                              FormatInternalUnits( primitive->GetStart() ).c_str(),
                              FormatInternalUnits( primitive->GetEnd() ).c_str(),
                              FormatInternalUnits( primitive->GetWidth() ).c_str() );
                break;

            case S_RECT:
                m_out->Print( nested_level, "(gr_rect (start %s) (end %s) (width %s))",
                              FormatInternalUnits( primitive->GetStart() ).c_str(),
                              FormatInternalUnits( primitive->GetEnd() ).c_str(),
                              FormatInternalUnits( primitive->GetWidth() ).c_str() );
                break;

            case S_ARC:             // Arc with rounded ends
                m_out->Print( nested_level, "(gr_arc (start %s) (end %s) (angle %s) (width %s))",
                              FormatInternalUnits( primitive->GetStart() ).c_str(),
                              FormatInternalUnits( primitive->GetEnd() ).c_str(),
                              FormatAngle( primitive->GetAngle() ).c_str(),
                              FormatInternalUnits( primitive->GetWidth() ).c_str() );
                break;

            case S_CIRCLE:          //  ring or circle (circle if width == 0
                m_out->Print( nested_level, "(gr_circle (center %s) (end %s) (width %s))",
                              FormatInternalUnits( primitive->GetStart() ).c_str(),
                              FormatInternalUnits( primitive->GetEnd() ).c_str(),
                              FormatInternalUnits( primitive->GetWidth() ).c_str() );
                break;

            case S_CURVE:          //  Bezier Curve
                m_out->Print( aNestLevel, "(gr_curve (pts (xy %s) (xy %s) (xy %s) (xy %s)) (width %s))",
                              FormatInternalUnits( primitive->GetStart() ).c_str(),
                              FormatInternalUnits( primitive->GetBezControl1() ).c_str(),
                              FormatInternalUnits( primitive->GetBezControl2() ).c_str(),
                              FormatInternalUnits( primitive->GetEnd() ).c_str(),
                              FormatInternalUnits( primitive->GetWidth() ).c_str() );
                break;

            case S_POLYGON:         // polygon
                if( primitive->GetPolyShape().COutline( 0 ).CPoints().size() < 2 )
                    break;      // Malformed polygon.

                {
                m_out->Print( nested_level, "(gr_poly (pts\n");

                // Write the polygon corners coordinates:
                int newLine = 0;

                for( const VECTOR2I &pt : primitive->GetPolyShape().COutline( 0 ).CPoints() )
                {
                    if( newLine == 0 )
                        m_out->Print( nested_level+1, "(xy %s)",
                                      FormatInternalUnits( (wxPoint) pt ).c_str() );
                    else
                        m_out->Print( 0, " (xy %s)",
                                      FormatInternalUnits( (wxPoint) pt ).c_str() );

                    if( ++newLine > 4 || !ADVANCED_CFG::GetCfg().m_CompactSave )
                    {
                        newLine = 0;
                        m_out->Print( 0, "\n" );
                    }
                }

                m_out->Print( 0, ") (width %s))", FormatInternalUnits( primitive->GetWidth() ).c_str() );
                }
                break;

            default:
                break;
            }
        }

        m_out->Print( 0, "\n");
        m_out->Print( aNestLevel+1, ")" );   // end of (basic_shapes
    }

    m_out->Print( 0, " (tstamp %s)", TO_UTF8( aPad->m_Uuid.AsString() ) );

    m_out->Print( 0, ")\n" );
}


void PCB_IO::format( PCB_TEXT* aText, int aNestLevel ) const
{
    m_out->Print( aNestLevel, "(gr_text %s (at %s",
                  m_out->Quotew( aText->GetText() ).c_str(),
                  FormatInternalUnits( aText->GetTextPos() ).c_str() );

    if( aText->GetTextAngle() != 0.0 )
        m_out->Print( 0, " %s", FormatAngle( aText->GetTextAngle() ).c_str() );

    m_out->Print( 0, ")" );

    formatLayer( aText );

    m_out->Print( 0, " (tstamp %s)", TO_UTF8( aText->m_Uuid.AsString() ) );

    m_out->Print( 0, "\n" );

    aText->EDA_TEXT::Format( m_out, aNestLevel, m_ctl );

    m_out->Print( aNestLevel, ")\n" );
}


void PCB_IO::format( PCB_GROUP* aGroup, int aNestLevel ) const
{
    // Don't write empty groups
    if( aGroup->GetItems().empty() )
        return;

    m_out->Print( aNestLevel, "(group %s (id %s)\n",
                              m_out->Quotew( aGroup->GetName() ).c_str(),
                              TO_UTF8( aGroup->m_Uuid.AsString() ) );

    m_out->Print( aNestLevel + 1, "(members\n" );

    wxArrayString memberIds;

    for( BOARD_ITEM* member : aGroup->GetItems() )
        memberIds.Add( member->m_Uuid.AsString() );

    memberIds.Sort();

    for( const wxString& memberId : memberIds )
        m_out->Print( aNestLevel + 2, "%s\n", TO_UTF8( memberId ) );

    m_out->Print( 0, " )\n" );
    m_out->Print( aNestLevel, ")\n" );
}


void PCB_IO::format( FP_TEXT* aText, int aNestLevel ) const
{
    std::string type;

    switch( aText->GetType() )
    {
    case FP_TEXT::TEXT_is_REFERENCE: type = "reference";   break;
    case FP_TEXT::TEXT_is_VALUE: type = "value";       break;
    case FP_TEXT::TEXT_is_DIVERS: type = "user";
    }

    m_out->Print( aNestLevel, "(fp_text %s %s (at %s",
                  type.c_str(),
                  m_out->Quotew( aText->GetText() ).c_str(),
                  FormatInternalUnits( aText->GetPos0() ).c_str() );

    // Due to Pcbnew history, fp_text angle is saved as an absolute on screen angle,
    // but internally the angle is held relative to its parent footprint.  parent
    // may be NULL when saving a footprint outside a BOARD.
    double   orient = aText->GetTextAngle();
    MODULE*  parent = (MODULE*) aText->GetParent();

    if( parent )
    {
        // GetTextAngle() is always in -360..+360 range because of
        // FP_TEXT::SetTextAngle(), but summing that angle with an
        // additional board angle could kick sum up >= 360 or <= -360, so to have
        // consistent results, normalize again for the BOARD save.  A footprint
        // save does not use this code path since parent is NULL.
#if 0
        // This one could be considered reasonable if you like positive angles
        // in your board text.
        orient = NormalizeAnglePos( orient + parent->GetOrientation() );
#else
        // Choose compatibility for now, even though this is only a 720 degree clamp
        // with two possible values for every angle.
        orient = NormalizeAngle360Min( orient + parent->GetOrientation() );
#endif
    }

    if( orient != 0.0 )
        m_out->Print( 0, " %s", FormatAngle( orient ).c_str() );

    if( !aText->IsKeepUpright() )
        m_out->Print( 0, " unlocked" );

    m_out->Print( 0, ")" );
    formatLayer( aText );

    if( !aText->IsVisible() )
        m_out->Print( 0, " hide" );

    m_out->Print( 0, "\n" );

    aText->EDA_TEXT::Format( m_out, aNestLevel, m_ctl | CTL_OMIT_HIDE );

    m_out->Print( aNestLevel + 1, "(tstamp %s)\n", TO_UTF8( aText->m_Uuid.AsString() ) );

    m_out->Print( aNestLevel, ")\n" );
}


void PCB_IO::format( TRACK* aTrack, int aNestLevel ) const
{
    if( aTrack->Type() == PCB_VIA_T )
    {
        PCB_LAYER_ID  layer1, layer2;

        const VIA*  via = static_cast<const VIA*>( aTrack );
        BOARD*      board = (BOARD*) via->GetParent();

        wxCHECK_RET( board != 0, wxT( "Via " ) + via->GetSelectMenuText( EDA_UNITS::MILLIMETRES )
                                         + wxT( " has no parent." ) );

        m_out->Print( aNestLevel, "(via" );

        via->LayerPair( &layer1, &layer2 );

        switch( via->GetViaType() )
        {
        case VIATYPE::THROUGH: //  Default shape not saved.
            break;

        case VIATYPE::BLIND_BURIED:
            m_out->Print( 0, " blind" );
            break;

        case VIATYPE::MICROVIA:
            m_out->Print( 0, " micro" );
            break;

        default:
            THROW_IO_ERROR( wxString::Format( _( "unknown via type %d"  ), via->GetViaType() ) );
        }

        m_out->Print( 0, " (at %s) (size %s)",
                      FormatInternalUnits( aTrack->GetStart() ).c_str(),
                      FormatInternalUnits( aTrack->GetWidth() ).c_str() );

        if( via->GetDrill() != UNDEFINED_DRILL_DIAMETER )
            m_out->Print( 0, " (drill %s)", FormatInternalUnits( via->GetDrill() ).c_str() );

        m_out->Print( 0, " (layers %s %s)",
                      m_out->Quotew( LSET::Name( layer1 ) ).c_str(),
                      m_out->Quotew( LSET::Name( layer2 ) ).c_str() );

        if( via->GetRemoveUnconnected() )
        {
            m_out->Print( 0, " (remove_unused_layers)" );

            if( via->GetKeepTopBottom() )
                m_out->Print( 0, " (keep_end_layers)" );
        }
    }
    else if( aTrack->Type() == PCB_ARC_T )
    {
        const ARC* arc = static_cast<const ARC*>( aTrack );

        m_out->Print( aNestLevel, "(arc (start %s) (mid %s) (end %s) (width %s)",
                FormatInternalUnits( arc->GetStart() ).c_str(),
                FormatInternalUnits( arc->GetMid() ).c_str(),
                FormatInternalUnits( arc->GetEnd() ).c_str(),
                FormatInternalUnits( arc->GetWidth() ).c_str() );

        m_out->Print( 0, " (layer %s)", m_out->Quotew( LSET::Name( arc->GetLayer() ) ).c_str() );
    }
    else
    {
        m_out->Print( aNestLevel, "(segment (start %s) (end %s) (width %s)",
                      FormatInternalUnits( aTrack->GetStart() ).c_str(),
                      FormatInternalUnits( aTrack->GetEnd() ).c_str(),
                      FormatInternalUnits( aTrack->GetWidth() ).c_str() );

        m_out->Print( 0, " (layer %s)", m_out->Quotew( LSET::Name( aTrack->GetLayer() ) ).c_str() );
    }

    if( aTrack->IsLocked() )
        m_out->Print( 0, " (locked)" );

    m_out->Print( 0, " (net %d)", m_mapping->Translate( aTrack->GetNetCode() ) );

    m_out->Print( 0, " (tstamp %s)", TO_UTF8( aTrack->m_Uuid.AsString() ) );

    m_out->Print( 0, ")\n" );
}


void PCB_IO::format( ZONE_CONTAINER* aZone, int aNestLevel ) const
{
    // Save the NET info; For keepout zones, net code and net name are irrelevant
    // so be sure a dummy value is stored, just for ZONE_CONTAINER compatibility
    // (perhaps netcode and netname should be not stored)
    m_out->Print( aNestLevel, "(zone (net %d) (net_name %s)",
                  aZone->GetIsRuleArea() ? 0 : m_mapping->Translate( aZone->GetNetCode() ),
                  m_out->Quotew( aZone->GetIsRuleArea() ? wxT("") : aZone->GetNetname() ).c_str() );

    // If a zone exists on multiple layers, format accordingly
    if( aZone->GetLayerSet().count() > 1 )
    {
        formatLayers( aZone->GetLayerSet() );
    }
    else
    {
        formatLayer( aZone );
    }

    m_out->Print( 0, " (tstamp %s)", TO_UTF8( aZone->m_Uuid.AsString() ) );

    if( !aZone->GetZoneName().empty() )
        m_out->Print( 0, " (name %s)", m_out->Quotew( aZone->GetZoneName() ).c_str() );

    // Save the outline aux info
    std::string hatch;

    switch( aZone->GetHatchStyle() )
    {
    default:
    case ZONE_BORDER_DISPLAY_STYLE::NO_HATCH:      hatch = "none"; break;
    case ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_EDGE: hatch = "edge"; break;
    case ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_FULL: hatch = "full"; break;
    }

    m_out->Print( 0, " (hatch %s %s)\n", hatch.c_str(),
                  FormatInternalUnits( aZone->GetBorderHatchPitch() ).c_str() );

    if( aZone->GetPriority() > 0 )
        m_out->Print( aNestLevel+1, "(priority %d)\n", aZone->GetPriority() );

    m_out->Print( aNestLevel+1, "(connect_pads" );

    switch( aZone->GetPadConnection() )
    {
    default:
    case ZONE_CONNECTION::THERMAL: // Default option not saved or loaded.
        break;

    case ZONE_CONNECTION::THT_THERMAL:
        m_out->Print( 0, " thru_hole_only" );
        break;

    case ZONE_CONNECTION::FULL:
        m_out->Print( 0, " yes" );
        break;

    case ZONE_CONNECTION::NONE:
        m_out->Print( 0, " no" );
        break;
    }

    m_out->Print( 0, " (clearance %s))\n",
                  FormatInternalUnits( aZone->GetLocalClearance() ).c_str() );

    m_out->Print( aNestLevel+1, "(min_thickness %s)",
                  FormatInternalUnits( aZone->GetMinThickness() ).c_str() );

    // write it only if V 6.O version option is used (i.e. do not write if the "legacy"
    // algorithm is used)
    if( !aZone->GetFilledPolysUseThickness() )
        m_out->Print( 0, " (filled_areas_thickness no)" );

    m_out->Print( 0, "\n" );

    if( aZone->GetIsRuleArea() )
    {
        m_out->Print( aNestLevel+1, "(keepout (tracks %s) (vias %s) (pads %s ) (copperpour %s) (footprints %s))\n",
                      aZone->GetDoNotAllowTracks() ? "not_allowed" : "allowed",
                      aZone->GetDoNotAllowVias() ? "not_allowed" : "allowed",
                      aZone->GetDoNotAllowPads() ? "not_allowed" : "allowed",
                      aZone->GetDoNotAllowCopperPour() ? "not_allowed" : "allowed",
                      aZone->GetDoNotAllowFootprints() ? "not_allowed" : "allowed" );
    }

    m_out->Print( aNestLevel+1, "(fill" );

    // Default is not filled.
    if( aZone->IsFilled() )
        m_out->Print( 0, " yes" );

    // Default is polygon filled.
    if( aZone->GetFillMode() == ZONE_FILL_MODE::HATCH_PATTERN )
        m_out->Print( 0, " (mode hatch)" );

    m_out->Print( 0, " (thermal_gap %s) (thermal_bridge_width %s)",
                  FormatInternalUnits( aZone->GetThermalReliefGap() ).c_str(),
                  FormatInternalUnits( aZone->GetThermalReliefSpokeWidth() ).c_str() );

    if( aZone->GetCornerSmoothingType() != ZONE_SETTINGS::SMOOTHING_NONE )
    {
        m_out->Print( 0, " (smoothing" );

        switch( aZone->GetCornerSmoothingType() )
        {
        case ZONE_SETTINGS::SMOOTHING_CHAMFER:
            m_out->Print( 0, " chamfer" );
            break;

        case ZONE_SETTINGS::SMOOTHING_FILLET:
            m_out->Print( 0,  " fillet" );
            break;

        default:
            THROW_IO_ERROR( wxString::Format( _( "unknown zone corner smoothing type %d"  ),
                                              aZone->GetCornerSmoothingType() ) );
        }
        m_out->Print( 0, ")" );

        if( aZone->GetCornerRadius() != 0 )
            m_out->Print( 0, " (radius %s)",
                          FormatInternalUnits( aZone->GetCornerRadius() ).c_str() );
    }

    if( aZone->GetIslandRemovalMode() != ISLAND_REMOVAL_MODE::ALWAYS )
    {
        m_out->Print( 0, " (island_removal_mode %d) (island_area_min %s)",
                      static_cast<int>( aZone->GetIslandRemovalMode() ),
                      FormatInternalUnits( aZone->GetMinIslandArea() / IU_PER_MM ).c_str() );
    }

    if( aZone->GetFillMode() == ZONE_FILL_MODE::HATCH_PATTERN )
    {
        m_out->Print( 0, "\n" );
        m_out->Print( aNestLevel+2, "(hatch_thickness %s) (hatch_gap %s) (hatch_orientation %s)",
                      FormatInternalUnits( aZone->GetHatchThickness() ).c_str(),
                      FormatInternalUnits( aZone->GetHatchGap() ).c_str(),
                      Double2Str( aZone->GetHatchOrientation() ).c_str() );

        if( aZone->GetHatchSmoothingLevel() > 0 )
        {
            m_out->Print( 0, "\n" );
            m_out->Print( aNestLevel+2, "(hatch_smoothing_level %d) (hatch_smoothing_value %s)",
                          aZone->GetHatchSmoothingLevel(),
                          Double2Str( aZone->GetHatchSmoothingValue() ).c_str() );
        }

        m_out->Print( 0, "\n" );
        m_out->Print( aNestLevel+2, "(hatch_border_algorithm %s) (hatch_min_hole_area %s)",
                      aZone->GetHatchBorderAlgorithm() ? "hatch_thickness" : "min_thickness",
                      Double2Str( aZone->GetHatchHoleMinArea() ).c_str() );
    }

    m_out->Print( 0, ")\n" );

    int newLine = 0;

    if( aZone->GetNumCorners() )
    {
        bool new_polygon = true;
        bool is_closed = false;

        for( auto iterator = aZone->IterateWithHoles(); iterator; iterator++ )
        {
            if( new_polygon )
            {
                newLine = 0;
                m_out->Print( aNestLevel+1, "(polygon\n" );
                m_out->Print( aNestLevel+2, "(pts\n" );
                new_polygon = false;
                is_closed = false;
            }

            if( newLine == 0 )
                m_out->Print( aNestLevel+3, "(xy %s %s)",
                              FormatInternalUnits( iterator->x ).c_str(),
                              FormatInternalUnits( iterator->y ).c_str() );
            else
                m_out->Print( 0, " (xy %s %s)",
                              FormatInternalUnits( iterator->x ).c_str(),
                              FormatInternalUnits( iterator->y ).c_str() );

            if( newLine < 4 && ADVANCED_CFG::GetCfg().m_CompactSave )
            {
                newLine += 1;
            }
            else
            {
                newLine = 0;
                m_out->Print( 0, "\n" );
            }

            if( iterator.IsEndContour() )
            {
                is_closed = true;

                if( newLine != 0 )
                    m_out->Print( 0, "\n" );

                m_out->Print( aNestLevel+2, ")\n" );
                m_out->Print( aNestLevel+1, ")\n" );
                new_polygon = true;
            }
        }

        if( !is_closed )    // Should not happen, but...
        {
            if( newLine != 0 )
                m_out->Print( 0, "\n" );

            m_out->Print( aNestLevel+2, ")\n" );
            m_out->Print( aNestLevel+1, ")\n" );
        }
    }

    // Save the PolysList (filled areas)
    for( PCB_LAYER_ID layer : aZone->GetLayerSet().Seq() )
    {
        const SHAPE_POLY_SET& fv = aZone->GetFilledPolysList( layer );
        newLine                  = 0;

        if( !fv.IsEmpty() )
        {
            int  poly_index  = 0;
            bool new_polygon = true;
            bool is_closed   = false;

            for( auto it = fv.CIterate(); it; ++it )
            {
                if( new_polygon )
                {
                    newLine = 0;
                    m_out->Print( aNestLevel + 1, "(filled_polygon\n" );
                    m_out->Print( aNestLevel + 2, "(layer %s)\n",
                                  m_out->Quotew( LSET::Name( layer ) ).c_str() );

                    if( aZone->IsIsland( layer, poly_index ) )
                        m_out->Print( aNestLevel + 2, "(island)\n" );

                    m_out->Print( aNestLevel + 2, "(pts\n" );
                    new_polygon = false;
                    is_closed   = false;
                    poly_index++;
                }

                if( newLine == 0 )
                    m_out->Print( aNestLevel + 3, "(xy %s %s)",
                            FormatInternalUnits( it->x ).c_str(),
                            FormatInternalUnits( it->y ).c_str() );
                else
                    m_out->Print( 0, " (xy %s %s)", FormatInternalUnits( it->x ).c_str(),
                            FormatInternalUnits( it->y ).c_str() );

                if( newLine < 4 && ADVANCED_CFG::GetCfg().m_CompactSave )
                {
                    newLine += 1;
                }
                else
                {
                    newLine = 0;
                    m_out->Print( 0, "\n" );
                }

                if( it.IsEndContour() )
                {
                    is_closed = true;

                    if( newLine != 0 )
                        m_out->Print( 0, "\n" );

                    m_out->Print( aNestLevel + 2, ")\n" );
                    m_out->Print( aNestLevel + 1, ")\n" );
                    new_polygon = true;
                }
            }

            if( !is_closed ) // Should not happen, but...
                m_out->Print( aNestLevel + 1, ")\n" );
        }

        // Save the filling segments list
        const auto& segs = aZone->FillSegments( layer );

        if( segs.size() )
        {
            m_out->Print( aNestLevel + 1, "(fill_segments\n" );
            m_out->Print( aNestLevel + 2, "(layer %s)\n",
                          TO_UTF8( BOARD::GetStandardLayerName( layer ) ) );

            for( ZONE_SEGMENT_FILL::const_iterator it = segs.begin(); it != segs.end(); ++it )
            {
                m_out->Print( aNestLevel + 2, "(pts (xy %s) (xy %s))\n",
                        FormatInternalUnits( wxPoint( it->A ) ).c_str(),
                        FormatInternalUnits( wxPoint( it->B ) ).c_str() );
            }

            m_out->Print( aNestLevel + 1, ")\n" );
        }
    }

    m_out->Print( aNestLevel, ")\n" );
}


PCB_IO::PCB_IO( int aControlFlags ) :
    m_cache( 0 ),
    m_ctl( aControlFlags ),
    m_parser( new PCB_PARSER() ),
    m_mapping( new NETINFO_MAPPING() )
{
    init( 0 );
    m_out = &m_sf;
}


PCB_IO::~PCB_IO()
{
    delete m_cache;
    delete m_parser;
    delete m_mapping;
}


BOARD* PCB_IO::Load( const wxString& aFileName, BOARD* aAppendToMe, const PROPERTIES* aProperties )
{
    FILE_LINE_READER reader( aFileName );

    BOARD* board = DoLoad( reader, aAppendToMe, aProperties );

    // Give the filename to the board if it's new
    if( !aAppendToMe )
        board->SetFileName( aFileName );

    return board;
}


BOARD* PCB_IO::DoLoad( LINE_READER& aReader, BOARD* aAppendToMe, const PROPERTIES* aProperties )
{
    init( aProperties );

    m_parser->SetLineReader( &aReader );
    m_parser->SetBoard( aAppendToMe );

    BOARD* board;

    try
    {
        board = dynamic_cast<BOARD*>( m_parser->Parse() );
    }
    catch( const FUTURE_FORMAT_ERROR& )
    {
        // Don't wrap a FUTURE_FORMAT_ERROR in another
        throw;
    }
    catch( const PARSE_ERROR& parse_error )
    {
        if( m_parser->IsTooRecent() )
            throw FUTURE_FORMAT_ERROR( parse_error, m_parser->GetRequiredVersion() );
        else
            throw;
    }

    if( !board )
    {
        // The parser loaded something that was valid, but wasn't a board.
        THROW_PARSE_ERROR( _( "this file does not contain a PCB" ),
                m_parser->CurSource(), m_parser->CurLine(),
                m_parser->CurLineNumber(), m_parser->CurOffset() );
    }

    return board;
}


void PCB_IO::init( const PROPERTIES* aProperties )
{
    m_board = NULL;
    m_reader = NULL;
    m_loading_format_version = SEXPR_BOARD_FILE_VERSION;
    m_props = aProperties;
}


void PCB_IO::validateCache( const wxString& aLibraryPath, bool checkModified )
{
    if( !m_cache || !m_cache->IsPath( aLibraryPath ) || ( checkModified && m_cache->IsModified() ) )
    {
        // a spectacular episode in memory management:
        delete m_cache;
        m_cache = new FP_CACHE( this, aLibraryPath );
        m_cache->Load();
    }
}


void PCB_IO::FootprintEnumerate( wxArrayString& aFootprintNames, const wxString& aLibPath,
                                 bool aBestEfforts, const PROPERTIES* aProperties )
{
    LOCALE_IO toggle;     // toggles on, then off, the C locale.
    wxDir     dir( aLibPath );
    wxString  errorMsg;

    init( aProperties );

    try
    {
        validateCache( aLibPath );
    }
    catch( const IO_ERROR& ioe )
    {
        errorMsg = ioe.What();
    }

    // Some of the files may have been parsed correctly so we want to add the valid files to
    // the library.

    for( MODULE_CITER it = m_cache->GetModules().begin(); it != m_cache->GetModules().end(); ++it )
        aFootprintNames.Add( it->first );

    if( !errorMsg.IsEmpty() && !aBestEfforts )
        THROW_IO_ERROR( errorMsg );
}


const MODULE* PCB_IO::getFootprint( const wxString& aLibraryPath,
                                    const wxString& aFootprintName,
                                    const PROPERTIES* aProperties,
                                    bool checkModified )
{
    LOCALE_IO   toggle;     // toggles on, then off, the C locale.

    init( aProperties );

    try
    {
        validateCache( aLibraryPath, checkModified );
    }
    catch( const IO_ERROR& )
    {
        // do nothing with the error
    }

    const MODULE_MAP& mods = m_cache->GetModules();

    MODULE_CITER it = mods.find( aFootprintName );

    if( it == mods.end() )
        return nullptr;

    return it->second->GetModule();
}


const MODULE* PCB_IO::GetEnumeratedFootprint( const wxString& aLibraryPath,
                                              const wxString& aFootprintName,
                                              const PROPERTIES* aProperties )
{
    return getFootprint( aLibraryPath, aFootprintName, aProperties, false );
}


bool PCB_IO::FootprintExists( const wxString& aLibraryPath, const wxString& aFootprintName,
                              const PROPERTIES* aProperties )
{
    // Note: checking the cache sounds like a good idea, but won't catch files which differ
    // only in case.
    //
    // Since this goes out to the native filesystem, we get platform differences (ie: MSW's
    // case-insensitive filesystem) handled "for free".
    // Warning: footprint names frequently contain a point. So be careful when initializing
    // wxFileName, and use a CTOR with extension specified
    wxFileName footprintFile( aLibraryPath, aFootprintName, KiCadFootprintFileExtension );

    return footprintFile.Exists();
}


MODULE* PCB_IO::FootprintLoad( const wxString& aLibraryPath, const wxString& aFootprintName,
                               const PROPERTIES* aProperties )
{
    const MODULE* footprint = getFootprint( aLibraryPath, aFootprintName, aProperties, true );
    return footprint ? (MODULE*) footprint->Duplicate() : nullptr;
}


void PCB_IO::FootprintSave( const wxString& aLibraryPath, const MODULE* aFootprint,
                            const PROPERTIES* aProperties )
{
    LOCALE_IO   toggle;     // toggles on, then off, the C locale.

    init( aProperties );

    // In this public PLUGIN API function, we can safely assume it was
    // called for saving into a library path.
    m_ctl = CTL_FOR_LIBRARY;

    validateCache( aLibraryPath );

    if( !m_cache->IsWritable() )
    {
        if( !m_cache->Exists() )
        {
            const wxString msg = wxString::Format( _( "Library \"%s\" does not exist.\n"
                                                   "Would you like to create it?"),
                                                   GetChars( aLibraryPath ) );

            if( wxMessageBox( msg, _( "Library Not Found"), wxYES_NO | wxICON_QUESTION ) != wxYES )
                return;

            // Save throws its own IO_ERROR on failure, so no need to recreate here
            m_cache->Save( NULL );
        }
        else
        {
            wxString msg = wxString::Format( _( "Library \"%s\" is read only" ), aLibraryPath );
            THROW_IO_ERROR( msg );
        }
    }

    wxString footprintName = aFootprint->GetFPID().GetLibItemName();

    MODULE_MAP& mods = m_cache->GetModules();

    // Quietly overwrite module and delete module file from path for any by same name.
    wxFileName fn( aLibraryPath, aFootprint->GetFPID().GetLibItemName(),
                   KiCadFootprintFileExtension );

#ifndef __WINDOWS__
    // Write through symlinks, don't replace them
    if( fn.Exists( wxFILE_EXISTS_SYMLINK ) )
    {
        char buffer[ PATH_MAX + 1 ];
        ssize_t pathLen = readlink( TO_UTF8( fn.GetFullPath() ), buffer, PATH_MAX );

        if( pathLen > 0 )
        {
            buffer[ pathLen ] = '\0';
            fn.Assign( fn.GetPath() + wxT( "/" ) + wxString::FromUTF8( buffer ) );
            fn.Normalize();
        }
    }
#endif

    if( !fn.IsOk() )
    {
        THROW_IO_ERROR( wxString::Format( _( "Footprint file name \"%s\" is not valid." ),
                                          fn.GetFullPath() ) );
    }

    if( fn.FileExists() && !fn.IsFileWritable() )
    {
        THROW_IO_ERROR( wxString::Format( _( "No write permissions to delete file \"%s\"" ),
                                          fn.GetFullPath() ) );
    }

    wxString fullPath = fn.GetFullPath();
    wxString fullName = fn.GetFullName();
    MODULE_CITER it = mods.find( footprintName );

    if( it != mods.end() )
    {
        wxLogTrace( traceKicadPcbPlugin, wxT( "Removing footprint file '%s'." ), fullPath );
        mods.erase( footprintName );
        wxRemoveFile( fullPath );
    }

    // I need my own copy for the cache
    MODULE* module = static_cast<MODULE*>( aFootprint->Clone() );

    // It should have no parent, orientation should be zero, and it should be on the front layer.
    module->SetParent( nullptr );
    module->SetOrientation( 0 );

    if( module->GetLayer() != F_Cu )
    {
        auto cfg = dynamic_cast<PCBNEW_SETTINGS*>( Kiface().KifaceSettings() );

        if( cfg )
            module->Flip( module->GetPosition(), cfg->m_FlipLeftRight );
        else
            module->Flip( module->GetPosition(), false );
    }

    wxLogTrace( traceKicadPcbPlugin, wxT( "Creating s-expr footprint file '%s'." ), fullPath );
    mods.insert( footprintName, new FP_CACHE_ITEM( module, WX_FILENAME( fn.GetPath(), fullName ) ) );
    m_cache->Save( module );
}


void PCB_IO::FootprintDelete( const wxString& aLibraryPath, const wxString& aFootprintName,
                              const PROPERTIES* aProperties )
{
    LOCALE_IO   toggle;     // toggles on, then off, the C locale.

    init( aProperties );

    validateCache( aLibraryPath );

    if( !m_cache->IsWritable() )
    {
        THROW_IO_ERROR( wxString::Format( _( "Library \"%s\" is read only." ),
                                          aLibraryPath.GetData() ) );
    }

    m_cache->Remove( aFootprintName );
}



long long PCB_IO::GetLibraryTimestamp( const wxString& aLibraryPath ) const
{
    return FP_CACHE::GetTimestamp( aLibraryPath );
}


void PCB_IO::FootprintLibCreate( const wxString& aLibraryPath, const PROPERTIES* aProperties )
{
    if( wxDir::Exists( aLibraryPath ) )
    {
        THROW_IO_ERROR( wxString::Format( _( "Cannot overwrite library path \"%s\"." ),
                                          aLibraryPath.GetData() ) );
    }

    LOCALE_IO   toggle;

    init( aProperties );

    delete m_cache;
    m_cache = new FP_CACHE( this, aLibraryPath );
    m_cache->Save();
}


bool PCB_IO::FootprintLibDelete( const wxString& aLibraryPath, const PROPERTIES* aProperties )
{
    wxFileName fn;
    fn.SetPath( aLibraryPath );

    // Return if there is no library path to delete.
    if( !fn.DirExists() )
        return false;

    if( !fn.IsDirWritable() )
    {
        THROW_IO_ERROR( wxString::Format( _( "User does not have permission to delete directory \"%s\"." ),
                                          aLibraryPath.GetData() ) );
    }

    wxDir dir( aLibraryPath );

    if( dir.HasSubDirs() )
    {
        THROW_IO_ERROR( wxString::Format( _( "Library directory \"%s\" has unexpected sub-directories." ),
                                          aLibraryPath.GetData() ) );
    }

    // All the footprint files must be deleted before the directory can be deleted.
    if( dir.HasFiles() )
    {
        unsigned      i;
        wxFileName    tmp;
        wxArrayString files;

        wxDir::GetAllFiles( aLibraryPath, &files );

        for( i = 0;  i < files.GetCount();  i++ )
        {
            tmp = files[i];

            if( tmp.GetExt() != KiCadFootprintFileExtension )
            {
                THROW_IO_ERROR( wxString::Format( _( "Unexpected file \"%s\" was found in library path \"%s\"." ),
                                                  files[i].GetData(), aLibraryPath.GetData() ) );
            }
        }

        for( i = 0;  i < files.GetCount();  i++ )
            wxRemoveFile( files[i] );
    }

    wxLogTrace( traceKicadPcbPlugin, wxT( "Removing footprint library \"%s\"." ),
                aLibraryPath.GetData() );

    // Some of the more elaborate wxRemoveFile() crap puts up its own wxLog dialog
    // we don't want that.  we want bare metal portability with no UI here.
    if( !wxRmdir( aLibraryPath ) )
    {
        THROW_IO_ERROR( wxString::Format( _( "Footprint library \"%s\" cannot be deleted." ),
                                          aLibraryPath.GetData() ) );
    }

    // For some reason removing a directory in Windows is not immediately updated.  This delay
    // prevents an error when attempting to immediately recreate the same directory when over
    // writing an existing library.
#ifdef __WINDOWS__
    wxMilliSleep( 250L );
#endif

    if( m_cache && !m_cache->IsPath( aLibraryPath ) )
    {
        delete m_cache;
        m_cache = NULL;
    }

    return true;
}


bool PCB_IO::IsFootprintLibWritable( const wxString& aLibraryPath )
{
    LOCALE_IO   toggle;

    init( NULL );

    validateCache( aLibraryPath );

    return m_cache->IsWritable();
}

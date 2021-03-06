/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2020 CERN
 *
 * @author Wayne Stambaugh <stambaughw@gmail.com>
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
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <cctype>

#include <wx/mstream.h>
#include <wx/filename.h>
#include <wx/tokenzr.h>

#include <build_version.h>
#include <gal/color4d.h>
#include <pgm_base.h>
#include <gr_text.h>
#include <kiway.h>
#include <kicad_string.h>
#include <richio.h>
#include <core/typeinfo.h>
#include <plotter.h>               // PLOT_DASH_TYPE
#include <properties.h>
#include <trace_helpers.h>

#include <general.h>
#include <sch_bitmap.h>
#include <sch_bus_entry.h>
#include <sch_component.h>
#include <sch_edit_frame.h>       // COMPONENT_ORIENTATION_T
#include <sch_junction.h>
#include <sch_line.h>
#include <sch_marker.h>
#include <sch_no_connect.h>
#include <sch_text.h>
#include <sch_sheet.h>
#include <sch_bitmap.h>
#include <bus_alias.h>
#include <sch_sexpr_plugin.h>
#include <template_fieldnames.h>
#include <sch_screen.h>
#include <class_libentry.h>
#include <class_library.h>
#include <lib_arc.h>
#include <lib_bezier.h>
#include <lib_circle.h>
#include <lib_field.h>
#include <lib_pin.h>
#include <lib_polyline.h>
#include <lib_rectangle.h>
#include <lib_text.h>
#include <pin_shape.h>
#include <pin_type.h>
#include <eeschema_id.h>       // for MAX_UNIT_COUNT_PER_PACKAGE definition
#include <sch_file_versions.h>
#include <schematic_lexer.h>
#include <sch_sexpr_parser.h>
#include <symbol_lib_table.h>  // for PropPowerSymsOnly definintion.
#include <confirm.h>
#include <tool/selection.h>

using namespace TSYMBOL_LIB_T;


#define SCH_PARSE_ERROR( text, reader, pos )                         \
    THROW_PARSE_ERROR( text, reader.GetSource(), reader.Line(),      \
                       reader.LineNumber(), pos - reader.Line() )


static const char* emptyString = "";

/**
 * Fill token formatting helper.
 */
static void formatFill( const LIB_ITEM* aItem, OUTPUTFORMATTER& aFormatter, int aNestLevel )
{
    wxCHECK_RET( aItem && aItem->IsFillable() && aItem->GetFillMode() != NO_FILL,
                 "Invalid fill item." );

    aFormatter.Print( aNestLevel, "(fill (type %s))",
                      aItem->GetFillMode() == FILLED_SHAPE ? "outline" : "background" );
}


static const char* getPinElectricalTypeToken( ELECTRICAL_PINTYPE aType )
{
    switch( aType )
    {
    case ELECTRICAL_PINTYPE::PT_INPUT:
        return SCHEMATIC_LEXER::TokenName( T_input );

    case ELECTRICAL_PINTYPE::PT_OUTPUT:
        return SCHEMATIC_LEXER::TokenName( T_output );

    case ELECTRICAL_PINTYPE::PT_BIDI:
        return SCHEMATIC_LEXER::TokenName( T_bidirectional );

    case ELECTRICAL_PINTYPE::PT_TRISTATE:
        return SCHEMATIC_LEXER::TokenName( T_tri_state );

    case ELECTRICAL_PINTYPE::PT_PASSIVE:
        return SCHEMATIC_LEXER::TokenName( T_passive );

    case ELECTRICAL_PINTYPE::PT_UNSPECIFIED:
        return SCHEMATIC_LEXER::TokenName( T_unspecified );

    case ELECTRICAL_PINTYPE::PT_POWER_IN:
        return SCHEMATIC_LEXER::TokenName( T_power_in );

    case ELECTRICAL_PINTYPE::PT_POWER_OUT:
        return SCHEMATIC_LEXER::TokenName( T_power_out );

    case ELECTRICAL_PINTYPE::PT_OPENCOLLECTOR:
        return SCHEMATIC_LEXER::TokenName( T_open_collector );

    case ELECTRICAL_PINTYPE::PT_OPENEMITTER:
        return SCHEMATIC_LEXER::TokenName( T_open_emitter );

    case ELECTRICAL_PINTYPE::PT_NC:
        return SCHEMATIC_LEXER::TokenName( T_unconnected );

    default:
        wxFAIL_MSG( "Missing symbol library pin connection type" );
    }

    return emptyString;
}


static const char* getPinShapeToken( GRAPHIC_PINSHAPE aShape )
{
    switch( aShape )
    {
    case GRAPHIC_PINSHAPE::LINE:
        return SCHEMATIC_LEXER::TokenName( T_line );

    case GRAPHIC_PINSHAPE::INVERTED:
        return SCHEMATIC_LEXER::TokenName( T_inverted );

    case GRAPHIC_PINSHAPE::CLOCK:
        return SCHEMATIC_LEXER::TokenName( T_clock );

    case GRAPHIC_PINSHAPE::INVERTED_CLOCK:
        return SCHEMATIC_LEXER::TokenName( T_inverted_clock );

    case GRAPHIC_PINSHAPE::INPUT_LOW:
        return SCHEMATIC_LEXER::TokenName( T_input_low );

    case GRAPHIC_PINSHAPE::CLOCK_LOW:
        return SCHEMATIC_LEXER::TokenName( T_clock_low );

    case GRAPHIC_PINSHAPE::OUTPUT_LOW:
        return SCHEMATIC_LEXER::TokenName( T_output_low );

    case GRAPHIC_PINSHAPE::FALLING_EDGE_CLOCK:
        return SCHEMATIC_LEXER::TokenName( T_edge_clock_high );

    case GRAPHIC_PINSHAPE::NONLOGIC:
        return SCHEMATIC_LEXER::TokenName( T_non_logic );

    default:
        wxFAIL_MSG( "Missing symbol library pin shape type" );
    }

    return emptyString;
}


static float getPinAngle( int aOrientation )
{
    switch( aOrientation )
    {
    case PIN_RIGHT:
        return 0.0;

    case PIN_LEFT:
        return 180.0;

    case PIN_UP:
        return 90.0;

    case PIN_DOWN:
        return 270.0;

    default:
        wxFAIL_MSG( "Missing symbol library pin orientation type" );

    return 0.0;
    }
}


static const char* getSheetPinShapeToken( PINSHEETLABEL_SHAPE aShape )
{
    switch( aShape )
    {
    case PINSHEETLABEL_SHAPE::PS_INPUT:       return SCHEMATIC_LEXER::TokenName( T_input );
    case PINSHEETLABEL_SHAPE::PS_OUTPUT:      return SCHEMATIC_LEXER::TokenName( T_output );
    case PINSHEETLABEL_SHAPE::PS_BIDI:        return SCHEMATIC_LEXER::TokenName( T_bidirectional );
    case PINSHEETLABEL_SHAPE::PS_TRISTATE:    return SCHEMATIC_LEXER::TokenName( T_tri_state );
    case PINSHEETLABEL_SHAPE::PS_UNSPECIFIED: return SCHEMATIC_LEXER::TokenName( T_passive );
    default:         wxFAIL;                  return SCHEMATIC_LEXER::TokenName( T_passive );
    }
}


static double getSheetPinAngle( SHEET_SIDE aSide )
{
    double retv;

    switch( aSide )
    {
    case SHEET_UNDEFINED_SIDE:
    case SHEET_LEFT_SIDE:       retv = 180.0;  break;
    case SHEET_RIGHT_SIDE:      retv = 0.0;    break;
    case SHEET_TOP_SIDE:        retv = 90.0;   break;
    case SHEET_BOTTOM_SIDE:     retv = 270.0;  break;
    default:   wxFAIL;          retv = 0.0;    break;
    }

    return retv;
}


static wxString getLineStyleToken( PLOT_DASH_TYPE aStyle )
{
    wxString token;

    switch( aStyle )
    {
    case PLOT_DASH_TYPE::DASH:     token = "dash";      break;
    case PLOT_DASH_TYPE::DOT:      token = "dot";       break;
    case PLOT_DASH_TYPE::DASHDOT:  token = "dash_dot";  break;
    case PLOT_DASH_TYPE::SOLID:
    default:                       token = "solid";     break;
    }

    return token;
}


static const char* getTextTypeToken( KICAD_T aType )
{
    switch( aType )
    {
    case SCH_TEXT_T:          return SCHEMATIC_LEXER::TokenName( T_text );
    case SCH_LABEL_T:         return SCHEMATIC_LEXER::TokenName( T_label );
    case SCH_GLOBAL_LABEL_T:  return SCHEMATIC_LEXER::TokenName( T_global_label );
    case SCH_HIER_LABEL_T:    return SCHEMATIC_LEXER::TokenName( T_hierarchical_label );
    default:     wxFAIL;      return SCHEMATIC_LEXER::TokenName( T_text );
    }
}


/**
 * Write stroke definition to \a aFormatter.
 *
 * This only writes the stroke definition if \a aWidth, \a aStyle and \a aColor are
 * not the default setting or are not defined.
 *
 * @param aFormatter A pointer to the #OUTPUTFORMATTER object to write to.
 * @param aNestLevel The nest level to indent the stroke definition.
 * @param aWidth The stroke line width in internal units.
 * @param aStyle The stroke line style.
 * @param aColor The stroke line color.
 */
static void formatStroke( OUTPUTFORMATTER* aFormatter, int aNestLevel, int aWidth,
                          PLOT_DASH_TYPE aStyle, const COLOR4D& aColor )
{
    wxASSERT( aFormatter != nullptr );

    aFormatter->Print( aNestLevel, "(stroke" );

    if( !( aWidth == GetDefaultLineThickness() || aWidth == 0 ) )
        aFormatter->Print( 0, " (width %s)", FormatInternalUnits( aWidth ).c_str() );

    if( !( aStyle == PLOT_DASH_TYPE::DEFAULT || aStyle == PLOT_DASH_TYPE::SOLID ) )
        aFormatter->Print( 0, " (type %s)", TO_UTF8( getLineStyleToken( aStyle ) ) );

    if( !( aColor == COLOR4D::UNSPECIFIED ) )
        aFormatter->Print( 0, " (color %d %d %d %0.4f)",
                           static_cast<int>( aColor.r * 255.0 + 0.5 ),
                           static_cast<int>( aColor.g * 255.0 + 0.5 ),
                           static_cast<int>( aColor.b * 255.0 + 0.5 ),
                           aColor.a );

    aFormatter->Print( 0, ")" );
}


/**
 * A cache assistant for the part library portion of the #SCH_PLUGIN API, and only for the
 * #SCH_SEXPR_PLUGIN, so therefore is private to this implementation file, i.e. not placed
 * into a header.
 */
class SCH_SEXPR_PLUGIN_CACHE
{
    static int      m_modHash;      // Keep track of the modification status of the library.

    wxString        m_fileName;     // Absolute path and file name.
    wxFileName      m_libFileName;  // Absolute path and file name is required here.
    wxDateTime      m_fileModTime;
    LIB_PART_MAP    m_symbols;      // Map of names of #LIB_PART pointers.
    bool            m_isWritable;
    bool            m_isModified;
    int             m_versionMajor;
    int             m_versionMinor;
    int             m_libType;      // Is this cache a component or symbol library.

    static FILL_T   parseFillMode( LINE_READER& aReader, const char* aLine,
                                   const char** aOutput );
    LIB_PART*       removeSymbol( LIB_PART* aAlias );

    static void     saveSymbolDrawItem( LIB_ITEM* aItem, OUTPUTFORMATTER& aFormatter,
                                        int aNestLevel );
    static void     saveArc( LIB_ARC* aArc, OUTPUTFORMATTER& aFormatter, int aNestLevel = 0 );
    static void     saveBezier( LIB_BEZIER* aBezier, OUTPUTFORMATTER& aFormatter,
                                int aNestLevel = 0 );
    static void     saveCircle( LIB_CIRCLE* aCircle, OUTPUTFORMATTER& aFormatter,
                                int aNestLevel = 0 );
    static void     saveField( LIB_FIELD* aField, OUTPUTFORMATTER& aFormatter, int aNestLevel = 0 );
    static void     savePin( LIB_PIN* aPin, OUTPUTFORMATTER& aFormatter, int aNestLevel = 0 );
    static void     savePolyLine( LIB_POLYLINE* aPolyLine, OUTPUTFORMATTER& aFormatter,
                                  int aNestLevel = 0 );
    static void     saveRectangle( LIB_RECTANGLE* aRectangle, OUTPUTFORMATTER& aFormatter,
                                   int aNestLevel = 0 );
    static void     saveText( LIB_TEXT* aText, OUTPUTFORMATTER& aFormatter, int aNestLevel = 0 );

    static void     saveDcmInfoAsFields( LIB_PART* aSymbol, OUTPUTFORMATTER& aFormatter,
                                         int aNestLevel = 0, int aFirstId = MANDATORY_FIELDS );

    friend SCH_SEXPR_PLUGIN;

public:
    SCH_SEXPR_PLUGIN_CACHE( const wxString& aLibraryPath );
    ~SCH_SEXPR_PLUGIN_CACHE();

    int GetModifyHash() const { return m_modHash; }

    // Most all functions in this class throw IO_ERROR exceptions.  There are no
    // error codes nor user interface calls from here, nor in any SCH_PLUGIN objects.
    // Catch these exceptions higher up please.

    /// Save the entire library to file m_libFileName;
    void Save();

    void Load();

    void AddSymbol( const LIB_PART* aPart );

    void DeleteSymbol( const wxString& aName );

    // If m_libFileName is a symlink follow it to the real source file
    wxFileName GetRealFile() const;

    wxDateTime GetLibModificationTime();

    bool IsFile( const wxString& aFullPathAndFileName ) const;

    bool IsFileChanged() const;

    void SetModified( bool aModified = true ) { m_isModified = aModified; }

    wxString GetLogicalName() const { return m_libFileName.GetName(); }

    void SetFileName( const wxString& aFileName ) { m_libFileName = aFileName; }

    wxString GetFileName() const { return m_libFileName.GetFullPath(); }

    static LIB_PART* LoadPart( LINE_READER& aReader, int aMajorVersion, int aMinorVersion,
                               LIB_PART_MAP* aMap = nullptr );
    static void      SaveSymbol( LIB_PART* aSymbol, OUTPUTFORMATTER& aFormatter,
                                 int aNestLevel = 0, LIB_PART_MAP* aMap = nullptr );
};


SCH_SEXPR_PLUGIN::SCH_SEXPR_PLUGIN()
{
    init( NULL );
}


SCH_SEXPR_PLUGIN::~SCH_SEXPR_PLUGIN()
{
    delete m_cache;
}


void SCH_SEXPR_PLUGIN::init( KIWAY* aKiway, const PROPERTIES* aProperties )
{
    m_version = 0;
    m_rootSheet = NULL;
    m_props = aProperties;
    m_kiway = aKiway;
    m_cache = NULL;
    m_out = NULL;
}


SCH_SHEET* SCH_SEXPR_PLUGIN::Load( const wxString& aFileName, KIWAY* aKiway,
                                   SCH_SHEET* aAppendToMe, const PROPERTIES* aProperties )
{
    wxASSERT( !aFileName || aKiway != NULL );

    LOCALE_IO   toggle;     // toggles on, then off, the C locale.
    SCH_SHEET*  sheet;

    wxFileName fn = aFileName;

    // Unfortunately child sheet file names the legacy schematic file format are not fully
    // qualified and are always appended to the project path.  The aFileName attribute must
    // always be an absolute path so the project path can be used for load child sheet files.
    wxASSERT( fn.IsAbsolute() );

    if( aAppendToMe )
    {
        wxLogTrace( traceSchLegacyPlugin, "Append \"%s\" to sheet \"%s\".",
                    aFileName, aAppendToMe->GetFileName() );

        wxFileName normedFn = aAppendToMe->GetFileName();

        if( !normedFn.IsAbsolute() )
        {
            if( aFileName.Right( normedFn.GetFullPath().Length() ) == normedFn.GetFullPath() )
                m_path = aFileName.Left( aFileName.Length() - normedFn.GetFullPath().Length() );
        }

        if( m_path.IsEmpty() )
            m_path = aKiway->Prj().GetProjectPath();

        wxLogTrace( traceSchLegacyPlugin, "Normalized append path \"%s\".", m_path );
    }
    else
    {
        m_path = aKiway->Prj().GetProjectPath();
    }

    m_currentPath.push( m_path );
    init( aKiway, aProperties );

    if( aAppendToMe == NULL )
    {
        // Clean up any allocated memory if an exception occurs loading the schematic.
        std::unique_ptr< SCH_SHEET > newSheet( new SCH_SHEET );
        newSheet->SetFileName( aFileName );
        m_rootSheet = newSheet.get();
        loadHierarchy( newSheet.get() );

        // If we got here, the schematic loaded successfully.
        sheet = newSheet.release();
    }
    else
    {
        m_rootSheet = aAppendToMe->GetRootSheet();
        wxASSERT( m_rootSheet != NULL );
        sheet = aAppendToMe;
        loadHierarchy( sheet );
    }

    wxASSERT( m_currentPath.size() == 1 );  // only the project path should remain

    return sheet;
}


// Everything below this comment is recursive.  Modify with care.

void SCH_SEXPR_PLUGIN::loadHierarchy( SCH_SHEET* aSheet )
{
    SCH_SCREEN* screen = NULL;

    if( !aSheet->GetScreen() )
    {
        // SCH_SCREEN objects store the full path and file name where the SCH_SHEET object only
        // stores the file name and extension.  Add the project path to the file name and
        // extension to compare when calling SCH_SHEET::SearchHierarchy().
        wxFileName fileName = aSheet->GetFileName();

        if( !fileName.IsAbsolute() )
            fileName.MakeAbsolute( m_currentPath.top() );

        // Save the current path so that it gets restored when decending and ascending the
        // sheet hierarchy which allows for sheet schematic files to be nested in folders
        // relative to the last path a schematic was loaded from.
        wxLogTrace( traceSchLegacyPlugin, "Saving path    \"%s\"", m_currentPath.top() );
        m_currentPath.push( fileName.GetPath() );
        wxLogTrace( traceSchLegacyPlugin, "Current path   \"%s\"", m_currentPath.top() );
        wxLogTrace( traceSchLegacyPlugin, "Loading        \"%s\"", fileName.GetFullPath() );

        m_rootSheet->SearchHierarchy( fileName.GetFullPath(), &screen );

        if( screen )
        {
            aSheet->SetScreen( screen );

            // Do not need to load the sub-sheets - this has already been done.
        }
        else
        {
            aSheet->SetScreen( new SCH_SCREEN( m_kiway ) );
            aSheet->GetScreen()->SetFileName( fileName.GetFullPath() );

            try
            {
                loadFile( fileName.GetFullPath(), aSheet->GetScreen() );

                for( auto aItem : aSheet->GetScreen()->Items().OfType( SCH_SHEET_T ) )
                {
                    assert( aItem->Type() == SCH_SHEET_T );
                    auto sheet = static_cast<SCH_SHEET*>( aItem );

                    // Set the parent to aSheet.  This effectively creates a method to find
                    // the root sheet from any sheet so a pointer to the root sheet does not
                    // need to be stored globally.  Note: this is not the same as a hierarchy.
                    // Complex hierarchies can have multiple copies of a sheet.  This only
                    // provides a simple tree to find the root sheet.
                    sheet->SetParent( aSheet );

                    // Recursion starts here.
                    loadHierarchy( sheet );
                }
            }
            catch( const IO_ERROR& ioe )
            {
                // If there is a problem loading the root sheet, there is no recovery.
                if( aSheet == m_rootSheet )
                    throw( ioe );

                // For all subsheets, queue up the error message for the caller.
                if( !m_error.IsEmpty() )
                    m_error += "\n";

                m_error += ioe.What();
            }
        }

        m_currentPath.pop();
        wxLogTrace( traceSchLegacyPlugin, "Restoring path \"%s\"", m_currentPath.top() );
    }
}


void SCH_SEXPR_PLUGIN::loadFile( const wxString& aFileName, SCH_SCREEN* aScreen )
{
    FILE_LINE_READER reader( aFileName );

    loadHeader( reader, aScreen );

    LoadContent( reader, aScreen, m_version );
}


void SCH_SEXPR_PLUGIN::LoadContent( LINE_READER& aReader, SCH_SCREEN* aScreen, int version )
{
    m_version = version;
}


void SCH_SEXPR_PLUGIN::loadHeader( LINE_READER& aReader, SCH_SCREEN* aScreen )
{
}


void SCH_SEXPR_PLUGIN::loadPageSettings( LINE_READER& aReader, SCH_SCREEN* aScreen )
{
    wxASSERT( aScreen != NULL );

    wxString    buf;
    PAGE_INFO   pageInfo;
    TITLE_BLOCK tb;
}


SCH_SHEET* SCH_SEXPR_PLUGIN::loadSheet( LINE_READER& aReader )
{
    std::unique_ptr< SCH_SHEET > sheet( new SCH_SHEET() );

    // Timesheet time stamps are now automatically generated.

    return sheet.release();
}


SCH_BITMAP* SCH_SEXPR_PLUGIN::loadBitmap( LINE_READER& aReader )
{
    std::unique_ptr< SCH_BITMAP > bitmap( new SCH_BITMAP );

    return bitmap.release();
}


SCH_JUNCTION* SCH_SEXPR_PLUGIN::loadJunction( LINE_READER& aReader )
{
    std::unique_ptr< SCH_JUNCTION > junction( new SCH_JUNCTION );

    return junction.release();
}


SCH_NO_CONNECT* SCH_SEXPR_PLUGIN::loadNoConnect( LINE_READER& aReader )
{
    std::unique_ptr< SCH_NO_CONNECT > no_connect( new SCH_NO_CONNECT );

    return no_connect.release();
}


SCH_LINE* SCH_SEXPR_PLUGIN::loadWire( LINE_READER& aReader )
{
    std::unique_ptr< SCH_LINE > wire( new SCH_LINE );

    return wire.release();
}


SCH_BUS_ENTRY_BASE* SCH_SEXPR_PLUGIN::loadBusEntry( LINE_READER& aReader )
{
    std::unique_ptr< SCH_BUS_ENTRY_BASE > busEntry;

    return busEntry.release();
}


SCH_TEXT* SCH_SEXPR_PLUGIN::loadText( LINE_READER& aReader )
{
    std::unique_ptr< SCH_TEXT> text;

    return text.release();
}


SCH_COMPONENT* SCH_SEXPR_PLUGIN::loadComponent( LINE_READER& aReader )
{
    std::unique_ptr< SCH_COMPONENT > component( new SCH_COMPONENT() );

    return component.release();
}


std::shared_ptr<BUS_ALIAS> SCH_SEXPR_PLUGIN::loadBusAlias( LINE_READER& aReader,
                                                           SCH_SCREEN* aScreen )
{
    auto busAlias = std::make_shared< BUS_ALIAS >( aScreen );

    return busAlias;
}


void SCH_SEXPR_PLUGIN::Save( const wxString& aFileName, SCH_SCREEN* aScreen, KIWAY* aKiway,
                             const PROPERTIES* aProperties )
{
    wxCHECK_RET( aScreen != NULL, "NULL SCH_SCREEN object." );
    wxCHECK_RET( !aFileName.IsEmpty(), "No schematic file name defined." );

    LOCALE_IO   toggle;     // toggles on, then off, the C locale, to write floating point values.

    init( aKiway, aProperties );

    wxFileName fn = aFileName;

    // File names should be absolute.  Don't assume everything relative to the project path
    // works properly.
    wxASSERT( fn.IsAbsolute() );

    FILE_OUTPUTFORMATTER formatter( fn.GetFullPath() );

    m_out = &formatter;     // no ownership

    Format( aScreen );
}


void SCH_SEXPR_PLUGIN::Format( SCH_SCREEN* aScreen )
{
    wxCHECK_RET( aScreen != NULL, "NULL SCH_SCREEN* object." );
    wxCHECK_RET( m_kiway != NULL, "NULL KIWAY* object." );

    m_out->Print( 0, "(kicad_sch (version %d) (host eeschema %s)\n\n",
                  SEXPR_SCHEMATIC_FILE_VERSION,
                  m_out->Quotew( GetBuildVersion() ).c_str() );

    // @todo save cache library here.

    aScreen->GetPageSettings().Format( m_out, 1, 0 );
    m_out->Print( 0, "\n" );
    aScreen->GetTitleBlock().Format( m_out, 1, 0 );

    // @todo save schematic instance information (page #).

    for( const auto& alias : aScreen->GetBusAliases() )
    {
        saveBusAlias( alias, 1 );
    }

    // Enforce item ordering
    auto cmp = []( const SCH_ITEM* a, const SCH_ITEM* b ) { return *a < *b; };
    std::multiset<SCH_ITEM*, decltype( cmp )> save_map( cmp );

    for( auto item : aScreen->Items() )
        save_map.insert( item );

    KICAD_T itemType = TYPE_NOT_INIT;

    for( auto& item : save_map )
    {
        if( itemType != item->Type() )
        {
            itemType = item->Type();

            if( itemType != SCH_COMPONENT_T
              && itemType != SCH_JUNCTION_T
              && itemType != SCH_SHEET_T )
                m_out->Print( 0, "\n" );
        }

        switch( item->Type() )
        {
        case SCH_COMPONENT_T:
            m_out->Print( 0, "\n" );
            saveSymbol( static_cast<SCH_COMPONENT*>( item ), 1 );
            break;
        case SCH_BITMAP_T:
            saveBitmap( static_cast<SCH_BITMAP*>( item ), 1 );
            break;
        case SCH_SHEET_T:
            m_out->Print( 0, "\n" );
            saveSheet( static_cast<SCH_SHEET*>( item ), 1 );
            break;
        case SCH_JUNCTION_T:
            saveJunction( static_cast<SCH_JUNCTION*>( item ), 1 );
            break;
        case SCH_NO_CONNECT_T:
            saveNoConnect( static_cast<SCH_NO_CONNECT*>( item ), 1 );
            break;
        case SCH_BUS_WIRE_ENTRY_T:
        case SCH_BUS_BUS_ENTRY_T:
            saveBusEntry( static_cast<SCH_BUS_ENTRY_BASE*>( item ), 1 );
            break;
        case SCH_LINE_T:
            saveLine( static_cast<SCH_LINE*>( item ), 1 );
            break;
        case SCH_TEXT_T:
        case SCH_LABEL_T:
        case SCH_GLOBAL_LABEL_T:
        case SCH_HIER_LABEL_T:
            saveText( static_cast<SCH_TEXT*>( item ), 1 );
            break;
        default:
            wxASSERT( "Unexpected schematic object type in SCH_SEXPR_PLUGIN::Format()" );
        }
    }

    m_out->Print( 0, ")\n" );
}


void SCH_SEXPR_PLUGIN::Format( SELECTION* aSelection, OUTPUTFORMATTER* aFormatter )
{
    m_out = aFormatter;

    for( unsigned i = 0; i < aSelection->GetSize(); ++i )
    {
        SCH_ITEM* item = (SCH_ITEM*) aSelection->GetItem( i );

        switch( item->Type() )
        {
        case SCH_COMPONENT_T:
            saveSymbol( static_cast< SCH_COMPONENT* >( item ), 0 );
            break;
        case SCH_BITMAP_T:
            saveBitmap( static_cast< SCH_BITMAP* >( item ), 0 );
            break;
        case SCH_SHEET_T:
            saveSheet( static_cast< SCH_SHEET* >( item ), 0 );
            break;
        case SCH_JUNCTION_T:
            saveJunction( static_cast< SCH_JUNCTION* >( item ), 0 );
            break;
        case SCH_NO_CONNECT_T:
            saveNoConnect( static_cast< SCH_NO_CONNECT* >( item ), 0 );
            break;
        case SCH_BUS_WIRE_ENTRY_T:
        case SCH_BUS_BUS_ENTRY_T:
            saveBusEntry( static_cast< SCH_BUS_ENTRY_BASE* >( item ), 0 );
            break;
        case SCH_LINE_T:
            saveLine( static_cast< SCH_LINE* >( item ), 0 );
            break;
        case SCH_TEXT_T:
        case SCH_LABEL_T:
        case SCH_GLOBAL_LABEL_T:
        case SCH_HIER_LABEL_T:
            saveText( static_cast< SCH_TEXT* >( item ), 0 );
            break;
        default:
            wxASSERT( "Unexpected schematic object type in SCH_SEXPR_PLUGIN::Format()" );
        }
    }
}


void SCH_SEXPR_PLUGIN::saveSymbol( SCH_COMPONENT* aSymbol, int aNestLevel )
{
    wxCHECK_RET( aSymbol != nullptr && m_out != nullptr, "" );

    std::string     name1;
    std::string     name2;
    wxArrayString   reference_fields;

    static wxString delimiters( wxT( " " ) );

    // This is redundant with the AR entries below, but it makes the files backwards-compatible.
    if( aSymbol->GetInstanceReferences().size() > 0 )
    {
        const COMPONENT_INSTANCE_REFERENCE& instance = aSymbol->GetInstanceReferences()[0];
        name1 = toUTFTildaText( instance.m_Reference );
    }
    else
    {
        if( aSymbol->GetField( REFERENCE )->GetText().IsEmpty() )
            name1 = toUTFTildaText( aSymbol->GetPrefix() );
        else
            name1 = toUTFTildaText( aSymbol->GetField( REFERENCE )->GetText() );
    }

    wxString part_name = aSymbol->GetLibId().Format();

    if( part_name.size() )
    {
        name2 = toUTFTildaText( part_name );
    }
    else
    {
        name2 = "_NONAME_";
    }

    double angle;
    int orientation = aSymbol->GetOrientation() & ~( CMP_MIRROR_X | CMP_MIRROR_Y );

    if( orientation == CMP_ORIENT_90 )
        angle = 90.0;
    else if( orientation == CMP_ORIENT_180 )
        angle = 180.0;
    else if( orientation == CMP_ORIENT_270 )
        angle = 270;
    else
        angle = 0.0;

    m_out->Print( aNestLevel, "(symbol %s is %s (at %d %d %g)",
                  m_out->Quotew( name1 ).c_str(), m_out->Quotew( name2 ).c_str(),
                  aSymbol->GetPosition().x, aSymbol->GetPosition().y, angle );

    bool mirrorX = aSymbol->GetOrientation() & CMP_MIRROR_X;
    bool mirrorY = aSymbol->GetOrientation() & CMP_MIRROR_Y;

    if( mirrorX || mirrorY )
    {
        m_out->Print( 0, "(mirror" );

        if( mirrorX )
            m_out->Print( 0, " x" );

        if( mirrorY )
            m_out->Print( 0, " y" );

        m_out->Print( 0, ")\n" );
    }
    else
    {
        m_out->Print( 0, "\n" );
    }

    // @todo Convert to full UUID if current UUID is a legacy time stamp.
    m_out->Print( aNestLevel + 1, "(uuid %s)\n",
                  m_out->Quotew( aSymbol->m_Uuid.AsString() ).c_str() );

    for( auto field : aSymbol->GetFields() )
    {
        saveField( field, aNestLevel + 1 );
    }

    // @todo Save sheet UUID at top level of schematic file.  This will require saving from
    //       the SCH_SHEET object instead of the SCH_SCREEN object.

    // KIID projectId;
    // wxString projectName = "unknown";
    // SCH_SHEET* sheet = dynamic_cast<SCH_SHEET*>( aSymbol->GetParent() );

    // if( sheet )
    // {
    //     SCH_SHEET* rootSheet = sheet->GetRootSheet();

    //     wxASSERT( rootSheet );

    //     projectName = rootSheet->GetName();
    //     projectId = rootSheet->m_Uuid;
    // }

    // For simple hierarchies, the reference is defined by the reference property (field).
    if( aSymbol->GetInstanceReferences().size() > 1 )
    {
        m_out->Print( aNestLevel + 1, "(instances\n" );

        // @todo Group project level instances.
        for( const auto instance : aSymbol->GetInstanceReferences() )
        {
            wxString path = "/";

            // Skip root sheet
            for( int i = 1; i < (int) instance.m_Path.size(); ++i )
                path += instance.m_Path[i].AsString() + "/";

            m_out->Print( aNestLevel + 2, "(path %s (reference %s) (unit %d))\n",
                          m_out->Quotew( path ).c_str(),
                          m_out->Quotew( instance.m_Reference ).c_str(),
                          instance.m_Unit );
        }

        m_out->Print( aNestLevel + 1, ")\n" );
    }

    m_out->Print( aNestLevel, ")\n" );
}


void SCH_SEXPR_PLUGIN::saveField( SCH_FIELD* aField, int aNestLevel )
{
    wxCHECK_RET( aField != nullptr && m_out != nullptr, "" );

    wxString fieldName;

    if( aField->GetId() < MANDATORY_FIELDS )
        fieldName = "ki_" + TEMPLATE_FIELDNAME::GetDefaultFieldName( aField->GetId() ).Lower();
    else
        fieldName = aField->GetName();

    m_out->Print( aNestLevel, "(property %s %s (at %d %d %g)",
                  m_out->Quotew( fieldName ).c_str(),
                  m_out->Quotew( aField->GetText() ).c_str(),
                  aField->GetPosition().x, aField->GetPosition().y,
                  aField->GetTextAngleDegrees() );

    if( !aField->IsDefaultFormatting() )
    {
        m_out->Print( 0, "\n" );
        aField->Format( m_out, aNestLevel, 0 );
        m_out->Print( aNestLevel, ")\n" );   // Closes property token with font effects.
    }
    else
    {
        m_out->Print( 0, ")\n" );            // Closes property token without font effects.
    }
}


void SCH_SEXPR_PLUGIN::saveBitmap( SCH_BITMAP* aBitmap, int aNestLevel )
{
    wxCHECK_RET( aBitmap != nullptr && m_out != nullptr, "" );

    const wxImage* image = aBitmap->GetImage()->GetImageData();

    wxCHECK_RET( image != NULL, "wxImage* is NULL" );

    m_out->Print( aNestLevel, "(image (at %d %d)",
                  aBitmap->GetPosition().x, aBitmap->GetPosition().y );

    if( aBitmap->GetImage()->GetScale() != 1.0 )
        m_out->Print( 0, " (scale %g)", aBitmap->GetImage()->GetScale() );

    m_out->Print( 0, "\n" );
    m_out->Print( aNestLevel + 1, "(data" );

    wxMemoryOutputStream stream;

    image->SaveFile( stream, wxBITMAP_TYPE_PNG );

    // Write binary data in hexadecimal form (ASCII)
    wxStreamBuffer* buffer = stream.GetOutputStreamBuffer();
    char*           begin  = (char*) buffer->GetBufferStart();

    for( int ii = 0; begin < buffer->GetBufferEnd(); begin++, ii++ )
    {
        if( ii % 32 )
        {

            m_out->Print( 0, " %2.2X ", *begin & 0xFF );
        }
        else
        {
            ii = 0;
            m_out->Print( 0, "\n" );
            m_out->Print( aNestLevel + 2, "%2.2X ", *begin & 0xFF );
        }
    }

    m_out->Print( aNestLevel + 1, ")\n" );  // Closes data token.
    m_out->Print( aNestLevel, ")\n" );      // Closes image token.
}


void SCH_SEXPR_PLUGIN::saveSheet( SCH_SHEET* aSheet, int aNestLevel )
{
    wxCHECK_RET( aSheet != nullptr && m_out != nullptr, "" );

    m_out->Print( aNestLevel, "(sheet (start %s %s) (end %s %s)",
                  FormatInternalUnits( aSheet->GetPosition().x ).c_str(),
                  FormatInternalUnits( aSheet->GetPosition().y ).c_str(),
                  FormatInternalUnits( aSheet->GetSize().GetWidth() ).c_str(),
                  FormatInternalUnits( aSheet->GetSize().GetHeight() ).c_str() );

    if( aSheet->UsesDefaultStroke() )
    {
        m_out->Print( 0, " " );
        formatStroke( m_out, 0, aSheet->GetBorderWidth(), PLOT_DASH_TYPE::SOLID,
                      aSheet->GetBorderColor() );
    }

    m_out->Print( 0, "\n" );
    m_out->Print( aNestLevel + 1, "(uuid %s)", TO_UTF8( aSheet->m_Uuid.AsString() ) );
    m_out->Print( 0, "\n" );

    for( auto field : aSheet->GetFields() )
    {
        saveField( &field, aNestLevel + 1 );
    }

    for( const auto pin : aSheet->GetPins() )
    {
        m_out->Print( aNestLevel + 1, "(pin %s %s at( %s %s %g)",
                      EscapedUTF8( pin->GetText() ).c_str(),
                      getSheetPinShapeToken( pin->GetShape() ),
                      FormatInternalUnits( pin->GetPosition().x ).c_str(),
                      FormatInternalUnits( pin->GetPosition().y ).c_str(),
                      getSheetPinAngle( pin->GetEdge() ) );

        if( !pin->IsDefaultFormatting() )
        {
            m_out->Print( 0, "\n" );
            pin->Format( m_out, aNestLevel + 1, 0 );
            m_out->Print( aNestLevel + 1, ")\n" );  // Closes pin token with font effects.
        }
        else
        {
            m_out->Print( 0, ")\n" );               // Closes pin token without font effects.
        }
    }

    m_out->Print( aNestLevel, ")\n" );              // Closes sheet token.
}


void SCH_SEXPR_PLUGIN::saveJunction( SCH_JUNCTION* aJunction, int aNestLevel )
{
    wxCHECK_RET( aJunction != nullptr && m_out != nullptr, "" );

    m_out->Print( aNestLevel, "(junction (at %s %s))\n",
                  FormatInternalUnits( aJunction->GetPosition().x ).c_str(),
                  FormatInternalUnits( aJunction->GetPosition().y ).c_str() );
}


void SCH_SEXPR_PLUGIN::saveNoConnect( SCH_NO_CONNECT* aNoConnect, int aNestLevel )
{
    wxCHECK_RET( aNoConnect != nullptr && m_out != nullptr, "" );

    m_out->Print( aNestLevel, "(no_connect (at %s %s))\n",
                  FormatInternalUnits( aNoConnect->GetPosition().x ).c_str(),
                  FormatInternalUnits( aNoConnect->GetPosition().y ).c_str() );
}


void SCH_SEXPR_PLUGIN::saveBusEntry( SCH_BUS_ENTRY_BASE* aBusEntry, int aNestLevel )
{
    wxCHECK_RET( aBusEntry != nullptr && m_out != nullptr, "" );

    // Bus to bus entries are converted to bus line segments.
    if( aBusEntry->GetClass() == "SCH_BUS_BUS_ENTRY" )
    {
        SCH_LINE busEntryLine( aBusEntry->GetPosition(), LAYER_BUS );

        busEntryLine.SetEndPoint( aBusEntry->m_End() );
        saveLine( &busEntryLine, aNestLevel );
    }
    else
    {
        m_out->Print( aNestLevel, "(bus_entry (start %s %s) (end %s %s))\n",
                      FormatInternalUnits( aBusEntry->GetPosition().x ).c_str(),
                      FormatInternalUnits( aBusEntry->GetPosition().y ).c_str(),
                      FormatInternalUnits( aBusEntry->m_End().x ).c_str(),
                      FormatInternalUnits( aBusEntry->m_End().y ).c_str() );
    }
}


void SCH_SEXPR_PLUGIN::saveLine( SCH_LINE* aLine, int aNestLevel )
{
    wxCHECK_RET( aLine != nullptr && m_out != nullptr, "" );

    wxString lineType;

    switch( aLine->GetLayer() )
    {
    case LAYER_BUS:    lineType = "bus";       break;
    case LAYER_WIRE:   lineType = "wire";      break;
    case LAYER_NOTES:
    default:           lineType = "polyline";  break;
    }

    m_out->Print( aNestLevel, "(%s (pts (xy %s %s) (xy %s %s))",
                  TO_UTF8( lineType ),
                  FormatInternalUnits( aLine->GetStartPoint().x ).c_str(),
                  FormatInternalUnits( aLine->GetStartPoint().y ).c_str(),
                  FormatInternalUnits( aLine->GetEndPoint().x ).c_str(),
                  FormatInternalUnits( aLine->GetEndPoint().y ).c_str() );

    if( !aLine->UsesDefaultStroke() )
    {
        m_out->Print( 0, " " );
        formatStroke( m_out, 0, aLine->GetLineSize(), aLine->GetLineStyle(),
                      aLine->GetLineColor() );
    }

    m_out->Print( 0, ")\n" );
}


void SCH_SEXPR_PLUGIN::saveText( SCH_TEXT* aText, int aNestLevel )
{
    wxCHECK_RET( aText != nullptr && m_out != nullptr, "" );

    m_out->Print( aNestLevel, "(%s %s",
                  getTextTypeToken( aText->Type() ),
                  m_out->Quotew( aText->GetText() ).c_str() );

    if( ( aText->Type() == SCH_GLOBAL_LABEL_T ) || ( aText->Type() == SCH_HIER_LABEL_T ) )
        m_out->Print( 0, " %s", getSheetPinShapeToken( aText->GetShape() ) );

    if( aText->GetText().Length() < 50 )
    {
        m_out->Print( 0, " (at %s %s %s)",
                      FormatInternalUnits( aText->GetPosition().x ).c_str(),
                      FormatInternalUnits( aText->GetPosition().y ).c_str(),
                      FormatAngle( aText->GetTextAngle() ).c_str() );
    }
    else
    {
        m_out->Print( 0, "\n" );
        m_out->Print( aNestLevel + 1, "(at %s %s %s)",
                      FormatInternalUnits( aText->GetPosition().x ).c_str(),
                      FormatInternalUnits( aText->GetPosition().y ).c_str(),
                      FormatAngle( aText->GetTextAngle() ).c_str() );
    }

    if( !aText->IsDefaultFormatting() )
    {
        m_out->Print( 0, "\n" );
        aText->Format( m_out, aNestLevel + 1, 0 );
        m_out->Print( aNestLevel, ")\n" );   // Closes text token with font effects.
    }
    else
    {
        m_out->Print( 0, ")\n" );            // Closes text token without font effects.
    }
}


void SCH_SEXPR_PLUGIN::saveBusAlias( std::shared_ptr<BUS_ALIAS> aAlias, int aNestLevel )
{
    wxCHECK_RET( aAlias != NULL, "BUS_ALIAS* is NULL" );

    wxString members = boost::algorithm::join( aAlias->Members(), " " );
}


int SCH_SEXPR_PLUGIN_CACHE::m_modHash = 1;     // starts at 1 and goes up


SCH_SEXPR_PLUGIN_CACHE::SCH_SEXPR_PLUGIN_CACHE( const wxString& aFullPathAndFileName ) :
    m_fileName( aFullPathAndFileName ),
    m_libFileName( aFullPathAndFileName ),
    m_isWritable( true ),
    m_isModified( false )
{
    m_versionMajor = -1;
    m_versionMinor = -1;
    m_libType = LIBRARY_TYPE_EESCHEMA;
}


SCH_SEXPR_PLUGIN_CACHE::~SCH_SEXPR_PLUGIN_CACHE()
{
    // When the cache is destroyed, all of the alias objects on the heap should be deleted.
    for( LIB_PART_MAP::iterator it = m_symbols.begin();  it != m_symbols.end();  ++it )
        delete it->second;

    m_symbols.clear();
}


// If m_libFileName is a symlink follow it to the real source file
wxFileName SCH_SEXPR_PLUGIN_CACHE::GetRealFile() const
{
    wxFileName fn( m_libFileName );

#ifndef __WINDOWS__
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

    return fn;
}


wxDateTime SCH_SEXPR_PLUGIN_CACHE::GetLibModificationTime()
{
    wxFileName fn = GetRealFile();

    // update the writable flag while we have a wxFileName, in a network this
    // is possibly quite dynamic anyway.
    m_isWritable = fn.IsFileWritable();

    return fn.GetModificationTime();
}


bool SCH_SEXPR_PLUGIN_CACHE::IsFile( const wxString& aFullPathAndFileName ) const
{
    return m_fileName == aFullPathAndFileName;
}


bool SCH_SEXPR_PLUGIN_CACHE::IsFileChanged() const
{
    wxFileName fn = GetRealFile();

    if( m_fileModTime.IsValid() && fn.IsOk() && fn.FileExists() )
        return fn.GetModificationTime() != m_fileModTime;

    return false;
}


LIB_PART* SCH_SEXPR_PLUGIN_CACHE::removeSymbol( LIB_PART* aPart )
{
    wxCHECK_MSG( aPart != NULL, NULL, "NULL pointer cannot be removed from library." );

    LIB_PART* firstChild = NULL;
    LIB_PART_MAP::iterator it = m_symbols.find( aPart->GetName() );

    if( it == m_symbols.end() )
        return NULL;

    // If the entry pointer doesn't match the name it is mapped to in the library, we
    // have done something terribly wrong.
    wxCHECK_MSG( *it->second == aPart, NULL,
                 "Pointer mismatch while attempting to remove alias entry <" + aPart->GetName() +
                 "> from library cache <" + m_libFileName.GetName() + ">." );

    // If the symbol is a root symbol used by other symbols find the first alias that uses
    // the root part and make it the new root.
    if( aPart->IsRoot() )
    {
        for( auto entry : m_symbols )
        {
            if( entry.second->IsAlias()
              && entry.second->GetParent().lock() == aPart->SharedPtr() )
            {
                firstChild = entry.second;
                break;
            }
        }

        if( firstChild )
        {
            for( LIB_ITEM& drawItem : aPart->GetDrawItems() )
            {
                if( drawItem.Type() == LIB_FIELD_T )
                {
                    LIB_FIELD& field = static_cast<LIB_FIELD&>( drawItem );

                    if( firstChild->FindField( field.GetName( NATIVE_FIELD_NAME ) ) )
                        continue;
                }

                LIB_ITEM* newItem = (LIB_ITEM*) drawItem.Clone();
                drawItem.SetParent( firstChild );
                firstChild->AddDrawItem( newItem );
            }

            // Reparent the remaining aliases.
            for( auto entry : m_symbols )
            {
                if( entry.second->IsAlias()
                  && entry.second->GetParent().lock() == aPart->SharedPtr() )
                    entry.second->SetParent( firstChild );
            }
        }
    }

    m_symbols.erase( it );
    delete aPart;
    m_isModified = true;
    ++m_modHash;
    return firstChild;
}


void SCH_SEXPR_PLUGIN_CACHE::AddSymbol( const LIB_PART* aPart )
{
    // aPart is cloned in PART_LIB::AddPart().  The cache takes ownership of aPart.
    wxString name = aPart->GetName();
    LIB_PART_MAP::iterator it = m_symbols.find( name );

    if( it != m_symbols.end() )
    {
        removeSymbol( it->second );
    }

    m_symbols[ name ] = const_cast< LIB_PART* >( aPart );
    m_isModified = true;
    ++m_modHash;
}


void SCH_SEXPR_PLUGIN_CACHE::Load()
{
    if( !m_libFileName.FileExists() )
    {
        THROW_IO_ERROR( wxString::Format( _( "Library file \"%s\" not found." ),
                                          m_libFileName.GetFullPath() ) );
    }

    wxCHECK_RET( m_libFileName.IsAbsolute(),
                 wxString::Format( "Cannot use relative file paths in sexpr plugin to "
                                   "open library \"%s\".", m_libFileName.GetFullPath() ) );

    wxLogTrace( traceSchLegacyPlugin, "Loading sexpr symbol library file \"%s\"",
                m_libFileName.GetFullPath() );

    FILE_LINE_READER reader( m_libFileName.GetFullPath() );

    SCH_SEXPR_PARSER parser( &reader );

    parser.ParseLib( m_symbols );
    ++m_modHash;

    // Remember the file modification time of library file when the
    // cache snapshot was made, so that in a networked environment we will
    // reload the cache as needed.
    m_fileModTime = GetLibModificationTime();
}


LIB_PART* SCH_SEXPR_PLUGIN_CACHE::LoadPart( LINE_READER& aReader, int aMajorVersion,
                                            int aMinorVersion, LIB_PART_MAP* aMap )
{
    std::unique_ptr< LIB_PART > part( new LIB_PART( wxEmptyString ) );

    return part.release();
}



void SCH_SEXPR_PLUGIN_CACHE::Save()
{
    if( !m_isModified )
        return;

    // Write through symlinks, don't replace them.
    wxFileName fn = GetRealFile();

    std::unique_ptr< FILE_OUTPUTFORMATTER > formatter( new FILE_OUTPUTFORMATTER( fn.GetFullPath() ) );

    formatter->Print( 0, "(kicad_symbol_lib (version %d) (host kicad_symbol_editor %s)\n",
                      SEXPR_SYMBOL_LIB_FILE_VERSION,
                      formatter->Quotew( GetBuildVersion() ).c_str() );

    for( auto parent : m_symbols )
    {
        // Save the root symbol first so alias can inherit from them.
        if( parent.second->IsRoot() )
        {
            SaveSymbol( parent.second, *formatter.get(), 1 );

            // Save all of the aliases associated with the current root symbol.
            for( auto alias : m_symbols )
            {
                if( !alias.second->IsAlias() )
                    continue;

                std::shared_ptr<LIB_PART> aliasParent = alias.second->GetParent().lock();

                if( aliasParent.get() != parent.second )
                    continue;

                SaveSymbol( alias.second, *formatter.get(), 1 );
            }
        }
    }

    formatter->Print( 0, ")\n" );

    formatter.reset();

    m_fileModTime = fn.GetModificationTime();
    m_isModified = false;
}


void SCH_SEXPR_PLUGIN_CACHE::SaveSymbol( LIB_PART* aSymbol, OUTPUTFORMATTER& aFormatter,
                                         int aNestLevel, LIB_PART_MAP* aMap )
{
    wxCHECK_RET( aSymbol, "Invalid LIB_PART pointer." );

    if( aSymbol->IsRoot() )
    {
        aFormatter.Print( aNestLevel, "(symbol %s",
                          aFormatter.Quotew( aSymbol->GetName() ).c_str() );

        if( aSymbol->IsPower() )
            aFormatter.Print( 0, " power" );

        // TODO: add uuid token here.

        // TODO: add anchor position token here.

        if( !aSymbol->ShowPinNumbers() )
            aFormatter.Print( 0, " (pin_numbers hide)" );

        if( aSymbol->GetPinNameOffset() != Mils2iu( DEFAULT_PIN_NAME_OFFSET )
          || !aSymbol->ShowPinNames() )
        {
            aFormatter.Print( 0, " (pin_names" );

            if( aSymbol->GetPinNameOffset() != Mils2iu( DEFAULT_PIN_NAME_OFFSET ) )
                aFormatter.Print( 0, " (offset %s)",
                                  FormatInternalUnits( aSymbol->GetPinNameOffset() ).c_str() );

            if( !aSymbol->ShowPinNames() )
                aFormatter.Print( 0, " hide" );

            aFormatter.Print( 0, ")" );
        }

        // TODO: add atomic token here.

        // TODO: add required token here."

        aFormatter.Print( 0, "\n" );

        LIB_FIELDS fields;

        aSymbol->GetFields( fields );

        for( auto field : fields )
            saveField( &field, aFormatter, aNestLevel + 1 );

        int lastFieldId = fields.back().GetId() + 1;

        // @todo At some point in the future the lock status (all units interchangeable) should
        // be set deterministically.  For now a custom lock properter is used to preserve the
        // locked flag state.
        if( aSymbol->UnitsLocked() )
        {
            LIB_FIELD locked( lastFieldId, "ki_locked" );
            saveField( &locked, aFormatter, aNestLevel + 1 );
            lastFieldId += 1;
        }

        saveDcmInfoAsFields( aSymbol, aFormatter, aNestLevel, fields.back().GetId() + 1 );

        // Save the draw items grouped by units.
        std::vector<PART_UNITS> units = aSymbol->GetUnitDrawItems();

        for( auto unit : units )
        {
            aFormatter.Print( aNestLevel + 1, "(symbol \"%s_%d_%d\"\n",
                              TO_UTF8( aSymbol->GetName() ),
                              unit.m_unit, unit.m_convert );

            for( auto item : unit.m_items )
                saveSymbolDrawItem( item, aFormatter, aNestLevel + 2 );

            aFormatter.Print( aNestLevel + 1, ")\n" );
        }
    }
    else
    {
        std::shared_ptr<LIB_PART> parent = aSymbol->GetParent().lock();

        wxASSERT( parent );

        aFormatter.Print( aNestLevel, "(symbol %s (extends %s)\n",
                          aFormatter.Quotew( aSymbol->GetName() ).c_str(),
                          aFormatter.Quotew( parent->GetName() ).c_str() );

        LIB_FIELD tmp = parent->GetValueField();
        tmp.SetText( aSymbol->GetName() );
        saveField( &tmp, aFormatter, aNestLevel + 1 );

        if( !aSymbol->GetDocFileName().IsEmpty() )
        {
            tmp = *aSymbol->GetField( DATASHEET );
            tmp.SetText( aSymbol->GetDocFileName() );
            saveField( &tmp, aFormatter, aNestLevel + 1 );
        }

        saveDcmInfoAsFields( aSymbol, aFormatter, aNestLevel, MANDATORY_FIELDS );
    }

    aFormatter.Print( aNestLevel, ")\n" );
}


void SCH_SEXPR_PLUGIN_CACHE::saveDcmInfoAsFields( LIB_PART* aSymbol, OUTPUTFORMATTER& aFormatter,
                                                  int aNestLevel, int aFirstId )
{
    wxCHECK_RET( aSymbol, "Invalid LIB_PART pointer." );

    int id = aFirstId;

    if( !aSymbol->GetKeyWords().IsEmpty() )
    {
        LIB_FIELD keywords( id, wxString( "ki_keywords" ) );
        keywords.SetVisible( false );
        keywords.SetText( aSymbol->GetKeyWords() );
        saveField( &keywords, aFormatter, aNestLevel + 1 );
        id += 1;
    }

    if( !aSymbol->GetDescription().IsEmpty() )
    {
        LIB_FIELD description( id, wxString( "ki_description" ) );
        description.SetVisible( false );
        description.SetText( aSymbol->GetDescription() );
        saveField( &description, aFormatter, aNestLevel + 1 );
        id += 1;
    }

    wxArrayString fpFilters = aSymbol->GetFootprints();

    if( !fpFilters.IsEmpty() )
    {
        wxString tmp;

        for( auto filter : fpFilters )
        {
            if( tmp.IsEmpty() )
                tmp = filter;
            else
                tmp += "\n" + filter;
        }

        LIB_FIELD description( id, wxString( "ki_fp_filters" ) );
        description.SetVisible( false );
        description.SetText( tmp );
        saveField( &description, aFormatter, aNestLevel + 1 );
        id += 1;
    }
}


void SCH_SEXPR_PLUGIN_CACHE::saveSymbolDrawItem( LIB_ITEM* aItem, OUTPUTFORMATTER& aFormatter,
                                                 int aNestLevel )
{
    wxCHECK_RET( aItem, "Invalid LIB_ITEM pointer." );

    switch( aItem->Type() )
    {
    case LIB_ARC_T:
        saveArc( (LIB_ARC*) aItem, aFormatter, aNestLevel );
        break;

    case LIB_BEZIER_T:
        saveBezier( (LIB_BEZIER*) aItem, aFormatter, aNestLevel );
        break;

    case LIB_CIRCLE_T:
        saveCircle( ( LIB_CIRCLE* ) aItem, aFormatter, aNestLevel );
        break;

    case LIB_PIN_T:
        savePin( (LIB_PIN* ) aItem, aFormatter, aNestLevel );
        break;

    case LIB_POLYLINE_T:
        savePolyLine( ( LIB_POLYLINE* ) aItem, aFormatter, aNestLevel );
        break;

    case LIB_RECTANGLE_T:
        saveRectangle( ( LIB_RECTANGLE* ) aItem, aFormatter, aNestLevel );
        break;

    case LIB_TEXT_T:
        saveText( ( LIB_TEXT* ) aItem, aFormatter, aNestLevel );
        break;

    default:
        ;
    }
}


void SCH_SEXPR_PLUGIN_CACHE::saveArc( LIB_ARC* aArc,
                                      OUTPUTFORMATTER& aFormatter,
                                      int aNestLevel )
{
    wxCHECK_RET( aArc && aArc->Type() == LIB_ARC_T, "Invalid LIB_ARC object." );

    int x1 = aArc->GetFirstRadiusAngle();

    if( x1 > 1800 )
        x1 -= 3600;

    int x2 = aArc->GetSecondRadiusAngle();

    if( x2 > 1800 )
        x2 -= 3600;

    aFormatter.Print( aNestLevel,
                      "(arc (start %s %s) (end %s %s) (radius (at %s %s) (length %s) "
                      "(angles %g %g))",
                      FormatInternalUnits( aArc->GetStart().x ).c_str(),
                      FormatInternalUnits( aArc->GetStart().y ).c_str(),
                      FormatInternalUnits( aArc->GetEnd().x ).c_str(),
                      FormatInternalUnits( aArc->GetEnd().y ).c_str(),
                      FormatInternalUnits( aArc->GetPosition().x ).c_str(),
                      FormatInternalUnits( aArc->GetPosition().y ).c_str(),
                      FormatInternalUnits( aArc->GetRadius() ).c_str(),
                      static_cast<double>( x1 ) / 10.0,
                      static_cast<double>( x2 ) / 10.0 );

    bool needsSpace = false;
    bool onNewLine = false;

    if( Iu2Mils( aArc->GetWidth() ) != DEFAULTDRAWLINETHICKNESS
      && aArc->GetWidth() != 0 )
    {
        aFormatter.Print( 0, "\n" );
        aFormatter.Print( aNestLevel + 1, "(stroke (width %s))",
                          FormatInternalUnits( aArc->GetWidth() ).c_str() );
        needsSpace = true;
        onNewLine = true;
    }

    if( aArc->GetFillMode() != NO_FILL )
    {
        if( !onNewLine || needsSpace )
            aFormatter.Print( 0, " " );

        formatFill( static_cast< LIB_ITEM* >( aArc ), aFormatter, 0 );
    }

    if( onNewLine )
    {
        aFormatter.Print( 0, "\n" );
        aFormatter.Print( aNestLevel, ")\n" );
    }
    else
    {
        aFormatter.Print( 0, ")\n" );
    }
}


void SCH_SEXPR_PLUGIN_CACHE::saveBezier( LIB_BEZIER* aBezier,
                                         OUTPUTFORMATTER& aFormatter,
                                         int aNestLevel )
{
    wxCHECK_RET( aBezier && aBezier->Type() == LIB_BEZIER_T, "Invalid LIB_BEZIER object." );

    int newLine = 0;
    int lineCount = 1;
    aFormatter.Print( aNestLevel, "(bezier\n" );
    aFormatter.Print( aNestLevel + 1, "(pts " );

    for( const auto& pt : aBezier->GetPoints() )
    {
        if( newLine == 4 )
        {
            aFormatter.Print( 0, "\n" );
            aFormatter.Print( aNestLevel + 3, " (xy %s %s)",
                              FormatInternalUnits( pt.x ).c_str(),
                              FormatInternalUnits( pt.y ).c_str() );
            newLine = 0;
            lineCount += 1;
        }
        else
        {
            aFormatter.Print( 0, " (xy %s %s)",
                              FormatInternalUnits( pt.x ).c_str(),
                              FormatInternalUnits( pt.y ).c_str() );
        }

        newLine += 1;
    }

    if( lineCount == 1 )
    {
        aFormatter.Print( 0, ")\n" );  // Closes pts token on same line.
    }
    else
    {
        aFormatter.Print( 0, "\n" );
        aFormatter.Print( aNestLevel + 1, ")\n" );  // Closes pts token with multiple lines.
    }

    bool needsSpace = false;

    if( Iu2Mils( aBezier->GetWidth() ) != DEFAULTDRAWLINETHICKNESS
      && aBezier->GetWidth() != 0 )
    {
        aFormatter.Print( aNestLevel + 1, "(stroke (width %s))",
                          FormatInternalUnits( aBezier->GetWidth() ).c_str() );
        needsSpace = true;

        if( aBezier->GetFillMode() == NO_FILL )
            aFormatter.Print( 0, "\n" );
    }

    if( aBezier->GetFillMode() != NO_FILL )
    {
        if( needsSpace )
        {
            aFormatter.Print( 0, " " );
            formatFill( static_cast< LIB_ITEM* >( aBezier ), aFormatter, 0 );
        }
        else
        {
            formatFill( static_cast< LIB_ITEM* >( aBezier ), aFormatter, aNestLevel + 1 );
        }

        aFormatter.Print( 0, "\n" );
    }

    aFormatter.Print( aNestLevel, ")\n" );
}


void SCH_SEXPR_PLUGIN_CACHE::saveCircle( LIB_CIRCLE* aCircle,
                                         OUTPUTFORMATTER& aFormatter,
                                         int aNestLevel )
{
    wxCHECK_RET( aCircle && aCircle->Type() == LIB_CIRCLE_T, "Invalid LIB_CIRCLE object." );

    aFormatter.Print( aNestLevel, "(circle (center %s %s) (radius %s)",
                      FormatInternalUnits( aCircle->GetPosition().x ).c_str(),
                      FormatInternalUnits( aCircle->GetPosition().y ).c_str(),
                      FormatInternalUnits( aCircle->GetRadius() ).c_str() );

    if( Iu2Mils( aCircle->GetWidth() ) != DEFAULTDRAWLINETHICKNESS
      && aCircle->GetWidth() != 0 )
    {
        aFormatter.Print( 0, " (stroke (width %s))",
                          FormatInternalUnits( aCircle->GetWidth() ).c_str() );
    }

    if( aCircle->GetFillMode() != NO_FILL )
    {
        aFormatter.Print( 0, " " );
        formatFill( static_cast< LIB_ITEM* >( aCircle ), aFormatter, 0 );
    }

    aFormatter.Print( 0, ")\n" );
}


void SCH_SEXPR_PLUGIN_CACHE::saveField( LIB_FIELD* aField,
                                        OUTPUTFORMATTER& aFormatter,
                                        int aNestLevel )
{
    wxCHECK_RET( aField && aField->Type() == LIB_FIELD_T, "Invalid LIB_FIELD object." );

    wxString fieldName = aField->GetName( NATIVE_FIELD_NAME );

    // When saving legacy fields, prefix the field name with "ki_" to prevent name clashes
    // with exisiting user defined fields.
    if( aField->IsMandatory() && !fieldName.StartsWith( "ki_" ) )
        fieldName = "ki_" + fieldName.Lower();

    aFormatter.Print( aNestLevel, "(property %s %s (at %s %s %g)",
                      aFormatter.Quotew( fieldName ).c_str(),
                      aFormatter.Quotew( aField->GetText() ).c_str(),
                      FormatInternalUnits( aField->GetPosition().x ).c_str(),
                      FormatInternalUnits( aField->GetPosition().y ).c_str(),
                      static_cast<double>( aField->GetTextAngle() ) / 10.0 );

    if( aField->IsDefaultFormatting() )
    {
        aFormatter.Print( 0, ")\n" );           // Close property token if no font effects.
    }
    else
    {
        aFormatter.Print( 0, "\n" );
        aField->Format( &aFormatter, aNestLevel, 0 );
        aFormatter.Print( aNestLevel, ")\n" );  // Close property token.
    }
}


void SCH_SEXPR_PLUGIN_CACHE::savePin( LIB_PIN* aPin,
                                      OUTPUTFORMATTER& aFormatter,
                                      int aNestLevel )
{
    wxCHECK_RET( aPin && aPin->Type() == LIB_PIN_T, "Invalid LIB_PIN object." );

    aPin->ClearFlags( IS_CHANGED );

    aFormatter.Print( aNestLevel, "(pin %s %s (at %s %s %s) (length %s)",
                      getPinElectricalTypeToken( aPin->GetType() ),
                      getPinShapeToken( aPin->GetShape() ),
                      FormatInternalUnits( aPin->GetPosition().x ).c_str(),
                      FormatInternalUnits( aPin->GetPosition().y ).c_str(),
                      FormatAngle( getPinAngle( aPin->GetOrientation() ) * 10.0 ).c_str(),
                      FormatInternalUnits( aPin->GetLength() ).c_str() );

    aFormatter.Print( 0, " (name %s",
                      aFormatter.Quotew( aPin->GetName() ).c_str() );

    // This follows the EDA_TEXT effects formatting for future expansion.
    if( aPin->GetNameTextSize() != Mils2iu( DEFAULTPINNAMESIZE ) )
        aFormatter.Print( 0, " (effects (font (size %s %s)))",
                          FormatInternalUnits( aPin->GetNameTextSize() ).c_str(),
                          FormatInternalUnits( aPin->GetNameTextSize() ).c_str() );

    aFormatter.Print( 0, ")" );
    aFormatter.Print( 0, " (number %s",
                      aFormatter.Quotew( aPin->GetNumber() ).c_str() );

    // This follows the EDA_TEXT effects formatting for future expansion.
    if( aPin->GetNumberTextSize() != Mils2iu( DEFAULTPINNUMSIZE ) )
        aFormatter.Print( 0, " (effects (font (size %s %s)))",
                          FormatInternalUnits( aPin->GetNumberTextSize() ).c_str(),
                          FormatInternalUnits( aPin->GetNumberTextSize() ).c_str() );
    aFormatter.Print( 0, ")" );

    if( !aPin->IsVisible() )
        aFormatter.Print( 0, " hide" );

    aFormatter.Print( 0, ")\n" );
}


void SCH_SEXPR_PLUGIN_CACHE::savePolyLine( LIB_POLYLINE* aPolyLine,
                                           OUTPUTFORMATTER& aFormatter,
                                           int aNestLevel )
{
    wxCHECK_RET( aPolyLine && aPolyLine->Type() == LIB_POLYLINE_T, "Invalid LIB_POLYLINE object." );

    int newLine = 0;
    int lineCount = 1;
    aFormatter.Print( aNestLevel, "(polyline\n" );
    aFormatter.Print( aNestLevel + 1, "(pts" );

    for( const auto& pt : aPolyLine->GetPolyPoints() )
    {
        if( newLine == 4 )
        {
            aFormatter.Print( 0, "\n" );
            aFormatter.Print( aNestLevel + 3, " (xy %s %s)",
                              FormatInternalUnits( pt.x ).c_str(),
                              FormatInternalUnits( pt.y ).c_str() );
            newLine = 0;
            lineCount += 1;
        }
        else
        {
            aFormatter.Print( 0, " (xy %s %s)",
                              FormatInternalUnits( pt.x ).c_str(),
                              FormatInternalUnits( pt.y ).c_str() );
        }

        newLine += 1;
    }

    if( lineCount == 1 )
    {
        aFormatter.Print( 0, ")\n" );  // Closes pts token on same line.
    }
    else
    {
        aFormatter.Print( 0, "\n" );
        aFormatter.Print( aNestLevel + 1, ")\n" );  // Closes pts token with multiple lines.
    }

    bool needsSpace = false;

    if( Iu2Mils( aPolyLine->GetWidth() ) != DEFAULTDRAWLINETHICKNESS
      && aPolyLine->GetWidth() != 0 )
    {
        aFormatter.Print( aNestLevel + 1, "(stroke (width %s))",
                          FormatInternalUnits( aPolyLine->GetWidth() ).c_str() );
        needsSpace = true;

        if( aPolyLine->GetFillMode() == NO_FILL )
            aFormatter.Print( 0, "\n" );
    }

    if( aPolyLine->GetFillMode() != NO_FILL )
    {
        if( needsSpace )
        {
            aFormatter.Print( 0, " " );
            formatFill( static_cast< LIB_ITEM* >( aPolyLine ), aFormatter, 0 );
        }
        else
        {
            formatFill( static_cast< LIB_ITEM* >( aPolyLine ), aFormatter, aNestLevel + 1 );
        }

        aFormatter.Print( 0, "\n" );
    }

    aFormatter.Print( aNestLevel, ")\n" );
}


void SCH_SEXPR_PLUGIN_CACHE::saveRectangle( LIB_RECTANGLE* aRectangle,
                                            OUTPUTFORMATTER& aFormatter,
                                            int aNestLevel )
{
    wxCHECK_RET( aRectangle && aRectangle->Type() == LIB_RECTANGLE_T,
                 "Invalid LIB_RECTANGLE object." );

    aFormatter.Print( aNestLevel, "(rectangle (start %s %s) (end %s %s)",
                      FormatInternalUnits( aRectangle->GetPosition().x ).c_str(),
                      FormatInternalUnits( aRectangle->GetPosition().y ).c_str(),
                      FormatInternalUnits( aRectangle->GetEnd().x ).c_str(),
                      FormatInternalUnits( aRectangle->GetEnd().y ).c_str() );

    bool needsSpace = false;

    if( Iu2Mils( aRectangle->GetWidth() ) != DEFAULTDRAWLINETHICKNESS
      && aRectangle->GetWidth() != 0 )
    {
        aFormatter.Print( 0, " (stroke (width %s))",
                          FormatInternalUnits( aRectangle->GetWidth() ).c_str() );
        needsSpace = true;
    }

    if( aRectangle->GetFillMode() != NO_FILL )
    {
        if( needsSpace )
            aFormatter.Print( 0, " " );

        formatFill( static_cast< LIB_ITEM* >( aRectangle ), aFormatter, 0 );
    }

    aFormatter.Print( 0, ")\n" );
}


void SCH_SEXPR_PLUGIN_CACHE::saveText( LIB_TEXT* aText,
                                       OUTPUTFORMATTER& aFormatter,
                                       int aNestLevel )
{
    wxCHECK_RET( aText && aText->Type() == LIB_TEXT_T, "Invalid LIB_TEXT object." );

    aFormatter.Print( aNestLevel, "(text %s (at %s %s %g)\n",
                      aFormatter.Quotew( aText->GetText() ).c_str(),
                      FormatInternalUnits( aText->GetPosition().x ).c_str(),
                      FormatInternalUnits( aText->GetPosition().y ).c_str(),
                      aText->GetTextAngle() );
    aText->Format( &aFormatter, aNestLevel, 0 );
    aFormatter.Print( aNestLevel, ")\n" );
}


void SCH_SEXPR_PLUGIN_CACHE::DeleteSymbol( const wxString& aSymbolName )
{
    LIB_PART_MAP::iterator it = m_symbols.find( aSymbolName );

    if( it == m_symbols.end() )
        THROW_IO_ERROR( wxString::Format( _( "library %s does not contain a symbol named %s" ),
                                          m_libFileName.GetFullName(), aSymbolName ) );

    LIB_PART* part = it->second;

    if( part->IsRoot() )
    {
        LIB_PART* rootPart = part;

        // Remove the root symbol and all it's children.
        m_symbols.erase( it );

        LIB_PART_MAP::iterator it1 = m_symbols.begin();

        while( it1 != m_symbols.end() )
        {
            if( it1->second->IsAlias() && it1->second->GetParent().lock() == rootPart->SharedPtr() )
            {
                delete it1->second;
                it1 = m_symbols.erase( it1 );
            }
            else
            {
                it1++;
            }
        }

        delete rootPart;
    }
    else
    {
        // Just remove the alias.
        m_symbols.erase( it );
        delete part;
    }

    ++m_modHash;
    m_isModified = true;
}


void SCH_SEXPR_PLUGIN::cacheLib( const wxString& aLibraryFileName )
{
    if( !m_cache || !m_cache->IsFile( aLibraryFileName ) || m_cache->IsFileChanged() )
    {
        // a spectacular episode in memory management:
        delete m_cache;
        m_cache = new SCH_SEXPR_PLUGIN_CACHE( aLibraryFileName );

        // Because m_cache is rebuilt, increment PART_LIBS::s_modify_generation
        // to modify the hash value that indicate component to symbol links
        // must be updated.
        PART_LIBS::s_modify_generation++;

        if( !isBuffering( m_props ) )
            m_cache->Load();
    }
}


bool SCH_SEXPR_PLUGIN::isBuffering( const PROPERTIES* aProperties )
{
    return ( aProperties && aProperties->Exists( SCH_SEXPR_PLUGIN::PropBuffering ) );
}


int SCH_SEXPR_PLUGIN::GetModifyHash() const
{
    if( m_cache )
        return m_cache->GetModifyHash();

    // If the cache hasn't been loaded, it hasn't been modified.
    return 0;
}


void SCH_SEXPR_PLUGIN::EnumerateSymbolLib( wxArrayString&    aSymbolNameList,
                                           const wxString&   aLibraryPath,
                                           const PROPERTIES* aProperties )
{
    LOCALE_IO   toggle;     // toggles on, then off, the C locale.

    m_props = aProperties;

    bool powerSymbolsOnly = ( aProperties &&
                              aProperties->find( SYMBOL_LIB_TABLE::PropPowerSymsOnly ) != aProperties->end() );
    cacheLib( aLibraryPath );

    const LIB_PART_MAP& symbols = m_cache->m_symbols;

    for( LIB_PART_MAP::const_iterator it = symbols.begin();  it != symbols.end();  ++it )
    {
        if( !powerSymbolsOnly || it->second->IsPower() )
            aSymbolNameList.Add( it->first );
    }
}


void SCH_SEXPR_PLUGIN::EnumerateSymbolLib( std::vector<LIB_PART*>& aSymbolList,
                                           const wxString&   aLibraryPath,
                                           const PROPERTIES* aProperties )
{
    LOCALE_IO   toggle;     // toggles on, then off, the C locale.

    m_props = aProperties;

    bool powerSymbolsOnly = ( aProperties &&
                              aProperties->find( SYMBOL_LIB_TABLE::PropPowerSymsOnly ) != aProperties->end() );
    cacheLib( aLibraryPath );

    const LIB_PART_MAP& symbols = m_cache->m_symbols;

    for( LIB_PART_MAP::const_iterator it = symbols.begin();  it != symbols.end();  ++it )
    {
        if( !powerSymbolsOnly || it->second->IsPower() )
            aSymbolList.push_back( it->second );
    }
}


LIB_PART* SCH_SEXPR_PLUGIN::LoadSymbol( const wxString& aLibraryPath, const wxString& aSymbolName,
                                        const PROPERTIES* aProperties )
{
    LOCALE_IO toggle;     // toggles on, then off, the C locale.

    m_props = aProperties;

    cacheLib( aLibraryPath );

    LIB_PART_MAP::const_iterator it = m_cache->m_symbols.find( aSymbolName );

    if( it == m_cache->m_symbols.end() )
        return nullptr;

    return it->second;
}


void SCH_SEXPR_PLUGIN::SaveSymbol( const wxString& aLibraryPath, const LIB_PART* aSymbol,
                                   const PROPERTIES* aProperties )
{
    m_props = aProperties;

    cacheLib( aLibraryPath );

    m_cache->AddSymbol( aSymbol );

    if( !isBuffering( aProperties ) )
        m_cache->Save();
}


void SCH_SEXPR_PLUGIN::DeleteSymbol( const wxString& aLibraryPath, const wxString& aSymbolName,
                                     const PROPERTIES* aProperties )
{
    m_props = aProperties;

    cacheLib( aLibraryPath );

    m_cache->DeleteSymbol( aSymbolName );

    if( !isBuffering( aProperties ) )
        m_cache->Save();
}


void SCH_SEXPR_PLUGIN::CreateSymbolLib( const wxString& aLibraryPath,
                                        const PROPERTIES* aProperties )
{
    if( wxFileExists( aLibraryPath ) )
    {
        THROW_IO_ERROR( wxString::Format(
            _( "symbol library \"%s\" already exists, cannot create a new library" ),
            aLibraryPath.GetData() ) );
    }

    LOCALE_IO toggle;

    m_props = aProperties;

    delete m_cache;
    m_cache = new SCH_SEXPR_PLUGIN_CACHE( aLibraryPath );
    m_cache->SetModified();
    m_cache->Save();
    m_cache->Load();    // update m_writable and m_mod_time
}


bool SCH_SEXPR_PLUGIN::DeleteSymbolLib( const wxString& aLibraryPath,
                                        const PROPERTIES* aProperties )
{
    wxFileName fn = aLibraryPath;

    if( !fn.FileExists() )
        return false;

    // Some of the more elaborate wxRemoveFile() crap puts up its own wxLog dialog
    // we don't want that.  we want bare metal portability with no UI here.
    if( wxRemove( aLibraryPath ) )
    {
        THROW_IO_ERROR( wxString::Format( _( "library \"%s\" cannot be deleted" ),
                                          aLibraryPath.GetData() ) );
    }

    if( m_cache && m_cache->IsFile( aLibraryPath ) )
    {
        delete m_cache;
        m_cache = 0;
    }

    return true;
}


void SCH_SEXPR_PLUGIN::SaveLibrary( const wxString& aLibraryPath, const PROPERTIES* aProperties )
{
    if( !m_cache )
        m_cache = new SCH_SEXPR_PLUGIN_CACHE( aLibraryPath );

    wxString oldFileName = m_cache->GetFileName();

    if( !m_cache->IsFile( aLibraryPath ) )
    {
        m_cache->SetFileName( aLibraryPath );
    }

    // This is a forced save.
    m_cache->SetModified();
    m_cache->Save();
    m_cache->SetFileName( oldFileName );
}


bool SCH_SEXPR_PLUGIN::CheckHeader( const wxString& aFileName )
{
    // Open file and check first line
    wxTextFile tempFile;

    tempFile.Open( aFileName );
    wxString firstline;
    // read the first line
    firstline = tempFile.GetFirstLine();
    tempFile.Close();

    return firstline.StartsWith( "EESchema" );
}


bool SCH_SEXPR_PLUGIN::IsSymbolLibWritable( const wxString& aLibraryPath )
{
    return wxFileName::IsFileWritable( aLibraryPath );
}


LIB_PART* SCH_SEXPR_PLUGIN::ParsePart( LINE_READER& reader, int aMajorVersion,
                                       int aMinorVersion )
{
    return SCH_SEXPR_PLUGIN_CACHE::LoadPart( reader, aMajorVersion, aMinorVersion );
}


void SCH_SEXPR_PLUGIN::FormatPart( LIB_PART* part, OUTPUTFORMATTER & formatter )
{
    SCH_SEXPR_PLUGIN_CACHE::SaveSymbol( part, formatter );
}


const char* SCH_SEXPR_PLUGIN::PropBuffering = "buffering";

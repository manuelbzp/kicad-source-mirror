/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2020 Jon Evans <jon@craftyjon.com>
 * Copyright (C) 2020 KiCad Developers, see AUTHORS.txt for contributors.
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

#ifndef _APP_SETTINGS_H
#define _APP_SETTINGS_H

#include <gal/color4d.h>
#include <settings/json_settings.h>

/**
 * Common cursor settings, available to every frame
 */
struct CURSOR_SETTINGS
{
    bool always_show_cursor;
    bool fullscreen_cursor;
};

/**
 * Common grid settings, available to every frame
 */
struct GRID_SETTINGS
{
    bool axes_enabled;
    int last_size;
    double line_width;
    double min_spacing;
    bool show;
    int style;
};

/**
 * Stores the common settings that are saved and loaded for each window / frame
 */
struct WINDOW_SETTINGS
{
    bool maximized;
    wxString mru_path;
    int size_x;
    int size_y;
    wxString perspective;
    int pos_x;
    int pos_y;

    CURSOR_SETTINGS cursor;
    GRID_SETTINGS grid;
    bool auto_zoom;
    double zoom;
};

/**
 * APP_SETTINGS_BASE is a settings class that should be derived for each standalone KiCad
 * application.  It stores settings that should exist for every app, but may be different from
 * app to app depending on the user's preferences.
 *
 * COMMON_SETTINGS stores settings that are always the same across all applications.
 */
class APP_SETTINGS_BASE : public JSON_SETTINGS
{
public:


    struct FIND_REPLACE
    {
        int                   flags;
        wxString              find_string;
        std::vector<wxString> find_history;
        wxString              replace_string;
        std::vector<wxString> replace_history;
    };

    struct GRAPHICS
    {
        int canvas_type;
    };

    struct LIB_TREE
    {
        int column_width;
    };

    struct PRINTING
    {
        bool   monochrome;
        double scale;
        bool   title_block;
        std::vector<int> layers;      ///< List of enabled layers for printing
    };

    struct SYSTEM
    {
        bool                  first_run_shown;
        int                   max_undo_items;
        std::vector<wxString> file_history;
        int                   units;
    };

    APP_SETTINGS_BASE( std::string aFilename, int aSchemaVersion );

    virtual ~APP_SETTINGS_BASE() {}

    virtual bool MigrateFromLegacy( wxConfigBase* aCfg ) override;

public:
    FIND_REPLACE m_FindReplace;

    GRAPHICS m_Graphics;

    LIB_TREE m_LibTree;

    PRINTING m_Printing;

    SYSTEM m_System;

    WINDOW_SETTINGS m_Window;

    /// Active color theme name
    wxString m_ColorTheme;

    ///! Local schema version for common app settings
    int m_appSettingsSchemaVersion;

protected:

    virtual std::string getLegacyFrameName() const { return std::string(); }

    ///! Migrates the find/replace history string lists
    void migrateFindReplace( wxConfigBase* aCfg );

    /**
     * Migrates legacy window settings into the JSON document
     * @param aCfg is the wxConfig object to read from
     * @param aFrameName is the prefix for window settings in the legacy config file
     * @param aJsonPath is the prefix for storing window settings in the JSON file
     * @return true if all settings were migrated
     */
    bool migrateWindowConfig( wxConfigBase* aCfg, const std::string& aFrameName,
            const std::string& aJsonPath );

    /**
     * Adds parameters for the given window object
     * @param aWindow is the target window settings object
     * @param aJsonPath is the path to read parameters from
     */
    void addParamsForWindow( WINDOW_SETTINGS* aWindow, const std::string& aJsonPath );
};

#endif

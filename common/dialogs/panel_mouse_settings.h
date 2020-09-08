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

#ifndef KICAD_PANEL_MOUSE_SETTINGS_H
#define KICAD_PANEL_MOUSE_SETTINGS_H

#include <dialogs/panel_mouse_settings_base.h>


class COMMON_SETTINGS;
class DIALOG_SHIM;


struct SCROLL_MOD_SET
{
    int zoom;
    int panh;
    int panv;
};


class PANEL_MOUSE_SETTINGS : public PANEL_MOUSE_SETTINGS_BASE
{
public:
    PANEL_MOUSE_SETTINGS( DIALOG_SHIM* aDialog, wxWindow* aParent );

    ~PANEL_MOUSE_SETTINGS();

    void ResetPanel() override;

protected:
    bool TransferDataFromWindow() override;

    bool TransferDataToWindow() override;

    void OnScrollRadioButton( wxCommandEvent& event ) override;

    DIALOG_SHIM*  m_dialog;

private:
    void applySettingsToPanel( const COMMON_SETTINGS& aSettings );

    SCROLL_MOD_SET getScrollModSet();

    void updateScrollModButtons();

    bool isScrollModSetValid( const SCROLL_MOD_SET& aSet );

    SCROLL_MOD_SET m_currentScrollMod;
};


#endif
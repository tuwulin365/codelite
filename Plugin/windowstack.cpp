//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//
// copyright            : (C) 2008 by Eran Ifrah
// file name            : windowstack.cpp
//
// -------------------------------------------------------------------------
// A
//              _____           _      _     _ _
//             /  __ \         | |    | |   (_) |
//             | /  \/ ___   __| | ___| |    _| |_ ___
//             | |    / _ \ / _  |/ _ \ |   | | __/ _ )
//             | \__/\ (_) | (_| |  __/ |___| | ||  __/
//              \____/\___/ \__,_|\___\_____/_|\__\___|
//
//                                                  F i l e
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
#include "windowstack.h"

#include "clSystemSettings.h"
#include "drawingutils.h"
#include "event_notifier.h"

#include <algorithm>
#include <wx/dcbuffer.h>
#include <wx/wupdlock.h>

#if !wxUSE_WINDOWSTACK_SIMPLEBOOK
WindowStack::WindowStack(wxWindow* parent, wxWindowID id, bool useNativeThemeColours)
    : wxWindow(parent, id)
{
    Bind(wxEVT_SIZE, &WindowStack::OnSize, this);
    SetBackgroundColour(clSystemSettings::GetDefaultPanelColour());
    EventNotifier::Get()->Bind(wxEVT_SYS_COLOURS_CHANGED, &WindowStack::OnColoursChanged, this);
}

WindowStack::~WindowStack()
{
    Unbind(wxEVT_SIZE, &WindowStack::OnSize, this);
    EventNotifier::Get()->Unbind(wxEVT_SYS_COLOURS_CHANGED, &WindowStack::OnColoursChanged, this);
}

void WindowStack::Select(wxWindow* win)
{
    //#ifndef __WXOSX__
    //    wxWindowUpdateLocker locker(this);
    //#endif
    int index = FindPage(win);
    if(index == wxNOT_FOUND) {
        return;
    }
    ChangeSelection(index);
}

void WindowStack::Clear()
{
    for (wxWindow* w : m_windows) {
        w->Hide();
        w->Destroy();
    }
    m_windows.clear();
    m_activeWin = nullptr;
}

bool WindowStack::Remove(wxWindow* win)
{
    int index = FindPage(win);
    if(index == wxNOT_FOUND) {
        return false;
    }
    m_windows.erase(m_windows.begin() + index);
    if(win == m_activeWin) {
        m_activeWin = nullptr;
    }
    return true;
}

bool WindowStack::Add(wxWindow* win, bool select)
{
    if(!win || Contains(win)) {
        return false;
    }
    win->Reparent(this);
    m_windows.push_back(win);
    if(select) {
        DoSelect(win);
    } else {
        win->Hide();
    }
    return true;
}

bool WindowStack::Contains(wxWindow* win) { return FindPage(win) != wxNOT_FOUND; }

int WindowStack::FindPage(wxWindow* page) const
{
    for(size_t i = 0; i < m_windows.size(); ++i) {
        if(m_windows[i] == page) {
            return i;
        }
    }
    return wxNOT_FOUND;
}

wxWindow* WindowStack::GetSelected() const { return m_activeWin; }

int WindowStack::ChangeSelection(size_t index)
{
    if(index >= m_windows.size()) {
        return wxNOT_FOUND;
    }
    return DoSelect(m_windows[index]);
}

int WindowStack::DoSelect(wxWindow* win)
{
    if(!win) {
        return wxNOT_FOUND;
    }
    // Firsr, show the window
    win->SetSize(wxRect(0, 0, GetSize().x, GetSize().y));
    win->Show();
    int oldSel = FindPage(win);
    m_activeWin = win;
    // Hide the rest
    CallAfter(&WindowStack::DoHideNoActiveWindows);
    return oldSel;
}

void WindowStack::OnSize(wxSizeEvent& e)
{
    e.Skip();
    if(!m_activeWin) {
        return;
    }
    m_activeWin->SetSize(wxRect(0, 0, GetSize().x, GetSize().y));
}

void WindowStack::DoHideNoActiveWindows()
{
    for(auto w : m_windows) {
        if(w != m_activeWin) {
            w->Hide();
        }
    }

#ifdef __WXOSX__
    if(m_activeWin) {
        m_activeWin->Refresh();
    }
#endif
}

void WindowStack::OnColoursChanged(clCommandEvent& event)
{
    event.Skip();
    SetBackgroundColour(clSystemSettings::GetDefaultPanelColour());
}
#else
WindowStack::WindowStack(wxWindow* parent, wxWindowID id, bool useNativeThemeColours)
    : wxSimplebook(parent, id)
{
    SetBackgroundColour(clSystemSettings::GetDefaultPanelColour());
    EventNotifier::Get()->Bind(wxEVT_SYS_COLOURS_CHANGED, &WindowStack::OnColoursChanged, this);
    wxUnusedVar(useNativeThemeColours);
}

WindowStack::~WindowStack()
{
    EventNotifier::Get()->Unbind(wxEVT_SYS_COLOURS_CHANGED, &WindowStack::OnColoursChanged, this);
}

bool WindowStack::Add(wxWindow* win, bool select)
{
    if(!win || Contains(win)) {
        return false;
    }
    win->Reparent(this);
    return AddPage(win, wxEmptyString, select);
}

void WindowStack::Select(wxWindow* win)
{
    wxWindowUpdateLocker locker(this);
    for(size_t i = 0; i < GetPageCount(); ++i) {
        if(GetPage(i) == win) {
            ChangeSelection(i);
        }
    }
}

int WindowStack::FindPage(wxWindow* win) const
{
    for(size_t i = 0; i < GetPageCount(); ++i) {
        if(GetPage(i) == win) {
            return i;
        }
    }
    return wxNOT_FOUND;
}

void WindowStack::Clear() { DeleteAllPages(); }

bool WindowStack::Remove(wxWindow* win)
{
    int index = FindPage(win);
    if(index == wxNOT_FOUND) {
        return false;
    }
    return RemovePage(index);
}

bool WindowStack::Contains(wxWindow* win) { return FindPage(win) != wxNOT_FOUND; }
wxWindow* WindowStack::GetSelected() const
{
    if(GetSelection() == wxNOT_FOUND) {
        return nullptr;
    }
    return GetPage(GetSelection());
}

void WindowStack::OnColoursChanged(clCommandEvent& e)
{
    e.Skip();
    SetBackgroundColour(clSystemSettings::GetDefaultPanelColour());
}
#endif

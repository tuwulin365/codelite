//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//
// copyright            : (C) 2014 Eran Ifrah
// file name            : LLDBPlugin.cpp
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

#include "DebugAdapterClient.hpp"

#include "AsyncProcess/asyncprocess.h"
#include "AsyncProcess/processreaderthread.h"
#include "DAPBreakpointsView.h"
#include "DAPConsoleOutput.hpp"
#include "DAPDebuggerPane.h"
#include "DAPMainView.h"
#include "DAPOutputPane.hpp"
#include "DAPTextView.h"
#include "DAPTooltip.hpp"
#include "DAPWatchesView.h"
#include "DapDebuggerSettingsDlg.h"
#include "DapLocator.hpp"
#include "Debugger/debuggermanager.h"
#include "FileSystemWorkspace/clFileSystemWorkspace.hpp"
#include "StringUtils.h"
#include "clResizableTooltip.h"
#include "clWorkspaceManager.h"
#include "environmentconfig.h"
#include "event_notifier.h"
#include "file_logger.h"
#include "globals.h"
#include "macromanager.h"
#include "wx/msgqueue.h"

#include <wx/aui/framemanager.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/stc/stc.h>
#include <wx/xrc/xmlres.h>

#if USE_SFTP
#include "sftp_settings.h"
#endif

namespace
{
clModuleLogger LOG;
#ifdef __WXMSW__
constexpr bool IS_WINDOWS = true;
#else
constexpr bool IS_WINDOWS = false;
#endif

const wxString DAP_DEBUGGER_PANE = _("Debugger Client");
const wxString DAP_MESSAGE_BOX_TITLE = "CodeLite - Debug Adapter Client";

// Reusing gdb ids so global debugger menu and accelerators work.
const int lldbRunToCursorContextMenuId = XRCID("dbg_run_to_cursor");
const int lldbJumpToCursorContextMenuId = XRCID("dbg_jump_cursor");
const int lldbAddWatchContextMenuId = XRCID("lldb_add_watch");

std::vector<wxString> to_string_array(const clEnvList_t& env_list)
{
    std::vector<wxString> arr;
    arr.reserve(env_list.size());
    for (const auto& vt : env_list) {
        arr.emplace_back(vt.first + "=" + vt.second);
    }
    return arr;
}

wxString get_dap_settings_file()
{
    wxFileName fn(clStandardPaths::Get().GetUserDataDir(), "debug-adapter-client.conf");
    fn.AppendDir("config");
    return fn.GetFullPath();
}

class StdioTransport : public dap::Transport
{
public:
    StdioTransport() {}
    ~StdioTransport() override {}

    void SetProcess(DapProcess::Ptr_t process) { m_dap_server = process; }

    /**
     * @brief return from the network with a given timeout
     * @returns true on success, false in case of an error. True is also returned when timeout occurs, check the buffer
     * length if it is 0, timeout occurred
     */
    bool Read(std::string& buffer, int msTimeout) override
    {
        if (wxThread::IsMain()) {
            LOG_ERROR(LOG) << "StdioTransport::Read is called from the main thread!" << endl;
            return false;
        }

        std::string msg;
        switch (m_dap_server->Queue().ReceiveTimeout(msTimeout, msg)) {
        case wxMSGQUEUE_NO_ERROR:
        case wxMSGQUEUE_TIMEOUT:
            buffer.swap(msg);
            return true;
        default:
            return false;
        }
    }

    /**
     * @brief send data over the network
     * @return number of bytes written
     */
    size_t Send(const std::string& buffer) override
    {
        if (!m_dap_server->Write(buffer)) {
            return 0;
        }
        return buffer.length();
    }

private:
    DapProcess::Ptr_t m_dap_server;
};
} // namespace

#define CHECK_IS_DAP_CONNECTED()   \
    if (!m_client.IsConnected()) { \
        event.Skip();              \
        return;                    \
    }

// Define the plugin entry point
CL_PLUGIN_API IPlugin* CreatePlugin(IManager* manager) { return new DebugAdapterClient(manager); }

CL_PLUGIN_API PluginInfo* GetPluginInfo()
{
    static PluginInfo info;
    info.SetAuthor(wxT("eran"));
    info.SetName(wxT("DebugAdapterClient"));
    info.SetDescription(_("Debug Adapter Client"));
    info.SetVersion(wxT("v1.0"));
    return &info;
}

CL_PLUGIN_API int GetPluginInterfaceVersion() { return PLUGIN_INTERFACE_VERSION; }

DebugAdapterClient::DebugAdapterClient(IManager* manager)
    : IPlugin(manager)
    , m_terminal_helper(LOG)
    , m_isPerspectiveLoaded(false)
{
    // setup custom logger for this module
    wxFileName logfile(clStandardPaths::Get().GetUserDataDir(), "dap.log");
    logfile.AppendDir("logs");

    LOG.Open(logfile);
    LOG.SetModule("dap");

    // even though set to DBG, the check is done against the global log verbosity
    LOG.SetCurrentLogLevel(FileLogger::Dbg);

    LOG_DEBUG(LOG) << "Debug Adapter Client startd" << endl;
    m_longName = _("Debug Adapter Client");
    m_shortName = wxT("DebugAdapterClient");

    // load settings
    m_dap_store.Load(get_dap_settings_file());

    RegisterDebuggers();

    Bind(wxEVT_ASYNC_PROCESS_OUTPUT, &DebugAdapterClient::OnProcessOutput, this);
    Bind(wxEVT_ASYNC_PROCESS_TERMINATED, &DebugAdapterClient::OnProcessTerminated, this);

    // UI events
    EventNotifier::Get()->Bind(wxEVT_FILE_LOADED, &DebugAdapterClient::OnFileLoaded, this);
    EventNotifier::Get()->Bind(wxEVT_WORKSPACE_LOADED, &DebugAdapterClient::OnWorkspaceLoaded, this);
    EventNotifier::Get()->Bind(wxEVT_WORKSPACE_CLOSED, &DebugAdapterClient::OnWorkspaceClosed, this);

    EventNotifier::Get()->Bind(wxEVT_DBG_UI_START, &DebugAdapterClient::OnDebugStart, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_CONTINUE, &DebugAdapterClient::OnDebugContinue, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_NEXT, &DebugAdapterClient::OnDebugNext, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_STEP_IN, &DebugAdapterClient::OnDebugStepIn, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_STEP_OUT, &DebugAdapterClient::OnDebugStepOut, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_STOP, &DebugAdapterClient::OnDebugStop, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_IS_RUNNING, &DebugAdapterClient::OnDebugIsRunning, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_CAN_INTERACT, &DebugAdapterClient::OnDebugCanInteract, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_INTERRUPT, &DebugAdapterClient::OnToggleInterrupt, this);
    EventNotifier::Get()->Bind(wxEVT_BUILD_STARTING, &DebugAdapterClient::OnBuildStarting, this);
    EventNotifier::Get()->Bind(wxEVT_INIT_DONE, &DebugAdapterClient::OnInitDone, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_EXPR_TOOLTIP, &DebugAdapterClient::OnDebugTooltip, this);
    EventNotifier::Get()->Bind(wxEVT_QUICK_DEBUG, &DebugAdapterClient::OnDebugQuickDebug, this);
    EventNotifier::Get()->Bind(wxEVT_TOOLTIP_DESTROY, &DebugAdapterClient::OnDestroyTip, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_CORE_FILE, &DebugAdapterClient::OnDebugCoreFile, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_DELETE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugDeleteAllBreakpoints,
                               this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_ATTACH_TO_PROCESS, &DebugAdapterClient::OnDebugAttachToProcess, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_ENABLE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugEnableAllBreakpoints,
                               this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_DISABLE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugDisableAllBreakpoints,
                               this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_NEXT_INST, &DebugAdapterClient::OnDebugNextInst, this);
    EventNotifier::Get()->Bind(wxEVT_DBG_UI_STEP_I, &DebugAdapterClient::OnDebugVOID, this); // not supported

    EventNotifier::Get()->Bind(wxEVT_DBG_UI_SHOW_CURSOR, &DebugAdapterClient::OnDebugShowCursor, this);
    wxTheApp->Bind(wxEVT_MENU, &DebugAdapterClient::OnSettings, this, XRCID("lldb_settings"));

    wxTheApp->Bind(wxEVT_COMMAND_MENU_SELECTED, &DebugAdapterClient::OnAddWatch, this, lldbAddWatchContextMenuId);
    wxTheApp->Bind(wxEVT_IDLE, &DebugAdapterClient::OnIdle, this);
    dap::Initialize(); // register all dap objects

    m_client.SetWantsLogEvents(true);
    m_client.Bind(wxEVT_DAP_INITIALIZE_RESPONSE, &DebugAdapterClient::OnDapInitializeResponse, this);
    m_client.Bind(wxEVT_DAP_INITIALIZED_EVENT, &DebugAdapterClient::OnDapInitializedEvent, this);
    m_client.Bind(wxEVT_DAP_RUN_IN_TERMINAL_REQUEST, &DebugAdapterClient::OnDapRunInTerminal, this);
    m_client.Bind(wxEVT_DAP_EXITED_EVENT, &DebugAdapterClient::OnDapExited, this);
    m_client.Bind(wxEVT_DAP_TERMINATED_EVENT, &DebugAdapterClient::OnDapExited, this);
    m_client.Bind(wxEVT_DAP_LAUNCH_RESPONSE, &DebugAdapterClient::OnDapLaunchResponse, this);
    m_client.Bind(wxEVT_DAP_STOPPED_EVENT, &DebugAdapterClient::OnDapStoppedEvent, this);
    m_client.Bind(wxEVT_DAP_THREADS_RESPONSE, &DebugAdapterClient::OnDapThreadsResponse, this);
    m_client.Bind(wxEVT_DAP_STACKTRACE_RESPONSE, &DebugAdapterClient::OnDapStackTraceResponse, this);
    m_client.Bind(wxEVT_DAP_SCOPES_RESPONSE, &DebugAdapterClient::OnDapScopesResponse, this);
    m_client.Bind(wxEVT_DAP_VARIABLES_RESPONSE, &DebugAdapterClient::OnDapVariablesResponse, this);
    m_client.Bind(wxEVT_DAP_SET_FUNCTION_BREAKPOINT_RESPONSE, &DebugAdapterClient::OnDapSetFunctionBreakpointResponse,
                  this);
    m_client.Bind(wxEVT_DAP_SET_SOURCE_BREAKPOINT_RESPONSE, &DebugAdapterClient::OnDapSetSourceBreakpointResponse,
                  this);
    m_client.Bind(wxEVT_DAP_LOG_EVENT, &DebugAdapterClient::OnDapLog, this);
    m_client.Bind(wxEVT_DAP_BREAKPOINT_EVENT, &DebugAdapterClient::OnDapBreakpointEvent, this);
    m_client.Bind(wxEVT_DAP_OUTPUT_EVENT, &DebugAdapterClient::OnDapOutputEvent, this);
    m_client.Bind(wxEVT_DAP_MODULE_EVENT, &DebugAdapterClient::OnDapModuleEvent, this);
    EventNotifier::Get()->Bind(wxEVT_NOTIFY_PAGE_CLOSING, &DebugAdapterClient::OnPageClosing, this);
}

void DebugAdapterClient::UnPlug()
{
    wxDELETE(m_breakpointsHelper);
    wxTheApp->Unbind(wxEVT_IDLE, &DebugAdapterClient::OnIdle, this);
    // DestroyUI();
    DebuggerMgr::Get().UnregisterDebuggers(m_shortName);

    // UI events
    EventNotifier::Get()->Unbind(wxEVT_FILE_LOADED, &DebugAdapterClient::OnFileLoaded, this);
    EventNotifier::Get()->Unbind(wxEVT_WORKSPACE_LOADED, &DebugAdapterClient::OnWorkspaceLoaded, this);
    EventNotifier::Get()->Unbind(wxEVT_WORKSPACE_CLOSED, &DebugAdapterClient::OnWorkspaceClosed, this);

    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_START, &DebugAdapterClient::OnDebugStart, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_CONTINUE, &DebugAdapterClient::OnDebugContinue, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_NEXT, &DebugAdapterClient::OnDebugNext, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_STOP, &DebugAdapterClient::OnDebugStop, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_IS_RUNNING, &DebugAdapterClient::OnDebugIsRunning, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_CAN_INTERACT, &DebugAdapterClient::OnDebugCanInteract, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_STEP_IN, &DebugAdapterClient::OnDebugStepIn, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_STEP_OUT, &DebugAdapterClient::OnDebugStepOut, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_INTERRUPT, &DebugAdapterClient::OnToggleInterrupt, this);
    EventNotifier::Get()->Unbind(wxEVT_BUILD_STARTING, &DebugAdapterClient::OnBuildStarting, this);
    EventNotifier::Get()->Unbind(wxEVT_INIT_DONE, &DebugAdapterClient::OnInitDone, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_EXPR_TOOLTIP, &DebugAdapterClient::OnDebugTooltip, this);
    EventNotifier::Get()->Unbind(wxEVT_QUICK_DEBUG, &DebugAdapterClient::OnDebugQuickDebug, this);
    EventNotifier::Get()->Unbind(wxEVT_TOOLTIP_DESTROY, &DebugAdapterClient::OnDestroyTip, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_CORE_FILE, &DebugAdapterClient::OnDebugCoreFile, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_DELETE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugDeleteAllBreakpoints,
                                 this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_ATTACH_TO_PROCESS, &DebugAdapterClient::OnDebugAttachToProcess, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_ENABLE_ALL_BREAKPOINTS, &DebugAdapterClient::OnDebugEnableAllBreakpoints,
                                 this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_DISABLE_ALL_BREAKPOINTS,
                                 &DebugAdapterClient::OnDebugDisableAllBreakpoints, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_STEP_I, &DebugAdapterClient::OnDebugVOID, this); // Not supported
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_NEXT_INST, &DebugAdapterClient::OnDebugNextInst, this);
    EventNotifier::Get()->Unbind(wxEVT_DBG_UI_SHOW_CURSOR, &DebugAdapterClient::OnDebugShowCursor, this);
    wxTheApp->Unbind(wxEVT_MENU, &DebugAdapterClient::OnSettings, this, XRCID("lldb_settings"));

    // Dap events
    m_client.Unbind(wxEVT_DAP_INITIALIZE_RESPONSE, &DebugAdapterClient::OnDapInitializeResponse, this);
    m_client.Unbind(wxEVT_DAP_INITIALIZED_EVENT, &DebugAdapterClient::OnDapInitializedEvent, this);
    m_client.Unbind(wxEVT_DAP_RUN_IN_TERMINAL_REQUEST, &DebugAdapterClient::OnDapRunInTerminal, this);
    m_client.Unbind(wxEVT_DAP_EXITED_EVENT, &DebugAdapterClient::OnDapExited, this);
    m_client.Unbind(wxEVT_DAP_TERMINATED_EVENT, &DebugAdapterClient::OnDapExited, this);
    m_client.Unbind(wxEVT_DAP_LAUNCH_RESPONSE, &DebugAdapterClient::OnDapLaunchResponse, this);
    m_client.Unbind(wxEVT_DAP_STOPPED_EVENT, &DebugAdapterClient::OnDapStoppedEvent, this);
    m_client.Unbind(wxEVT_DAP_THREADS_RESPONSE, &DebugAdapterClient::OnDapThreadsResponse, this);
    m_client.Unbind(wxEVT_DAP_STACKTRACE_RESPONSE, &DebugAdapterClient::OnDapStackTraceResponse, this);
    m_client.Unbind(wxEVT_DAP_SCOPES_RESPONSE, &DebugAdapterClient::OnDapScopesResponse, this);
    m_client.Unbind(wxEVT_DAP_VARIABLES_RESPONSE, &DebugAdapterClient::OnDapVariablesResponse, this);
    m_client.Unbind(wxEVT_DAP_SET_FUNCTION_BREAKPOINT_RESPONSE, &DebugAdapterClient::OnDapSetFunctionBreakpointResponse,
                    this);
    m_client.Unbind(wxEVT_DAP_SET_SOURCE_BREAKPOINT_RESPONSE, &DebugAdapterClient::OnDapSetSourceBreakpointResponse,
                    this);
    m_client.Unbind(wxEVT_DAP_LOG_EVENT, &DebugAdapterClient::OnDapLog, this);
    m_client.Unbind(wxEVT_DAP_BREAKPOINT_EVENT, &DebugAdapterClient::OnDapBreakpointEvent, this);
    m_client.Unbind(wxEVT_DAP_OUTPUT_EVENT, &DebugAdapterClient::OnDapOutputEvent, this);
    m_client.Unbind(wxEVT_DAP_MODULE_EVENT, &DebugAdapterClient::OnDapModuleEvent, this);
    EventNotifier::Get()->Unbind(wxEVT_NOTIFY_PAGE_CLOSING, &DebugAdapterClient::OnPageClosing, this);
}

DebugAdapterClient::~DebugAdapterClient() {}

void DebugAdapterClient::RegisterDebuggers()
{
    wxArrayString debuggers;
    debuggers.reserve(m_dap_store.GetEntries().size());
    for (const auto& entry : m_dap_store.GetEntries()) {
        debuggers.Add(entry.first);
    }
    DebuggerMgr::Get().RegisterDebuggers(m_shortName, debuggers);
}

void DebugAdapterClient::CreateToolBar(clToolBarGeneric* toolbar) { wxUnusedVar(toolbar); }

void DebugAdapterClient::CreatePluginMenu(wxMenu* pluginsMenu)
{
    // We want to add an entry in the global settings menu
    // Menu Bar > Settings > LLDB Settings

    // Get the main frame's menubar
    auto mb = clGetManager()->GetMenuBar();
    if (mb) {
        wxMenu* settingsMenu(NULL);
        int menuPos = mb->FindMenu(_("Settings"));
        if (menuPos != wxNOT_FOUND) {
            settingsMenu = mb->GetMenu(menuPos);
            if (settingsMenu) {
                settingsMenu->Append(XRCID("lldb_settings"), _("Debug Adapter Client..."));
            }
        }
    }
}

void DebugAdapterClient::HookPopupMenu(wxMenu* menu, MenuType type)
{
    wxUnusedVar(type);
    wxUnusedVar(menu);
}

void DebugAdapterClient::ClearDebuggerMarker()
{
    IEditor::List_t editors;
    clGetManager()->GetAllEditors(editors);

    for (auto editor : editors) {
        DAPTextView::ClearMarker(editor->GetCtrl());
    }
}

void DebugAdapterClient::RefreshBreakpointsView()
{
    if (GetBreakpointsView()) {
        GetBreakpointsView()->RefreshView(m_sessionBreakpoints);
    }

    // clear all breakpoint markers
    IEditor::List_t editors;
    clGetManager()->GetAllEditors(editors);
    for (auto editor : editors) {
        editor->DeleteBreakpointMarkers();
    }

    // update the open editors with breakpoint markers
    for (const auto& bp : m_sessionBreakpoints.get_breakpoints()) {
        wxString path = NormaliseReceivedPath(bp.source.path);
        auto editor = clGetManager()->FindEditor(path);
        if (!editor) {
            continue;
        }
        editor->SetBreakpointMarker(bp.line - 1);
    }
}

void DebugAdapterClient::OnDebugContinue(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    // call continue
    m_client.Continue();
    LOG_DEBUG(LOG) << "Sending 'continue' command" << endl;
}

void DebugAdapterClient::OnDebugStart(clDebugEvent& event)
{
    if (m_client.IsConnected()) {
        // already running - assume "continue"
        OnDebugContinue(event);
        return;
    }

    LOG_DEBUG(LOG) << "debug-start event is called for debugger:" << event.GetDebuggerName() << endl;

    if (!IsDebuggerOwnedByPlugin(event.GetDebuggerName())) {
        event.Skip();
        LOG_DEBUG(LOG) << "Not a dap debugger (" << event.GetDebuggerName() << ")" << endl;
        return;
    }

    // fetch the requested debugger details
    DapEntry dap_server;
    m_dap_store.Get(event.GetDebuggerName(), &dap_server);

    LOG_DEBUG(LOG) << "working directory is:" << ::wxGetCwd() << endl;

    // the following 4 variables are used for launching the debugger
    wxString working_directory;
    wxString exepath;
    wxString args;
    clEnvList_t env;
    wxString ssh_account;

    if (clCxxWorkspaceST::Get()->IsOpen()) {
        //
        // standard C++ workspace
        //
        ProjectPtr project = clCxxWorkspaceST::Get()->GetActiveProject();
        if (!project) {
            ::wxMessageBox(wxString() << _("Could not locate project: ")
                                      << clCxxWorkspaceST::Get()->GetActiveProjectName(),
                           DAP_MESSAGE_BOX_TITLE, wxICON_ERROR | wxOK | wxCENTER);
            LOG_ERROR(LOG) << "unable to locate project:" << clCxxWorkspaceST::Get()->GetActiveProjectName() << endl;
            return;
        }

        BuildConfigPtr bldConf = project->GetBuildConfiguration();
        if (!bldConf) {
            ::wxMessageBox(wxString() << _("Could not locate the requested build configuration"), DAP_MESSAGE_BOX_TITLE,
                           wxICON_ERROR | wxOK | wxCENTER);
            return;
        }

        // Determine the executable to debug, working directory and arguments
        LOG_DEBUG(LOG) << "Preparing environment variables.." << endl;
        env = bldConf->GetEnvironment(project.get());
        LOG_DEBUG(LOG) << "Success" << endl;
        exepath = bldConf->GetCommand();

        // Get the debugging arguments.
        if (bldConf->GetUseSeparateDebugArgs()) {
            args = bldConf->GetDebugArgs();
        } else {
            args = bldConf->GetCommandArguments();
        }

        working_directory = MacroManager::Instance()->Expand(bldConf->GetWorkingDirectory(), m_mgr, project->GetName());
        exepath = MacroManager::Instance()->Expand(exepath, m_mgr, project->GetName());

        if (working_directory.empty()) {
            working_directory = wxGetCwd();
        }
        wxFileName fn(exepath);
        if (fn.IsRelative()) {
            fn.MakeAbsolute(working_directory);
        }
        exepath = fn.GetFullPath();
    } else if (clFileSystemWorkspace::Get().IsOpen()) {
        //
        // Handle file system workspace
        //
        auto conf = clFileSystemWorkspace::Get().GetSettings().GetSelectedConfig();
        if (!conf) {
            LOG_ERROR(LOG) << "No active configuration found!" << endl;
            return;
        }

        auto workspace = clWorkspaceManager::Get().GetWorkspace();
        bool is_remote = workspace->IsRemote();
        ssh_account = workspace->GetSshAccount();

        clFileSystemWorkspace::Get().GetExecutable(exepath, args, working_directory);
        if (is_remote) {
            env = StringUtils::BuildEnvFromString(conf->GetEnvironment());
        } else {
            env = StringUtils::ResolveEnvList(conf->GetEnvironment());
            wxFileName fnExepath(exepath);
            if (fnExepath.IsRelative()) {
                fnExepath.MakeAbsolute(workspace->GetDir());
            }
            exepath = fnExepath.GetFullPath();
        }
    }

    if (working_directory.empty()) {
        // always pass a working directory
        working_directory =
            clWorkspaceManager::Get().IsWorkspaceOpened()
                ? wxFileName(clWorkspaceManager::Get().GetWorkspace()->GetFileName()).GetPath(wxPATH_UNIX)
                : ::wxGetCwd();
    }

    // start the debugger
    LOG_DEBUG(LOG) << "Initializing debugger for executable:" << exepath << endl;
    if (!InitialiseSession(dap_server, exepath, args, working_directory, ssh_account, env)) {
        return;
    }
    StartAndConnectToDapServer();
}

void DebugAdapterClient::OnDebugNext(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    LOG_DEBUG(LOG) << "-> Next" << endl;
    m_client.Next();
}

void DebugAdapterClient::OnDebugStop(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    LOG_DEBUG(LOG) << "-> Stop" << endl;
    DoCleanup();
}

void DebugAdapterClient::OnDebugIsRunning(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    event.SetAnswer(m_client.IsConnected());
}

void DebugAdapterClient::OnDebugCanInteract(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    event.SetAnswer(m_client.IsConnected() && m_client.CanInteract());
}

void DebugAdapterClient::OnDebugStepIn(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    m_client.StepIn();
    LOG_DEBUG(LOG) << "-> StopIn" << endl;
}

void DebugAdapterClient::OnDebugStepOut(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    m_client.StepOut();
    LOG_DEBUG(LOG) << "-> StopOut" << endl;
}

void DebugAdapterClient::RestoreUI()
{
    // Save current perspective before destroying the session
    if (m_isPerspectiveLoaded) {
        m_mgr->SavePerspective("DAP");

        // Restore the old perspective
        m_mgr->LoadPerspective("Default");
        m_isPerspectiveLoaded = false;
    }

    HideDebuggerUI();
}

void DebugAdapterClient::LoadPerspective()
{
    // Save the current persepctive we start debguging
    m_mgr->SavePerspective("Default");

    // Hide all the panes
    auto& panes = m_mgr->GetDockingManager()->GetAllPanes();
    for (size_t i = 0; i < panes.size(); ++i) {
        auto& pane = panes[i];
        if (pane.dock_direction != wxAUI_DOCK_CENTER) {
            pane.Hide();
        }
    }

    m_mgr->LoadPerspective("DAP");
    m_isPerspectiveLoaded = true;

    // Make sure that all the panes are visible
    ShowPane(DAP_DEBUGGER_PANE, true);

    // Hide the output pane
    wxAuiPaneInfo& pi = m_mgr->GetDockingManager()->GetPane("Output View");
    if (pi.IsOk() && pi.IsShown()) {
        pi.Hide();
    }
    m_mgr->GetDockingManager()->Update();
}

void DebugAdapterClient::ShowPane(const wxString& paneName, bool show)
{
    wxAuiPaneInfo& pi = m_mgr->GetDockingManager()->GetPane(paneName);
    if (pi.IsOk()) {
        if (show) {
            if (!pi.IsShown()) {
                pi.Show();
            }
        } else {
            if (pi.IsShown()) {
                pi.Hide();
            }
        }
    }
}

void DebugAdapterClient::HideDebuggerUI()
{
    // Destroy the callstack window
    if (m_debuggerPane) {
        wxAuiPaneInfo& pi = m_mgr->GetDockingManager()->GetPane(DAP_DEBUGGER_PANE);
        if (pi.IsOk()) {
            m_mgr->GetDockingManager()->DetachPane(m_debuggerPane);
        }
        m_debuggerPane->Destroy();
        m_debuggerPane = nullptr;
    }

    if (m_textView) {
        int index = clGetManager()->GetMainNotebook()->FindPage(m_textView);
        if (index != wxNOT_FOUND) {
            clGetManager()->GetMainNotebook()->RemovePage(index, false);
        }
        m_textView->Destroy();
        m_textView = nullptr;
    }

    DestroyTooltip();
    ClearDebuggerMarker();
    m_mgr->GetDockingManager()->Update();
    EventNotifier::Get()->TopFrame()->PostSizeEvent();
}

void DebugAdapterClient::InitializeUI()
{
    wxWindow* parent = m_mgr->GetDockingManager()->GetManagedWindow();
    if (!m_debuggerPane) {
        m_debuggerPane = new DAPDebuggerPane(parent, this, LOG);
        m_mgr->GetDockingManager()->AddPane(m_debuggerPane, wxAuiPaneInfo()
                                                                .MinSize(300, 300)
                                                                .Layer(10)
                                                                .Bottom()
                                                                .Position(1)
                                                                .CloseButton(false)
                                                                .Caption(DAP_DEBUGGER_PANE)
                                                                .Name(DAP_DEBUGGER_PANE));
    }

    if (!m_textView) {
        m_textView = new DAPTextView(clGetManager()->GetMainNotebook());
        clGetManager()->GetMainNotebook()->AddPage(m_textView, _("Debug Adapter Client"), true);
    }
}

void DebugAdapterClient::DoCleanup()
{
    m_client.Reset();
    ClearDebuggerMarker();
    m_raisOnBpHit = false;
    StopProcess();
    m_session.Clear();
    m_terminal_helper.Terminate();
    m_sessionBreakpoints.clear();
    wxDELETE(m_breakpointsHelper);

    // clear all breakpoint markers
    IEditor::List_t editors;
    clGetManager()->GetAllEditors(editors);
    for (auto editor : editors) {
        editor->DeleteBreakpointMarkers();
    }

    clDebuggerBreakpoint::Vec_t all_bps;
    clGetManager()->GetAllBreakpoints(all_bps);

    for (const auto& bp : all_bps) {
        if (bp.file.empty()) {
            continue;
        }

        auto editor = clGetManager()->FindEditor(bp.file);
        if (!editor) {
            continue;
        }
        editor->SetBreakpointMarker(bp.lineno - 1);
    }
}

void DebugAdapterClient::OnWorkspaceClosed(clWorkspaceEvent& event)
{
    event.Skip();
    DoCleanup();
}

void DebugAdapterClient::OnWorkspaceLoaded(clWorkspaceEvent& event) { event.Skip(); }

void DebugAdapterClient::OnToggleInterrupt(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    event.Skip();
    m_client.Pause();
}

void DebugAdapterClient::OnBuildStarting(clBuildEvent& event)
{
    if (m_client.IsConnected()) {
        // lldb session is active, prompt the user
        if (::wxMessageBox(_("A debug session is running\nCancel debug session and continue building?"),
                           DAP_MESSAGE_BOX_TITLE, wxICON_QUESTION | wxYES_NO | wxNO_DEFAULT | wxCENTER) == wxYES) {
            clDebugEvent dummy;
            OnDebugStop(dummy);
            event.Skip();
        } else {
            // do nothing - this will cancel the build
        }

    } else {
        event.Skip();
    }
}

void DebugAdapterClient::OnAddWatch(wxCommandEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    // FIXME
}

void DebugAdapterClient::OnSettings(wxCommandEvent& event)
{
    event.Skip();
    clDapSettingsStore store = m_dap_store;
    DapDebuggerSettingsDlg dlg(EventNotifier::Get()->TopFrame(), store);
    if (dlg.ShowModal() != wxID_OK) {
        return;
    }
    m_dap_store = store;
    m_dap_store.Save(get_dap_settings_file());

    // refresh the list of debuggers we are registering by this plugin
    RegisterDebuggers();
}

void DebugAdapterClient::OnInitDone(wxCommandEvent& event)
{
    event.Skip();
    if (!m_dap_store.empty()) {
        return;
    }
    // this seems like a good time to scan for available debuggers
    DapLocator locator;
    std::vector<DapEntry> entries;
    if (locator.Locate(&entries) > 0) {
        m_dap_store.Set(entries);
        m_dap_store.Save(get_dap_settings_file());
        LOG_SYSTEM(LOG) << "Found and configured" << entries.size() << "dap servers" << endl;
        RegisterDebuggers();
    }
}

void DebugAdapterClient::OnDebugTooltip(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    DestroyTooltip();

    wxString word = event.GetString();
    int frame_id = GetThreadsView()->GetCurrentFrameId();

    m_client.EvaluateExpression(
        word, frame_id, dap::EvaluateContext::HOVER,
        [this, word](bool success, const wxString& result, const wxString& type, int variablesReference) {
            if (!success) {
                clGetManager()->SetStatusMessage(_("Failed to evaluate expression: ") + word);
                return;
            }

            auto editor = clGetManager()->GetActiveEditor();
            CHECK_PTR_RET(editor);
            m_tooltip = new DAPTooltip(&m_client, word, result, type, variablesReference);
            m_tooltip->Move(::wxGetMousePosition());
            m_tooltip->Show();
        });
}

void DebugAdapterClient::OnDestroyTip(clCommandEvent& event)
{
    event.Skip();
    DestroyTooltip();
}

void DebugAdapterClient::OnDebugQuickDebug(clDebugEvent& event)
{
    if (!IsDebuggerOwnedByPlugin(event.GetDebuggerName())) {
        event.Skip();
        return;
    }

    // ours to handle
    event.Skip(false);
    wxString exe_to_debug = event.GetExecutableName();
    const wxString& working_dir = event.GetWorkingDirectory();
    const wxString& args = event.GetArguments();

    wxFileName fnExepath(exe_to_debug);
    if (fnExepath.IsRelative()) {
        wxString cwd;
        if (clFileSystemWorkspace::Get().IsOpen()) {
            cwd = clFileSystemWorkspace::Get().GetDir();
        }
        fnExepath.MakeAbsolute(cwd);
    }

#ifdef __WXMSW__
    fnExepath.SetExt("exe");
#endif

    exe_to_debug = fnExepath.GetFullPath();

    // fetch the requested debugger details
    DapEntry dap_server;
    m_dap_store.Get(event.GetDebuggerName(), &dap_server);

    auto env = PrepareEnvForFileSystemWorkspace(dap_server, !event.IsSSHDebugging());
    if (!InitialiseSession(dap_server, exe_to_debug, args, working_dir, event.GetSshAccount(), env)) {
        return;
    }
    StartAndConnectToDapServer();
}

void DebugAdapterClient::OnDebugCoreFile(clDebugEvent& event)
{
    // FIXME
    event.Skip();
}

void DebugAdapterClient::OnDebugAttachToProcess(clDebugEvent& event)
{
    // FIXME
    event.Skip();
}

void DebugAdapterClient::OnDebugDeleteAllBreakpoints(clDebugEvent& event)
{
    event.Skip();
    // FIXME
}

void DebugAdapterClient::OnDebugDisableAllBreakpoints(clDebugEvent& event) { event.Skip(); }

void DebugAdapterClient::OnDebugEnableAllBreakpoints(clDebugEvent& event) { event.Skip(); }

void DebugAdapterClient::OnDebugVOID(clDebugEvent& event) { CHECK_IS_DAP_CONNECTED(); }

void DebugAdapterClient::OnDebugNextInst(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    m_client.Next(wxNOT_FOUND, true, dap::SteppingGranularity::INSTRUCTION);
}

void DebugAdapterClient::OnDebugShowCursor(clDebugEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    // FIXME
}

/// --------------------------------------------------------------------------
/// dap events starting here
/// --------------------------------------------------------------------------

void DebugAdapterClient::OnDapExited(DAPEvent& event)
{
    event.Skip();
    LOG_DEBUG(LOG) << "dap-server exited" << endl;
    DoCleanup();
}

void DebugAdapterClient::OnDapLog(DAPEvent& event)
{
    event.Skip();
    LOG_DEBUG(LOG) << event.GetString() << endl;
}

void DebugAdapterClient::OnDapOutputEvent(DAPEvent& event)
{
    if (GetOutputView()) {
        GetOutputView()->AddEvent(event.GetDapEvent()->As<dap::OutputEvent>());
    }
}

void DebugAdapterClient::OnDapModuleEvent(DAPEvent& event)
{
    CHECK_IS_DAP_CONNECTED();
    if (GetOutputView()) {
        GetOutputView()->AddEvent(event.GetDapEvent()->As<dap::ModuleEvent>());
    }
}

void DebugAdapterClient::OnDapLaunchResponse(DAPEvent& event)
{
    // Check that the debugee was started successfully
    dap::LaunchResponse* resp = event.GetDapResponse()->As<dap::LaunchResponse>();

    if (resp && !resp->success) {
        // launch failed!
        wxMessageBox("Failed to launch debuggee: " + resp->message, DAP_MESSAGE_BOX_TITLE,
                     wxICON_ERROR | wxOK | wxOK_DEFAULT | wxCENTRE);
        CallAfter(&DebugAdapterClient::DoCleanup);
    }
}

void DebugAdapterClient::OnDapInitializeResponse(DAPEvent& event)
{
    if (m_session.working_directory.empty() && m_session.dap_server.GetLaunchType() == DapLaunchType::LAUNCH) {
        // ensure we have a working directory
        m_session.working_directory =
            clWorkspaceManager::Get().IsWorkspaceOpened()
                ? wxFileName(clWorkspaceManager::Get().GetWorkspace()->GetFileName()).GetPath(wxPATH_UNIX)
                : ::wxGetCwd();
    }

    LOG_DEBUG(LOG) << "got initialize response" << endl;
    LOG_DEBUG(LOG) << "Starting debugger for command:" << endl;
    LOG_DEBUG(LOG) << m_session.command << endl;
    LOG_DEBUG(LOG) << "working directory:" << m_session.working_directory << endl;

    // FIXME: apply the environment here
    LOG_DEBUG(LOG) << "Calling Launch() with command:" << m_session.command << endl;
    if (m_session.dap_server.GetLaunchType() == DapLaunchType::LAUNCH) {
        auto v = m_session.command;
        m_client.Launch(std::move(v), m_session.working_directory, m_session.MakeEnvironment());

    } else {
        auto v = m_session.command;
        v.erase(v.begin()); // remove the exe and pass just the arguments
        m_client.Attach(m_session.m_pid, v);
    }
}

/// DAP server responded to our `initialize` request
void DebugAdapterClient::OnDapInitializedEvent(DAPEvent& event)
{
    // place a single breakpoint on main
    dap::FunctionBreakpoint main_bp{ "main" };
    m_session.need_to_set_breakpoints = true;
    m_client.SetFunctionBreakpoints({ main_bp });

    if (m_breakpointsHelper) {
        m_breakpointsHelper->ApplyBreakpoints(wxEmptyString);
    }
    // place all breakpoints
    m_client.ConfigurationDone();
}

void DebugAdapterClient::OnDapStoppedEvent(DAPEvent& event)
{
    // raise CodeLite
    EventNotifier::Get()->TopFrame()->Raise();

    // got stopped event
    if (m_session.need_to_set_breakpoints) {
        if (m_breakpointsHelper) {
            m_breakpointsHelper->ApplyBreakpoints(wxEmptyString);
        }
        m_session.need_to_set_breakpoints = false;
    }

    LOG_DEBUG(LOG) << " *** DAP Stopped Event *** " << endl;
    dap::StoppedEvent* stopped_data = event.GetDapEvent()->As<dap::StoppedEvent>();
    if (stopped_data) {
        m_client.GetThreads();
    }

    // update watches if needed
    UpdateWatches();
}

void DebugAdapterClient::OnDapThreadsResponse(DAPEvent& event)
{
    CHECK_PTR_RET(GetThreadsView());

    auto response = event.GetDapResponse()->As<dap::ThreadsResponse>();
    CHECK_PTR_RET(response);

    GetThreadsView()->UpdateThreads(m_client.GetActiveThreadId(), response);

    // get the frames for the active thread
    m_client.GetFrames();
}

void DebugAdapterClient::OnDapStackTraceResponse(DAPEvent& event)
{
    CHECK_PTR_RET(GetThreadsView());

    auto response = event.GetDapResponse()->As<dap::StackTraceResponse>();
    CHECK_PTR_RET(response);

    GetThreadsView()->UpdateFrames(response->refId, response);
    if (!response->stackFrames.empty()) {
        auto frame = response->stackFrames[0];
        LoadFile(frame.source, frame.line - 1);

        // ask the scopes for the first frame
        m_client.GetScopes(frame.id);
    }
}

void DebugAdapterClient::OnDapScopesResponse(DAPEvent& event)
{
    auto response = event.GetDapResponse()->As<dap::ScopesResponse>();
    CHECK_PTR_RET(response);
    CHECK_PTR_RET(GetThreadsView());

    if (!response->success) {
        LOG_DEBUG(LOG) << "failed to retrieve scopes." << response->message << endl;
        return;
    }
    GetThreadsView()->UpdateScopes(response->refId, response);
}

void DebugAdapterClient::OnDapVariablesResponse(DAPEvent& event)
{
    auto response = event.GetDapResponse()->As<dap::VariablesResponse>();
    CHECK_PTR_RET(response);
    CHECK_PTR_RET(GetThreadsView());
    switch (response->context) {
    case dap::EvaluateContext::HOVER:
        if (m_tooltip) {
            m_tooltip->UpdateChildren(response->refId, response);
        }
        break;
    case dap::EvaluateContext::WATCH:
        // update the watches view
        if (GetWatchesView()) {
            GetWatchesView()->UpdateChildren(response->refId, response);
        }
        break;
    default:
        // assume its the variables view
        GetThreadsView()->UpdateVariables(response->refId, response);
        break;
    }
}

void DebugAdapterClient::OnDapSetFunctionBreakpointResponse(DAPEvent& event)
{
    auto resp = event.GetDapResponse()->As<dap::SetFunctionBreakpointsResponse>();
    CHECK_PTR_RET(resp);
    m_sessionBreakpoints.delete_by_paths(resp->breakpoints);

    for (const auto& bp : resp->breakpoints) {
        m_sessionBreakpoints.update_or_insert(bp);
    }
    RefreshBreakpointsView();
}

void DebugAdapterClient::OnDapSetSourceBreakpointResponse(DAPEvent& event)
{
    auto resp = event.GetDapResponse()->As<dap::SetBreakpointsResponse>();
    CHECK_PTR_RET(resp);

    auto req = event.GetOriginatingRequest();
    CHECK_PTR_RET(req);

    auto set_bp_req = req->As<dap::SetBreakpointsRequest>();
    CHECK_PTR_RET(set_bp_req);

    // delete all breakpoints associated with the reported file
    // in some cases, the DAP server does not report back a file
    // so we use the originating request path instead
    LOG_DEBUG(LOG) << "Deleting session breakpoints for file:"
                   << (resp->originSource.empty() ? set_bp_req->arguments.source.path : resp->originSource) << endl;
    m_sessionBreakpoints.delete_by_path(resp->originSource);

    for (auto bp : resp->breakpoints) {
        if (bp.source.path.empty()) {
            bp.source.path = set_bp_req->arguments.source.path;
        }
        m_sessionBreakpoints.update_or_insert(bp);
    }
    RefreshBreakpointsView();
}

void DebugAdapterClient::OnDapBreakpointEvent(DAPEvent& event)
{
    auto event_data = event.GetDapEvent()->As<dap::BreakpointEvent>();
    CHECK_PTR_RET(event_data);
    CHECK_PTR_RET(GetBreakpointsView());
    // check the event reason
    auto bp = event_data->breakpoint;

    // load the current bp before we modify it
    dap::Breakpoint before_bp;
    m_sessionBreakpoints.find_by_id(bp.id, &before_bp);
    m_sessionBreakpoints.delete_by_id(bp.id);
    if (event_data->reason != "removed") {
        if (bp.source.path.empty()) {
            bp.source.path = before_bp.source.path;
        }
        m_sessionBreakpoints.update_or_insert(bp);
    }
    RefreshBreakpointsView();
}

void DebugAdapterClient::OnDapRunInTerminal(DAPEvent& event)
{
    auto request = event.GetDapRequest()->As<dap::RunInTerminalRequest>();
    CHECK_PTR_RET(request);

    int process_id = m_terminal_helper.RunProcess(request->arguments.args, wxEmptyString, {});
    // send the response back to the dap server
    auto response = m_client.MakeRequest<dap::RunInTerminalResponse>();
    LOG_DEBUG(LOG) << "RunInTerminal process ID:" << process_id << endl;
    response->request_seq = request->seq;
    if (process_id == wxNOT_FOUND) {
        response->success = false;
        response->processId = 0;
    } else {
        response->success = true;
        response->processId = process_id;
    }
    m_client.SendResponse(*response);
    wxDELETE(response);
}

/// --------------------------------------------------------------------------
/// dap events stops here
/// --------------------------------------------------------------------------

void DebugAdapterClient::UpdateWatches()
{
    if (!m_client.IsConnected()) {
        return;
    }

    CHECK_PTR_RET(GetWatchesView());
    GetWatchesView()->Update(GetThreadsView()->GetCurrentFrameId());
}

bool DebugAdapterClient::StartSocketDap()
{
    m_dap_server.reset();
    const DapEntry& dap_server = m_session.dap_server;
    wxString command = ReplacePlaceholders(dap_server.GetCommand());

    LOG_DEBUG(LOG) << "starting dap with command:" << command << endl;

    if (m_session.debug_over_ssh) {
        // launch ssh process
        auto env_list = StringUtils::BuildEnvFromString(dap_server.GetEnvironment());
        m_dap_server.reset(new DapProcess(::CreateAsyncProcess(
            this, command, IProcessCreateDefault | IProcessCreateSSH | IProcessWrapInShell | IProcessNoPty,
            wxEmptyString, &env_list, m_session.ssh_acount.GetAccountName())));
    } else {
        // launch local process
        EnvSetter env; // apply CodeLite env variables
        auto env_list = StringUtils::ResolveEnvList(dap_server.GetEnvironment());
        m_dap_server.reset(new DapProcess(::CreateAsyncProcess(
            this, command, IProcessNoRedirect | IProcessWrapInShell | IProcessCreateWithHiddenConsole | IProcessNoPty,
            wxEmptyString, &env_list)));
    }
    return m_dap_server->IsOk();
}

dap::Transport* DebugAdapterClient::StartStdioDap()
{
    m_dap_server.reset();
    const DapEntry& dap_server = m_session.dap_server;
    wxString command = ReplacePlaceholders(dap_server.GetCommand());

    LOG_DEBUG(LOG) << "starting dap with command:" << command << endl;

    auto transport = new StdioTransport();

    if (m_session.debug_over_ssh) {
        // launch ssh process
        auto env_list = StringUtils::BuildEnvFromString(dap_server.GetEnvironment());
        m_dap_server.reset(new DapProcess(::CreateAsyncProcess(
            this, command, IProcessCreateDefault | IProcessCreateSSH | IProcessWrapInShell | IProcessNoPty,
            wxEmptyString, &env_list, m_session.ssh_acount.GetAccountName())));
    } else {
        // launch local process
        EnvSetter env; // apply CodeLite env variables
        auto env_list = StringUtils::ResolveEnvList(dap_server.GetEnvironment());
        m_dap_server.reset(new DapProcess(::CreateAsyncProcess(
            this, command, IProcessWrapInShell | IProcessStderrEvent | IProcessCreateWithHiddenConsole | IProcessNoPty,
            wxEmptyString, &env_list)));
    }

    transport->SetProcess(m_dap_server);
    if (!m_dap_server->IsOk()) {
        m_dap_server.reset();
        wxDELETE(transport);
        return nullptr;
    }
    return transport;
}

bool DebugAdapterClient::InitialiseSession(const DapEntry& dap_server, const wxString& exepath, const wxString& args,
                                           const wxString& working_directory, const wxString& ssh_account,
                                           const clEnvList_t& env)
{
    m_session.Clear();
    m_session.dap_server = dap_server;
    wxArrayString command_array = StringUtils::BuildArgv(args);
    command_array.Insert(exepath, 0);
    m_session.command = { command_array.begin(), command_array.end() };
    m_session.debug_over_ssh = !ssh_account.empty();

    if (!m_session.debug_over_ssh) {
        // only add the working directory if it exists
        if (wxFileName::DirExists(working_directory)) {
            m_session.working_directory = working_directory;
        }
    } else {
        m_session.working_directory = working_directory;
    }
    m_session.environment = env;

#if USE_SFTP
    if (m_session.debug_over_ssh) {
        m_session.ssh_acount = SSHAccountInfo::LoadAccount(ssh_account);
        if (m_session.ssh_acount.GetAccountName().empty()) {
            LOG_ERROR(LOG) << "failed to load ssh account:" << ssh_account << endl;
            m_session.Clear();
            return false;
        }
    }
#endif

    return true;
}

void DebugAdapterClient::StartAndConnectToDapServer()
{
    m_client.Reset();
    m_dap_server.reset();

    LOG_DEBUG(LOG) << "Connecting to dap-server:" << m_session.dap_server.GetName() << endl;
    LOG_DEBUG(LOG) << "exepath:" << m_session.command << endl;
    LOG_DEBUG(LOG) << "working_directory:" << m_session.working_directory << endl;
    LOG_DEBUG(LOG) << "env:" << to_string_array(m_session.environment) << endl;

    dap::Transport* transport = nullptr;
    if (m_session.dap_server.GetConnectionString().CmpNoCase("stdio") == 0) {
        // start the dap server (for the current session)
        transport = StartStdioDap();
        if (transport == nullptr) {
            return;
        }
    } else {
        // start the dap server (for the current session)
        if (!StartSocketDap()) {
            LOG_WARNING(LOG) << "Failed to start dap server" << endl;
            return;
        }
        LOG_DEBUG(LOG) << "dap server started!" << endl;
        wxBusyCursor cursor;
        // Using socket transport
        auto socket_transport = new dap::SocketTransport();
        LOG_DEBUG(LOG) << "Connecting to dap server:" << m_session.dap_server.GetConnectionString() << endl;
        if (!socket_transport->Connect(m_session.dap_server.GetConnectionString().ToStdString(), 10)) {
            wxMessageBox("Failed to connect to DAP server using socket", DAP_MESSAGE_BOX_TITLE,
                         wxICON_ERROR | wxOK | wxCENTRE);
            wxDELETE(socket_transport);
            m_client.Reset();
            m_dap_server.reset();
            return;
        }
        LOG_DEBUG(LOG) << "Success" << endl;
        transport = socket_transport;
    }

    wxDELETE(m_breakpointsHelper);
    m_breakpointsHelper = new BreakpointsHelper(m_client, m_session, LOG);

    // Notify about debug start event
    // + load the UI
    InitializeUI();
    LoadPerspective();

    // Fire CodeLite IDE event indicating that a debug session started
    clDebugEvent cl_event{ wxEVT_DEBUG_STARTED };
    cl_event.SetDebuggerName(m_session.dap_server.GetName());
    EventNotifier::Get()->AddPendingEvent(cl_event);

    // construct new client with the transport
    m_client.SetTransport(transport);

    LOG_DEBUG(LOG) << "Sending Initialize request" << endl;
    // send protocol Initialize request
    dap::InitializeRequestArguments init_request_args;
    init_request_args.clientID = "CodeLite";
    init_request_args.linesStartAt1 = true;
    init_request_args.clientName = "CodeLite IDE";
    m_client.Initialize(&init_request_args);
}

void DebugAdapterClient::OnFileLoaded(clCommandEvent& event) { event.Skip(); }

bool DebugAdapterClient::IsDebuggerOwnedByPlugin(const wxString& name) const
{
    return m_dap_store.GetEntries().count(name) != 0;
}

void DebugAdapterClient::OnProcessOutput(clProcessEvent& event)
{
    event.Skip();
    if (m_dap_server && m_dap_server->IsRedirect()) {
        m_dap_server->Queue().Post(event.GetOutputRaw());
    }
}

void DebugAdapterClient::OnProcessTerminated(clProcessEvent& event)
{
    event.Skip();
    m_client.Reset();
    m_dap_server.reset();

    RestoreUI();
    LOG_DEBUG(LOG) << event.GetOutput() << endl;
    LOG_DEBUG(LOG) << "dap-server terminated" << endl;

    clDebugEvent e(wxEVT_DEBUG_ENDED);
    EventNotifier::Get()->AddPendingEvent(e);
}

void DebugAdapterClient::StopProcess()
{
    if (m_dap_server) {
        // wxEVT_DEBUG_ENDED is sent in OnProcessTerminated() handler
        LOG_DEBUG(LOG) << "Terminating dap-server..." << endl;
        m_dap_server->Terminate();

    } else {
        clDebugEvent e(wxEVT_DEBUG_ENDED);
        EventNotifier::Get()->AddPendingEvent(e);
    }
}

wxString DebugAdapterClient::NormaliseReceivedPath(const wxString& path) const
{
    wxFileName fn(path);
    if (m_session.debug_over_ssh) {
        if (fn.IsRelative()) {
            fn.MakeAbsolute(m_session.working_directory, wxPATH_UNIX);
        }
        return fn.GetFullPath(wxPATH_UNIX);
    } else {
        if (fn.IsRelative()) {
            fn.MakeAbsolute(m_session.working_directory);
        }
#ifdef __WXMSW__
        if (!fn.HasVolume()) {
            // try to fix path volume issue (lldb-vscode)
            fn.SetVolume("C");
        }
#endif
        wxString fullpath = fn.GetFullPath();
        return fullpath;
    }
}

void DebugAdapterClient::LoadFile(const dap::Source& sourceId, int line_number)
{
    if (sourceId.sourceReference <= 0 && !sourceId.path.empty()) {
        // use local file system
        // not a server file, load it locally
        wxFileName fp(sourceId.path);

        // the is already loaded
        wxString file_to_load = fp.GetFullPath();
        LOG_DEBUG(LOG) << "Loading file.." << file_to_load << endl;
        file_to_load = NormaliseReceivedPath(file_to_load);
        LOG_DEBUG(LOG) << "Normalised form:" << file_to_load << endl;

        if (m_session.debug_over_ssh) {
            clGetManager()->SetStatusMessage(_("ERROR: (dap) loading remote file over SSH is not supported yet"));
            return;
        } else {
            wxFileName fn(file_to_load);
            if (!fn.FileExists()) {
                clGetManager()->SetStatusMessage(_("ERROR: (dap) file:") + file_to_load + _(" does not exist"));
                return;
            }

            auto callback = [line_number](IEditor* editor) {
                DAPTextView::ClearMarker(editor->GetCtrl());
                DAPTextView::SetMarker(editor->GetCtrl(), line_number);
            };
            clGetManager()->OpenFileAndAsyncExecute(fn.GetFullPath(), callback);
            if (m_textView) {
                m_textView->ClearMarker();
            }
        }

    } else if (sourceId.sourceReference > 0) {
        // reference file, load it into the editor

        // easy path
        CHECK_PTR_RET(m_textView);
        if (m_textView->IsSame(sourceId)) {
            clGetManager()->SelectPage(m_textView);
            m_textView->SetMarker(line_number);
            return;
        }

        m_client.LoadSource(sourceId, [this, sourceId, line_number](bool success, const wxString& content,
                                                                    const wxString& mimeType) {
            if (!success) {
                return;
            }
            LOG_DEBUG(LOG) << "mimeType:" << mimeType << endl;
            clGetManager()->SelectPage(m_textView);
            m_textView->SetText(sourceId, content,
                                wxString() << sourceId.name << " (ref: " << sourceId.sourceReference << ")", mimeType);
            m_textView->SetMarker(line_number);
        });
    }
}

clEnvList_t DebugAdapterClient::PrepareEnvForFileSystemWorkspace(const DapEntry& dap_server, bool resolve_vars)
{
    clEnvList_t envlist = StringUtils::BuildEnvFromString(dap_server.GetEnvironment());
    if (clFileSystemWorkspace::Get().IsOpen()) {
        auto conf = clFileSystemWorkspace::Get().GetSettings().GetSelectedConfig();
        if (conf) {
            auto workspace_env = StringUtils::BuildEnvFromString(conf->GetEnvironment());
            envlist.insert(envlist.end(), workspace_env.begin(), workspace_env.end());
        }
    }

    if (resolve_vars) {
        EnvSetter setter; // apply global variables
        envlist = StringUtils::ResolveEnvList(envlist);
    }
    return envlist;
}

void DebugAdapterClient::OnIdle(wxIdleEvent& event)
{
    event.Skip();
    CHECK_PTR_RET(m_client.IsConnected());

    if (!m_client.CanInteract()) {
        ClearDebuggerMarker();
    }
}

void DebugAdapterClient::DestroyTooltip()
{
    CHECK_PTR_RET(m_tooltip);
    wxDELETE(m_tooltip);
}

void DebugAdapterClient::OnPageClosing(wxNotifyEvent& event)
{
    event.Skip();
    if (!m_client.IsConnected())
        return;
    // do not allow the user to close our text view control while debugging is active
    if (m_textView && m_textView == event.GetClientData()) {
        event.Veto();
    }
}

wxString DebugAdapterClient::ReplacePlaceholders(const wxString& str) const
{
    wxString project_name;
    if (clWorkspaceManager::Get().IsWorkspaceOpened()) {
        project_name = clWorkspaceManager::Get().GetWorkspace()->GetActiveProjectName();
    }

    wxString command = MacroManager::Instance()->Expand(str, clGetManager(), project_name);
    return command;
}

int DebugAdapterClient::GetCurrentFrameId() const
{
    if (!GetThreadsView()) {
        return wxNOT_FOUND;
    }
    return GetThreadsView()->GetCurrentFrameId();
}

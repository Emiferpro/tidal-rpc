/**
 * @file TidalRPC.cpp
 * @brief A lightweight Windows application that provides Discord Rich Presence for the TIDAL desktop app.
 *
 * This application runs in the system tray and monitors the Windows Global System Media Transport Controls (SMTC)
 * for media sessions originating from TIDAL. When a new track is detected, it fetches the metadata, uploads the
 * cover art to a temporary hosting service, and displays the information as a "Listening to" status in Discord.
 *
 * Core Technologies:
 * - C++/WinRT: For modern, standard C++ interaction with Windows Runtime APIs (specifically the SMTC).
 * - discord-partner-sdk: The official Discord SDK for Rich Presence integration.
 * - Win32 API: For creating the system tray icon, hidden window, and message loop.
 */

#include "pch.h"

#pragma comment(lib, "Shell32.lib")
#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "discord_partner_sdk.lib")
#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <iostream>
#include <io.h>
#include <fcntl.h>
#include <thread>
#include <string>
#include <functional>
#include <chrono>
#include <vector>
#include <fstream> 

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Web.Http.Filters.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Web::Http;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage::Streams;
using namespace Windows::Media::Control;
using namespace Windows::Web::Http::Headers;
using namespace Windows::Web::Http::Filters;

/**
 * @struct trackInfo
 * @brief Holds metadata for the currently playing media track.
 */
struct trackInfo {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring coverArtUrl; // Holds the public URL of the cover art
};

// Application constants

const std::string RELEASE_VER = "v0.2";
constexpr uint64_t APPLICATION_ID = 1429350918310072372;

NOTIFYICONDATAW g_notifyIconData{};
HWND            g_hWnd = nullptr;
HWND            g_hConsoleWnd = nullptr;

std::shared_ptr<discordpp::Client> client;
winrt::event_token                     g_mediaPropertiesChangedToken;
GlobalSystemMediaTransportControlsSessionManager g_sessionManager = nullptr;
GlobalSystemMediaTransportControlsSession        g_currentSession = nullptr;
trackInfo                                        g_lastTrackProcessed;
bool                                             g_isParsing = false;

struct ParsingGuard {
    ParsingGuard(bool& flag) : m_flag(flag) { m_flag = true; }
    ~ParsingGuard() { m_flag = false; }
private:
    bool& m_flag;
};


/**
 * @brief Converts a std::wstring (UTF-16) to a std::string (UTF-8).
 * @param wstr The wide string to convert.
 * @return The UTF-8 encoded string, suitable for use with libraries like the Discord SDK.
 */
std::string ws2s(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

/**
 * @brief Copies data from a WinRT IBuffer into a std::vector<byte>.
 * @param buffer The IBuffer to read from.
 * @return A std::vector<byte> containing the copied data.
 */
std::vector<byte> BufferToVector(IBuffer const& buffer)
{
    auto dataReader = DataReader::FromBuffer(buffer);
    std::vector<byte> bytes(buffer.Length());
    dataReader.ReadBytes(bytes);
    return bytes;
}


/**
 * @brief Callback function for logging messages from the Discord client.
 * @param message The log message.
 * @param severity The severity level of the message.
 */
void clientLogCallback(std::string message, discordpp::LoggingSeverity severity)
{
    std::cerr << "[Discord SDK][" << EnumToString(severity) << "] " << message << std::endl;
}
/**
 * @brief Clears the user's Rich Presence status in Discord.
 */
void clearPresence()
{
    client->ClearRichPresence();
}

/**
 * @brief Updates the Discord Rich Presence with the provided track information.
 * @param track The trackInfo struct containing the metadata to display.
 */
void updatePresence(const trackInfo& track)
{
    discordpp::Activity activity;
    activity.SetType(discordpp::ActivityTypes::Listening);
    activity.SetName(ws2s(track.artist));
    activity.SetDetails(ws2s(track.title));
    activity.SetState(ws2s(track.artist));

    discordpp::ActivityAssets assets;

    if (!track.coverArtUrl.empty()) {
        assets.SetLargeImage(ws2s(track.coverArtUrl));
		assets.SetLargeText("Playing on TIDAL");
    }

    assets.SetSmallImage("tidal-icon");
    assets.SetSmallText("tidal-rpc " + RELEASE_VER + " by @emiferpro");
    assets.SetSmallUrl("https://github.com/Emiferpro/tidal-rpc");
    activity.SetAssets(assets);

    client->UpdateRichPresence(activity, [](const discordpp::ClientResult& result) {
        if (result.Successful()) {
            std::cout << "Rich presence updated successfully." << std::endl;
        }
        else {
            std::cerr << "Failed to update rich presence: " << result.Error() << std::endl;
        }
        });
}



/**
 * @brief Uploads a raw binary image buffer by shelling out to curl.exe.
 * @param binaryData The std::vector<byte> containing the raw image data.
 * @return An awaitable operation that resolves to the public URL of the uploaded image, or an error string.
 */
IAsyncOperation<winrt::hstring> UploadCoverArtAsync(std::vector<byte> const& binaryData)
{
    wchar_t tempPath[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempPath) == 0) {
        co_return L"Error: Could not get temp path";
    }
    std::wstring tempFilePath = std::wstring(tempPath) + L"\\TIDALRPC_" + std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count()) + L".png";

    {
        std::ofstream tempFile(tempFilePath, std::ios::binary);
        if (!tempFile.is_open()) {
            co_return L"Error: Could not open temp file for writing";
        }
        tempFile.write(reinterpret_cast<const char*>(binaryData.data()), binaryData.size());
    }

    HANDLE hChildStd_OUT_Rd = NULL;
    HANDLE hChildStd_OUT_Wr = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hChildStd_OUT_Rd, &hChildStd_OUT_Wr, &sa, 0)) {
        DeleteFileW(tempFilePath.c_str());
        co_return L"Error: Could not create stdout pipe";
    }
    if (!SetHandleInformation(hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(hChildStd_OUT_Rd);
        CloseHandle(hChildStd_OUT_Wr);
        DeleteFileW(tempFilePath.c_str());
        co_return L"Error: Could not set handle information for pipe";
    }

    PROCESS_INFORMATION piProcInfo{};
    STARTUPINFOW siStartInfo{};
    siStartInfo.cb = sizeof(STARTUPINFOW);
    siStartInfo.hStdError = hChildStd_OUT_Wr;
    siStartInfo.hStdOutput = hChildStd_OUT_Wr;
    siStartInfo.dwFlags |= STARTF_USESTDHANDLES;

    auto expires_time = std::chrono::system_clock::now() + std::chrono::minutes(7);
    auto expires_ms = std::chrono::duration_cast<std::chrono::milliseconds>(expires_time.time_since_epoch()).count();
    std::wstring command = L"curl.exe -s -F \"file=@" + tempFilePath + L"\" -F \"expires=" + std::to_wstring(expires_ms) + L"\" http://0x0.st";

    BOOL bSuccess = CreateProcessW(NULL,
        &command[0],
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &siStartInfo,
        &piProcInfo);

    CloseHandle(hChildStd_OUT_Wr);

    std::string output;
    if (bSuccess) {
        CHAR chBuf[4096];
        DWORD dwRead;
        while (ReadFile(hChildStd_OUT_Rd, chBuf, 4096, &dwRead, NULL) && dwRead != 0) {
            output.append(chBuf, dwRead);
        }

        WaitForSingleObject(piProcInfo.hProcess, INFINITE);
        CloseHandle(piProcInfo.hProcess);
        CloseHandle(piProcInfo.hThread);
    }
    else {
        output = "Error: CreateProcess failed with code " + std::to_string(GetLastError());
    }

    CloseHandle(hChildStd_OUT_Rd);
    DeleteFileW(tempFilePath.c_str());

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &output[0], (int)output.size(), NULL, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &output[0], (int)output.size(), &wstr[0], size_needed);

    if (!wstr.empty() && (wstr.back() == L'\n' || wstr.back() == L'\r')) {
        wstr.pop_back();
    }
    if (!wstr.empty() && (wstr.back() == L'\n' || wstr.back() == L'\r')) {
        wstr.pop_back();
    }

    co_return winrt::hstring(wstr);
}


IAsyncAction parseTrack(bool force = false);

/**
 * @brief Fetches media properties from the current SMTC session, uploads cover art, and updates Discord presence.
 *
 * This is the central logic function. It checks if the active media session is TIDAL.
 * If it is, it extracts track metadata, gets the thumbnail, uploads it directly using
 * UploadCoverArtAsync, and then calls updatePresence. If the session is not TIDAL
 * or is invalid, it calls clearPresence.
 * @return An awaitable fire-and-forget async action.
 */
IAsyncAction parseTrack(bool force)
{
    // *** FIX: Prevent concurrent executions with a flag ***
    if (!force && g_isParsing) {
        std::cout << "Already processing a track, ignoring concurrent event." << std::endl;
        co_return;
    }
    ParsingGuard guard(g_isParsing);

    if (g_currentSession)
    {
        try {
            auto appId = g_currentSession.SourceAppUserModelId();
            std::wstring_view appIdView(appId.c_str(), appId.size());
            if (appIdView.find(L"TIDAL") != std::wstring::npos)
            {
                auto mediaProperties = co_await g_currentSession.TryGetMediaPropertiesAsync();

                trackInfo track;
                track.title = mediaProperties.Title().c_str();
                track.artist = mediaProperties.Artist().c_str();
                track.album = mediaProperties.AlbumTitle().c_str();
                track.coverArtUrl = L"";

                // *** CACHE CHECK to prevent duplicate processing from spammy events ***
                if (!force && track.title == g_lastTrackProcessed.title && track.artist == g_lastTrackProcessed.artist)
                {
                    std::cout << "Duplicate event for '" << ws2s(track.title) << "' ignored." << std::endl;
                    co_return;
                }

                auto thumbnail = mediaProperties.Thumbnail();
                if (thumbnail)
                {
                    auto stream = co_await thumbnail.OpenReadAsync();
                    if (stream && stream.Size() > 0)
                    {
                        InMemoryRandomAccessStream memoryStream;
                        co_await RandomAccessStream::CopyAsync(stream, memoryStream);

                        if (memoryStream.Size() > 0)
                        {
                            memoryStream.Seek(0);
                            auto dataReader = DataReader{ memoryStream.GetInputStreamAt(0) };
                            unsigned int numBytesLoaded = co_await dataReader.LoadAsync(static_cast<unsigned int>(memoryStream.Size()));

                            if (numBytesLoaded > 0)
                            {
                                auto buffer = dataReader.ReadBuffer(numBytesLoaded);
                                if (buffer.Length() > 0)
                                {
                                    std::vector<byte> stableData = BufferToVector(buffer);
                                    if (stableData.size() > 0)
                                    {
                                        std::cout << "Found cover art for '" << ws2s(track.title) << "'. Uploading..." << std::endl;
                                        winrt::hstring uploadedUrl = co_await UploadCoverArtAsync(stableData);

                                        if (!uploadedUrl.empty()) {
                                            std::wstring_view urlView(uploadedUrl.c_str(), uploadedUrl.size());
                                            if (urlView.find(L"Error:") == std::wstring::npos && urlView.find(L"Exception:") == std::wstring::npos) {
                                                track.coverArtUrl = uploadedUrl.c_str();
                                                std::cout << "Upload successful: " << ws2s(track.coverArtUrl) << std::endl;
                                            }
                                            else {
                                                std::cerr << "Failed to upload cover art: " << ws2s(uploadedUrl.c_str()) << std::endl;
                                            }
                                        }
                                    }
                                    else { std::cerr << "BufferToVector created a 0-size vector. Aborting upload." << std::endl; }
                                }
                                else { std::cerr << "ReadBuffer created a 0-length buffer. Aborting upload." << std::endl; }
                            }
                            else { std::cout << "Cover art stream for '" << ws2s(track.title) << "' was empty (0 bytes loaded)." << std::endl; }
                        }
                        else { std::cout << "Cover art stream for '" << ws2s(track.title) << "' was empty (memoryStream size 0)." << std::endl; }
                    }
                }
                else
                {
                    std::cout << "No cover art found for '" << ws2s(track.title) << "'." << std::endl;
                }

                updatePresence(track);

                // *** UPDATE CACHE with the newly processed track ***
                g_lastTrackProcessed = track;
            }
            else
            {
                std::cout << "No active TIDAL session found. Clearing presence." << std::endl;
                clearPresence();
                g_lastTrackProcessed = {};
            }
        }
        catch (winrt::hresult_error const& ex)
        {
            std::cerr << "Failed to parse track info: "
                << ws2s(ex.message().c_str()) << std::endl;
            g_lastTrackProcessed = {};
        }
    }
    else
    {
        std::cout << "No active media session found. Clearing presence." << std::endl;
        clearPresence();
        g_lastTrackProcessed = {};
    }
    co_return;
}


/**
 * @brief Creates and attaches an event handler for media property changes on the current session.
 */
void RegisterMediaPropertiesChangedHandler()
{
    if (g_currentSession) {
        std::cout << "Registering MediaPropertiesChanged event handler." << std::endl;
        g_mediaPropertiesChangedToken = g_currentSession.MediaPropertiesChanged([](auto&&, auto&&) -> fire_and_forget
            {
                co_await std::chrono::milliseconds(300);

                std::cout << "Media properties changed. Reparsing track..." << std::endl;
                co_await parseTrack();
            });
    }
}

/**
 * @brief Creates a debug console window and redirects standard I/O streams to it.
 */
void CreateDebugConsole()
{
    if (AllocConsole())
    {
        FILE* pFile = nullptr;
        freopen_s(&pFile, "CONOUT$", "w", stdout);
        freopen_s(&pFile, "CONOUT$", "w", stderr);
        freopen_s(&pFile, "CONIN$", "r", stdin);

        std::cout.clear();
        std::cerr.clear();
        std::cin.clear();

        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        g_hConsoleWnd = GetConsoleWindow();
        if (g_hConsoleWnd) {
            ShowWindow(g_hConsoleWnd, SW_HIDE);
        }
        std::cout << "Debug Console Initialized." << std::endl;
    }
}

/**
 * @brief Adds the application icon to the system tray.
 * @param hInstance The application instance handle.
 * @param hwnd The handle of the window to receive callback messages.
 */
void AddTrayIcon(HINSTANCE hInstance, HWND hwnd)
{
    g_notifyIconData.cbSize = sizeof(NOTIFYICONDATAW);
    g_notifyIconData.hWnd = hwnd;
    g_notifyIconData.uID = 1;
    g_notifyIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_notifyIconData.uCallbackMessage = WM_APP + 1;
    g_notifyIconData.hIcon = LoadIcon(nullptr, IDI_APPLICATION); // TODO: Replace with a custom icon resource
    wcscpy_s(g_notifyIconData.szTip, L"TIDAL Rich Presence");
    Shell_NotifyIconW(NIM_ADD, &g_notifyIconData);
}

/**
 * @brief Displays the right-click context menu for the tray icon.
 * @param hwnd The parent window handle.
 */
void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, -1, MF_BYPOSITION, 1, L"Force Update");
    InsertMenuW(hMenu, -1, MF_BYPOSITION, 3, L"Show/Hide Console");
    InsertMenuW(hMenu, -1, MF_BYPOSITION, 2, L"Exit");

    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(
        hMenu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD,
        pt.x, pt.y, 0, hwnd, nullptr);

    DestroyMenu(hMenu);

    switch (cmd)
    {
    case 1:
        parseTrack(true);
        break;
    case 2:
        Shell_NotifyIconW(NIM_DELETE, &g_notifyIconData);
        PostQuitMessage(0);
        break;
    case 3:
        if (g_hConsoleWnd) {
            bool isVisible = IsWindowVisible(g_hConsoleWnd);
            ShowWindow(g_hConsoleWnd, isVisible ? SW_HIDE : SW_SHOW);
            if (!isVisible) {
                SetForegroundWindow(g_hConsoleWnd);
            }
        }
        break;
    }
}

/**
 * @brief The window procedure for the application's hidden message-only window.
 * @param hwnd Handle to the window.
 * @param msg The message.
 * @param wParam Additional message information.
 * @param lParam Additional message information.
 * @return The result of the message processing.
 */
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_APP + 1:
        if (lParam == WM_RBUTTONUP)
            ShowTrayMenu(hwnd);
        else if (lParam == WM_LBUTTONDBLCLK)
            MessageBoxW(hwnd, L"TIDAL Rich Presence is running.", L"TIDAL RPC", MB_OK);
        break;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_notifyIconData);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}



int __stdcall wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    init_apartment();
    CreateDebugConsole();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MyTrayWindow";
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"TIDAL RPC Hidden Window", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!g_hWnd) return -1;

    AddTrayIcon(hInstance, g_hWnd);

    client = std::make_shared<discordpp::Client>();
    client->SetApplicationId(APPLICATION_ID);
    client->AddLogCallback(clientLogCallback, discordpp::LoggingSeverity::Info);

    try {
        g_sessionManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        g_sessionManager.CurrentSessionChanged([](auto&&, auto&&) -> fire_and_forget
            {
                std::cout << "Current media session changed." << std::endl;
                if (g_currentSession) {
                    g_currentSession.MediaPropertiesChanged(g_mediaPropertiesChangedToken);
                }

                g_lastTrackProcessed = {};
                g_currentSession = g_sessionManager.GetCurrentSession();
                RegisterMediaPropertiesChangedHandler();

                co_await std::chrono::milliseconds(300);

                co_await parseTrack();
            });

        g_currentSession = g_sessionManager.GetCurrentSession();
        if (g_currentSession) {
            RegisterMediaPropertiesChangedHandler();
            std::cout << "Performing initial track analysis..." << std::endl;
            parseTrack().get();
        }
        else {
            std::cout << "No active media session on startup. Waiting for changes." << std::endl;
        }
    }
    catch (const winrt::hresult_error& ex) {
        std::wstring errorMessage = L"FATAL: WinRT initialization failed: " + std::wstring(ex.message());
        std::cerr << ws2s(errorMessage) << std::endl;
        MessageBoxW(nullptr, errorMessage.c_str(), L"TIDAL RPC Error", MB_OK | MB_ICONERROR);
        return -1;
    }


    bool bIsRunning = true;
    MSG msg;
    while (bIsRunning)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                bIsRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!bIsRunning) break;

        // CRITICAL: Run Discord SDK callbacks on every loop iteration.
        discordpp::RunCallbacks();

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    clearPresence();
    if (g_hConsoleWnd) {
        FreeConsole();
    }

    return (int)msg.wParam;
}
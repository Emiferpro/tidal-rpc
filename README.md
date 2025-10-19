# TIDAL Rich Presence for Discord

A lightweight, native Windows C++ application that provides Discord Rich Presence for the TIDAL desktop app.

It runs in the system tray, monitors your media activity using the Windows System Media Transport Controls (SMTC), and automatically updates your Discord status to "Listening to..." with the current track, artist, and cover art.



## Features

* **Real-time Status:** Updates your Discord presence in real-time as you listen to music on TIDAL.
* **Full Metadata:** Displays song title, artist, and album.
* **Cover Art:** Fetches the track's cover art, uploads it to a temporary host, and displays it directly in your Discord status.
* **System Tray Icon:** Runs quietly in the system tray with a context menu for:
    * Forcing a presence update.
    * Showing/hiding a debug console.
    * Exiting the application.
* **Lightweight:** Native C++/WinRT and Win32 application with no heavy frameworks or dependencies (aside from `curl`).

---

## How It Works

This application uses a combination of modern and classic Windows APIs:

1.  **SMTC Monitoring (C++/WinRT):** It hooks into the `GlobalSystemMediaTransportControlsSessionManager` to monitor all media activity on the system.
2.  **Session Filtering:** It checks the `SourceAppUserModelId` of the current media session to see if it originates from TIDAL.
3.  **Metadata Fetching:** When a TIDAL session is active and a track changes, it asynchronously fetches the `MediaProperties` (title, artist, album, and thumbnail).
4.  **Cover Art Upload:** Discord Rich Presence requires a public URL for images. To solve this:
    * The application reads the thumbnail `IRandomAccessStream` into a memory buffer.
    * It writes this buffer to a temporary `.png` file.
    * It then **shells out to `curl.exe`** to upload this file to the `http://0x0.st` temporary file hosting service with a 7-minute expiration.
    * The public URL returned by `0x0.st` is used for the Rich Presence art.
5.  **Discord Integration (Discord SDK):** It uses the official Discord Partner SDK to set the `Activity` status (Listening to...), populating it with all the fetched metadata and the cover art URL.
6.  **UI (Win32):** The application runs as a hidden, message-only window with a `NOTIFYICONDATA` system tray icon, which serves as the main user interface.

---

## Prerequisites

### For Building

* **Visual Studio 2022** (or 2019) with the **"Desktop development with C++"** workload.
* **Windows SDK** (latest version, usually included with the VS workload).
* **C++/WinRT** (usually included with the VS workload).
* **Discord Partner SDK:** You must [download the SDK](https://discord.com/developers/docs/game-sdk/sdk-starter-guide) from the Discord Developer Portal.

### For Running

* **`curl.exe`:** The application **requires `curl.exe` to be in your system's `PATH`** to upload cover art. Windows 10 and 11 include `curl` by default, so this should not be an issue on modern systems.

---

## Setup & Configuration

Before you can build and run this, you should create a Discord Application, or use the one included (idk if im going to get banned from discord for this).

1.  **Create a Discord Application:**
    * Go to the [Discord Developer Portal](https://discord.com/developers/applications).
    * Click **"New Application"** and give it a name (e.g., "TIDAL RPC").
    * On the "General Information" page, copy the **Application ID**.

2.  **Update the Source Code:**
    * Open `TidalRPC.cpp`.
    * Find the `APPLICATION_ID` constant and replace the placeholder value with your own Application ID:
        ```cpp
        // --- Application Constants ---
        constexpr uint64_t APPLICATION_ID = YOUR_APPLICATION_ID_HERE; // e.g., 1429350918310072372
        ```

3.  **Add Rich Presence Assets (Optional but Recommended):**
    * In the Developer Portal, go to the **"Rich Presence"** tab.
    * Under "Rich Presence Assets," upload an image to use as the small icon (e.g., a TIDAL logo).
    * Name the asset `tidal-icon`. The code refers to this key (`assets.SetSmallImage("tidal-icon");`). If you name it something else, update the code to match.

---

## Building

This project is intended to be built with Visual Studio.

1.  Clone this repository.
2.  Download and extract the **Discord Partner SDK** to a known location (e.g., `C:\SDKs\discord_partner_sdk`).
3.  Open the `.sln` file in Visual Studio.
4.  Configure your project properties to point to the SDK:
    * Right-click the project in Solution Explorer -> **Properties**.
    * **C/C++ -> General -> Additional Include Directories:** Add the path to the Discord SDK's `cpp` directory (e.g., `C:\SDKs\discord_partner_sdk\cpp`).
    * **Linker -> General -> Additional Library Directories:** Add the path to the Discord SDK's library directory (e.g., `C:\SDKs\discord_partner_sdk\lib\x86_64`).
5.  Build the project (e.g., in `Release` mode, `x64`).
6.  Find the compiled `.exe` in your `x64\Release` folder and run it.

---

## Limitations

* **`curl.exe` Dependency:** The application will fail to upload (and therefore display) cover art if `curl.exe` is not found in your system's `PATH`.
* **`0x0.st` Host:** Cover art is uploaded to a public, temporary hosting service. If this service is down or blocks a request, cover art will not appear.
* **Windows Only:** This is a Windows-native application using WinRT and Win32 APIs. It will not run on macOS or Linux.

# User Handbook - Troubleshooting

## Vulkan initialization error

- Ensure you have the latest drivers
- Uninstall old drivers, for Windows you can use [Display Driver Uninstaller](https://www.guru3d.com/download/display-driver-uninstaller-download/)
- Change the backend manually in the settings file: open `qt-config.ini` in the Nemyu configuration folder (`%APPDATA%\nemyu\config` on Windows, `~/.config/nemyu` or `$XDG_CONFIG_HOME/nemyu` on Linux and macOS, `user/config` in a portable install; see [configuration directories](./Basics.md#configuration-directories)) and, under `[Renderer]`, set `backend\default=false` and `backend=0` for OpenGL (or `backend=1` for Vulkan).
- Disconnect your second monitor, if any

## This mod only works on an Emulator

- Enable RNG seed
- Set RNG seed to 0

## Nemyu Fails to Launch and Does Not Leave Any Logs

An illustrated version of this guide, written for Eden, is available [here](https://evilperson1337.notion.site/Windows-Eden-Fails-to-Launch-and-Does-Not-Leave-Any-Logs-2b057c2edaf68156b640cf1ac549870a).

### Error Details

- *Behavior*: Program appears not to launch or exits immediately without leaving any log entries.
- *Platform(s) Affected*: **Windows**

**Error Log Entries:** None.

**Example Error Message Entry in Windows Event Viewer**

```text
Faulting application name: nemyu.exe, version: 0.0.0.0, time stamp: 0x6795dc3c
Faulting module name: ntdll.dll, version: 10.0.26100.3037, time stamp: 0x95e6c489
Exception code: 0xc0000005
Fault offset: 0x0000000000014778
Faulting process id: 0x2AF0
Faulting application start time: 0x1DB7C30D2972402
Faulting application path: C:\temp\nemyu\nemyu.exe
Faulting module path: C:\WINDOWS\SYSTEM32\ntdll.dll
Report Id: 4c8a6e13-9637-438c-b4d0-e802d279af66
Faulting package full name:
Faulting package-relative application ID:
```

---

### Causes

<aside>

#### Issue 1: Missing C++ Redistributable

*Nemyu needs the Microsoft Visual C++ runtime in order to run. The Windows release zip ships it next to `nemyu.exe`; if those files are missing or damaged, or you are running a build that does not include them, the program cannot run until you install the redistributable.*

1. Download the [Latest C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170#visual-studio-2015-2017-2019-and-2022) from Microsoft for your machine.
2. Double Click the downloaded executable file and wait for the software to install.
3. Restart the computer.
4. Launch Nemyu again, if the main window appears, you are good to go. If not, proceed to the next issue.
</aside>

<aside>

#### Issue 2: Corrupted System Files

*A corruption of necessary system files can cause odd behaviors when Nemyu tries to access them. It is a very rare case and you would likely see other programs misbehaving if this is what your issue is, but you can try if you have no other options.*

1. Launch Nemyu to generate a crash.
2. Confirm there are no logs created in the log directory.
    1. See the [How to Access Logs](./HowToAccessLogs.md) page for the log location if you need it.
    2. If there are any entries in here since you tried step 1, this is likely not your issue.
3. Navigate to your *Windows Event Viewer* (Start Menu > **eventvwr.msc**).
4. Expand **Windows Logs** and select **Application**.
5. Look for an entry with the Level of Error, and look for a message similar to the following

    ```text
    Faulting application name: nemyu.exe, version: 0.0.0.0, time stamp: 0x6795dc3c
    Faulting module name: ntdll.dll, version: 10.0.26100.3037, time stamp: 0x95e6c489
    Exception code: 0xc0000005
    Fault offset: 0x0000000000014778
    Faulting process id: 0x2AF0
    Faulting application start time: 0x1DB7C30D2972402
    Faulting application path: C:\temp\nemyu\nemyu.exe
    Faulting module path: C:\WINDOWS\SYSTEM32\ntdll.dll
    Report Id: 4c8a6e13-9637-438c-b4d0-e802d279af66
    Faulting package full name:
    Faulting package-relative application ID:
    ```

6. Run a Command Prompt terminal Window as Administrator.
7. Enter the following command and wait for it to complete. It will take a while, just be patient and do other things while it completes.

    ```bat
    DISM /Online /Cleanup-Image /RestoreHealth
    ```

8. Reboot your computer.
9. Launch Nemyu and verify it is now working.
</aside>

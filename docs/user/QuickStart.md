# Nemyu Quick Start

Use this guide to get started with Nemyu.

## Windows

### Pre-Requisites

- A PC running 64-bit Windows on an x86-64 (Intel or AMD) CPU
- Firmware dumped from your console
- Keys extracted from your console
- Games dumped from your console
- Internet connection

### Steps

1. Download the latest Windows zip from the [Releases page](https://gitlab.com/nemyu-emulator/nemyu/-/releases). It already contains Qt and the Visual C++ runtime, so there is nothing else to install.
    <aside>

    ***TIP***: If you have questions about the requirements, architectures, or general information about the releases, see the [*Basics Guide*](./Basics.md) and [*Architectures Guide*](./Architectures.md).

    </aside>
2. Extract the contents to wherever you want to store the program on your computer.
    <aside>

    ***TIP***: If you want to run Nemyu completely portable (everything is stored in the folder you extracted it to), create a folder called **user** next to **nemyu.exe**.

    </aside>
3. Run ***nemyu.exe*** to launch the program.

    <aside>

    ***INFO***: You may get a "*Windows protected your PC*" SmartScreen message. This means Windows did not recognize the application and did not run it. If you downloaded the zip from the Releases page above, click **More info** and then **Run anyway** to dismiss this message.

    </aside>
4. Nemyu does not ask for decryption keys at startup, so install them now: navigate to **Tools > Install Decryption Keys**, go to the folder containing your key files and select your ***prod.keys*** file (you can only select one).
5. Navigate to **Tools > Install Firmware** and select **From Folder** or **From ZIP**, depending on how your firmware is stored, then navigate to where it is stored and select it.
6. Double-click the main window to add the folder containing your games.
7. Go to **Emulation > Configure... > Controls** and set up your controller of choice. Click **OK** to close the dialog window.
8. Double-click a game to run it.

## Steam Deck

### Pre-Requisites

- A Linux build of Nemyu. There are no prebuilt Nemyu releases for Linux yet, so build it from source by following the [build guide](../Build.md).
- Firmware dumped from your console
- Keys extracted from your console
- Games dumped from your console
- Internet connection

### Steps

1. Access Steam Desktop Mode.
2. Put a Nemyu build that runs on SteamOS somewhere accessible on your Steam Deck.
3. Double-click the `nemyu` executable to launch the program.
4. If you have had a different Switch emulator installed, Nemyu will detect it and ask if you want to import its data. Make your selection to close the screen.
5. Nemyu does not ask for decryption keys at startup, so install them now: navigate to **Tools > Install Decryption Keys**, go to the folder containing your ***prod.keys*** file, select the file and hit **Open**.
6. Navigate to **Tools > Install Firmware** and select **From Folder** or **From ZIP**, depending on how your firmware is stored, then navigate to where it is stored and select it.
7. Double-click the main window to add the folder containing your games.
8. Go to **Emulation > Configure... > Controls** and set up your controller. Click **OK** to close the dialog window.
9. Double-click a game to run it.

## macOS

There are no prebuilt Nemyu releases for macOS. Build the `nemyu.app` bundle from source by following the [build guide](../Build.md).

Current macOS support is still experimental and very reliant on MoltenVK developments, plans have shifted to properly provide support for KosmicKrisp and similar new GPU endeavors, but macOS users still are bound to MoltenVK itself.

Users of macOS may wish to use [Asahi Linux](https://wiki.gentoo.org/wiki/Project:Asahi/Guide) for the rising KosmicKrisp support.

As of writing, neither macOS nor Asahi has support for NCE; additionally Asahi has extraneous paging bugs with fastmem.

### Allowing Nemyu to Run on macOS

Use this guide when you need to allow Nemyu to run on a Mac system, but are being blocked by Apple Security policy.

An illustrated version of this guide, written for Eden, is available [here](https://evilperson1337.notion.site/Allowing-Eden-to-Run-on-MacOS-2b057c2edaf681fea63dc81027efeffd).

---

#### Pre-Requisites

- Permissions to modify settings in macOS

---

#### Why am I Seeing This?

Recent versions of macOS (Catalina & newer) introduced the **Gatekeeper** security functionality, requiring software to be signed by Apple or a trusted (aka - paying) developer. If the signature isn't on the list of trusted ones, it will stop the program from executing and display a warning instead.

---

#### Steps

1. Open the *System Settings* panel.
2. Navigate to *Privacy & Security*.
3. Scroll down to the **Security** settings, where macOS shows a message saying Nemyu was blocked.
4. Select **Open Anyway** to tell your Mac that you trust the application.
5. You will now get another window appearing to verify you want to open Nemyu. Select **Open Anyway**.
6. You will be prompted for your password to authorize the request. Enter the credentials of an account that has permission to modify settings and press **OK**.
7. Nemyu will now open and any subsequent launches of the program will not prompt this.

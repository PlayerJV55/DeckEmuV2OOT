# How to Access Logs

Use this when you need to review the logs to determine an issue, or to attach them to a bug report on the [issue tracker](https://gitlab.com/nemyu-emulator/nemyu/-/issues). The log of the most recent session is `nemyu_log.txt` in the log folder. Starting Nemyu again renames it to `nemyu_log.txt.old.txt`, so if you reopened Nemyu to get to the folder, attach `nemyu_log.txt.old.txt` instead. Builds before this change named the log `eden_log.txt`, so a file with that name in the folder is from one of them.

An illustrated version of this guide, written for Eden, is available [here](https://evilperson1337.notion.site/How-to-Access-Logs-2b057c2edaf68105a281fe1688a332d4).

---

## Pre-Requisites

- Nemyu installed and run at least once

---

## Steps

### Windows

*By default the logs are in the `log` subfolder of the Nemyu data folder, `C:\Users\<USER>\AppData\Roaming\nemyu\log`. For a portable installation, look in `user\log` next to `nemyu.exe` instead.*

<aside>

1. Open Nemyu.
2. Navigate to *File > Open Nemyu Folders > Log Folder*, a Windows explorer window will appear.
3. Your log files are now here for your review or upload.
</aside>

### Linux, macOS and Steam Deck

*By default the logs are in `~/.local/share/nemyu/log` (`$XDG_DATA_HOME/nemyu/log`), which is `/home/deck/.local/share/nemyu/log` on a Steam Deck. For a portable installation, look in `user/log` in the directory you start Nemyu from instead.*

<aside>

1. Open Nemyu.
2. Navigate to *File > Open Nemyu Folders > Log Folder*, a file manager window (Dolphin on the Steam Deck) will appear.
3. Your log files are now here for your review or upload.
</aside>

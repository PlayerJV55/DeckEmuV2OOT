# User Handbook - Using Amiibo

Use this guide when you want to load Amiibo into your games for use with Nemyu.

An illustrated version of this guide, written for Eden, is available [here](https://evilperson1337.notion.site/Using-Amiibo-with-Eden-2b057c2edaf681b1b28ec6be600c6d3e).

## Desktop

### Pre-Requisites

1. Nemyu fully set up and configured.
2. The Amiibo file you want to use.

    <aside>

    ***NOTE***: Nemyu only supports the *.bin* Amiibo format, ***NOT*** the *.nfc* format. Both the usual 540-byte dumps and the 2048-byte dumps of the newer "v3" Amiibo (Kirby Air Riders onward) are accepted. Data a game keeps in the extra storage of v3 Amiibo is not supported yet.

    </aside>

### Steps

1. Launch Nemyu and launch the game you want to load Amiibo for.
    <aside>

    ***TIP***: You must be in-game before you can emulate scanning an Amiibo

    </aside>

1. Navigate to the Amiibo section of the game.  The method for initiating the scanning varies from game to game, for *Captain Toad's Treasure Tracker*, you need to go to the press the **+** button when on the level select.  You will need to look up how to do so with your specific game.
2. Upon activating the Amiibo scan functionality, you should get a Scan page.  Nemyu is now looking for an Amiibo file to be loaded, which emulates scanning an Amiibo on actual hardware.
3. Navigate to **File > Load/Remove Amiibo...**, or press the hotkey to do the same (**F2** on keyboard by default).
4. In the file explorer that opens, navigate to the amiibo file you want to use. The explorer opens in the folder you last loaded an amiibo from. You can also drag the *.bin* file onto the Nemyu window while the game runs.

    <aside>

    ***NOTE***: If the game is not scanning at that moment, Nemyu keeps the amiibo "on the reader" and scans it as soon as the game asks for one, like leaving a figure on the console. Using **Load/Remove Amiibo...** again takes it off.

    </aside>

5. Upon loading a valid file, you will get a confirmation screen and your bonus content will be unlocked/functionality activated.
6. Repeat with any other Amiibo you want to use.

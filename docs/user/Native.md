# User Handbook - Native Application Development

Debugging on physical hardware can get tedious and time consuming. Users are empowered with the debugging capabilities of the emulator to ensure their applications run as-is on the system. To the greatest extent possible at least.

## Debugging

**Use dev.keys**: Makes the key manager load `dev.keys` instead of `prod.keys`, which is useful for testing with a development keyset. Off by default.

**Changing serial**: Very basic way to set debug values for the serial (and battery number). Developers do not need to write the full serial as only the first digits (excluding the last) will be accounted for. Region settings will affect the generated serial. The serial corresponds to a non-OLED/Lite console.

# Rogue 5.4.4

Source: https://github.com/Davidslv/rogue at f4653c2a2ee6981a73abe9dfda055134285e1e79
(the Roguelike Restoration Project's 5.4.4). License: BSD 3-clause, see LICENSE.TXT.

Every file here is unmodified. Not vendored, and replaced by `port/`:

| Upstream file | Why it is out | Replacement |
|---|---|---|
| main.c | Unix startup, signals, shell | port/rogue_port.c |
| mach_dep.c, mdport.c | terminal, users, score file locking | port/rogue_port.c |
| save.c, state.c, xcrypt.c | save files and their encryption | stubs in port/rogue_port.c |

`port/include` supplies the C library and curses headers the sources include,
`port/libc.c` and `port/curses.c` implement them.

#pragma once
// Line protocol to the PC bridge over USB serial. See PROTOCOL.md.

void link_begin();
void link_poll();                          // call from loop(): reads and dispatches lines
void link_sendf(const char *fmt, ...);     // one line, '\n' appended

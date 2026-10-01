#pragma once
// Assembles the album cover from the bridge's base64 chunks (see PROTOCOL.md, message A).
#include <stdint.h>

void cover_begin();  // allocates the buffer in SDRAM; call after display_begin()
// Stores chunk `index` of cover `id`. True when the cover is complete: after its last missing
// chunk, and again at the end of a resend. Then cover_pixels() holds it (COVER_PX x COVER_PX
// RGB565) until the first chunk of another cover arrives.
bool cover_chunk(uint32_t id, int index, const char *b64);
const uint8_t *cover_pixels();

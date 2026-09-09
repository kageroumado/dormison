/*
 * Provenance lines for a game process: which engine, renderer and toolkit
 * answered, and whether a frame ever reached the screen.
 *
 * Copyright 2026 Sevoflurane
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_SEVO_PROVENANCE_H
#define __WINE_SEVO_PROVENANCE_H

/* The three `sevo:run`/`sevo:gfx` header lines, and the exit line that
   follows them when the process leaves through exit(). */
extern void sevo_provenance_init(int presenter_on, const char *upscaler);

/* One presented frame, from the client surface funcs table. The first call
   prints `first present`; every call counts. */
extern void sevo_provenance_note_present(const void *surface);

/* D3DMetal's own present hook: a CLIENT_SURFACE_PRESENTED posted from
   nextDrawable, and one executed by the driver's event handler. The pair
   is printed under SEVO_GFX_LOG=1. */
extern void sevo_provenance_note_drawable(void);
extern void sevo_provenance_note_presented_event(void);

#endif  /* __WINE_SEVO_PROVENANCE_H */

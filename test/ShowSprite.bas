REM > ShowSprite
REM M3: display the V3D-rendered sprite in a Wimp window.
REM
REM Companion to the V3DDriver module, not part of it - a kernel
REM module can't easily run its own Wimp event loop, so the module's
REM job stops at "hand back a valid sprite" (SYS "V3D_ClearToSprite")
REM and this ordinary BASIC program does the Wimp/display side, using
REM only well-documented, decades-stable RISC OS APIs (Window Block:
REM riscosopen.org/wiki/documentation/show/Window%20Block, Window
REM Flags, Icon Flags, Rectangle Data, OS_SpriteOp - all verified
REM against ROOL's own PRM pages, not written from memory).
REM
REM Before running: *RMLoad the built V3DDriver,ffa module yourself.
REM This program does not load it for you.
REM
REM Run this by double-clicking it in the Filer (or LOAD+RUN from a
REM BASIC session started that way), NOT from inside a Task Window.
REM This is a real Wimp application (calls Wimp_Initialise/Wimp_Poll),
REM and Task Windows are specifically for programs that don't - running
REM one there fails with "Window Manager is currently in use" (a real
REM RISC OS Wimp/TaskWindow veto, confirmed against ROOL's own source
REM and docs - not a bug in this script).
REM
REM The sprite's logical address returned by V3D_ClearToSprite is
REM directly usable here, in a completely separate task, without any
REM copy - it's a PMP dynamic area pointer (see cl_clear.h), which
REM lives in RISC OS's shared logical address space rather than this
REM task's own Application Space.

task% = 0 : REM must exist before ON ERROR can safely reference it -
            REM see PROCerror: a first version left this unset, and an
            REM early error (before Wimp_Initialise assigns task%) sent
            REM the handler into infinite recursion (referencing task%
            REM inside a handler for "undefined variable" is itself an
            REM "undefined variable" error), flooding the screen with
            REM the SAME message and hiding whatever the real original
            REM error was.
ON ERROR PROCerror : END

REM --- power on and get a sprite from the GPU ---
SYS "V3D_PowerOn"
SYS "V3D_ClearToSprite", &FF00FFFF TO sprite%, readback%, physaddr%
PRINT "Sprite area: "; ~sprite%; "  physical: "; ~physaddr%; "  readback: "; ~readback%
IF readback% <> &FF00FFFF THEN ERROR 1, "Pixel readback didn't match what was requested - GPU output looks wrong"

W% = 64 : REM sprite width in OS units - OS_SpriteOp 34/290 does no scaling,
H% = 64 : REM 1 sprite pixel = 1 OS unit exactly, regardless of sprite DPI

REM --- register with the Wimp ---
SYS "Wimp_Initialise", 310, &4B534154, "V3D sprite test" TO wimpversion%, task%

REM --- build the window definition block (see Window Block doc for
REM     every offset below) ---
DIM wb% 200
wb%!0  = 200          : REM visible area minx
wb%!4  = 200          : REM visible area miny
wb%!8  = 200 + W%     : REM visible area maxx
wb%!12 = 200 + H% + 40 : REM visible area maxy (+40 for title bar height)
wb%!16 = 0             : REM scroll x
wb%!20 = 0             : REM scroll y
wb%!24 = -1            : REM behind handle: -1 = top of stack
REM flags: new format(31) | title bar(26) | close icon(25) | back icon(24) | moveable(1)
wb%!28 = (1<<31) OR (1<<26) OR (1<<25) OR (1<<24) OR (1<<1)
wb%?32 = 7  : REM title fg/frame colour (black)
wb%?33 = 2  : REM title bg colour (grey)
wb%?34 = 7  : REM work area fg colour (black)
wb%?35 = 1  : REM work area bg colour (white)
wb%?36 = 3  : REM scroll bar outer colour (unused, no scroll bars)
wb%?37 = 1
wb%?38 = 12 : REM title bg colour when has input focus (cream)
wb%?39 = 0  : REM extra window flags
wb%!40 = 0             : REM work area minx
wb%!44 = -H%           : REM work area miny (origin top-left, y decreases downward)
wb%!48 = W%            : REM work area maxx
wb%!52 = 0             : REM work area maxy
wb%!56 = 1 + 4 + 8 + 16 + 32 : REM title icon flags: text|border|hcentred|vcentred|filled
wb%!60 = 0             : REM window button type: never (no icons to respond to clicks)
wb%!64 = 1             : REM sprite area: use the Wimp's own pool (we don't need icons)
wb%!68 = 16 + (16 << 16) : REM min width/height (2 bytes each, packed in one word)
$(wb%+72) = "V3D test    " : REM title text, 12 bytes (not indirected)
wb%!84 = 0             : REM icon count: 0, we draw everything ourselves in the redraw loop

SYS "Wimp_CreateWindow", , wb% TO handle%

REM --- open it ---
wb%!0 = handle%
SYS "Wimp_OpenWindow", , wb%

REM --- poll loop ---
DIM pb% 256
quit% = FALSE
REPEAT
  SYS "Wimp_Poll", 0, pb% TO reason%
  CASE reason% OF
    WHEN 1: REM Redraw_Window_Request
      SYS "Wimp_RedrawWindow", , pb% TO more%
      WHILE more%
        REM screen coords of the sprite's bottom-left corner (work area
        REM origin (0,-H%) - the sprite fills the whole work area here)
        sx% = pb%!4 - pb%!20
        sy% = pb%!16 - pb%!24 - H%
        SYS "OS_SpriteOp", 34 + 256, sprite%, "v3dtest", sx%, sy%, 0
        SYS "Wimp_GetRectangle", , pb% TO more%
      ENDWHILE
    WHEN 2: REM Open_Window_Request
      SYS "Wimp_OpenWindow", , pb%
    WHEN 3: REM Close_Window_Request
      SYS "Wimp_CloseWindow", , pb%
      quit% = TRUE
  ENDCASE
UNTIL quit%

SYS "Wimp_CloseDown", task%, &4B534154
END

DEF PROCerror
  ON ERROR OFF : REM disarm first - belt and braces against this
                 REM handler ever recursing into itself again,
                 REM regardless of what future edits do here
  REPORT : PRINT " (error "; ERR; " at line "; ERL; ")"
  IF task% > 0 THEN SYS "Wimp_CloseDown", task%, &4B534154
ENDPROC

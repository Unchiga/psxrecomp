# Forbidden Memories game-code reference

The title team collaborates on
[krystalgamer/memories-decomp](https://github.com/krystalgamer/memories-decomp),
a byte-matching decompilation of the North American SLUS-01411 executable.
Use its game sources, overlay sources, symbol inventory and research notes
when investigating title-specific behavior. Check addresses against the exact
disc/executable revision being recompiled.

Useful starting points are `src/game/`, `src/overlays/`,
`config/slus_01411/` and `notes/`. Sound-driver timer/interrupt ownership is
particularly relevant to game-speed changes: increasing frame-driven game
logic must preserve real-time audio production and musical pace. Measure
guest frame rate, SPU sample production and host underruns separately.

This reference complements runtime debugging; it does not replace generated
C or authorize changing the decompilation project. Verify any local checkout's
remote rather than assuming an old directory name still names this project.

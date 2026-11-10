# Animations
- Per-window animation overrides
- Ensure animations and blur work together without visual artifacts
- Custom animation shaders for windows (e.g. open/close animations)
- Send to desktop animation?

# Effects
- Effects per window state (e.g. unfocused, focused, ...)
- Inner glow effects on borders
- Toplevels with `blur=on` do not render toplevels with blur or mica behind it

# Layout
- Better tab grouping (see sway or Hyprland for reference)
- Better scrolling layout handling (see niri for reference)
- Tiled layout with many toplevels is buggy (adding or removing toplevels causes borders to not be the correct size temporarily, toplevels get clipped to a very small size temporarily)

# Misc
- Multimon is barely tested

# Potential
- Per desktop rules (e.g. force master_stack on a desktop)
- Focus grab protocol
- Overview/Expose mode from niri
- Plugin system
- Move animations to fully shader-based

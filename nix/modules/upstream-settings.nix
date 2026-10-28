# The `config-parity` flake check compares the set of `key`s below against
# `settings_table` in src/ipc/cmd/config.c plus `cfg_extras` in
# src/ipc/cmd/config_list.c plus the specials handled in
# src/ipc/cmd/config_special.c. Adding an upstream setting without adding an
# entry here fails that check.
{ lib }:
let
  inherit (lib) types;
  inherit (builtins)
    concatStringsSep
    isString
    length
    match
    ;

  num = toString;
  bool = v: if v then "true" else "false";
  joinNums = v: concatStringsSep " " (map num v);

  hexColor = types.addCheck types.str (s: match "#?[0-9a-fA-F]{6}([0-9a-fA-F]{2})?" s != null) // {
    description = "a hex colour like \"RRGGBB\" or \"RRGGBBAA\"";
  };

  angle = types.addCheck types.str (s: match "-?[0-9]*\\.?[0-9]+deg" s != null) // {
    description = "an angle like \"90deg\"";
  };

  unit = types.addCheck types.number (x: x >= 0.0 && x <= 1.0) // {
    description = "a number 0.0-1.0";
  };

  between =
    lo: hi:
    types.addCheck types.number (x: x >= lo && x <= hi)
    // {
      description = "a number ${num lo}-${num hi}";
    };

  exclusive = types.addCheck types.number (x: x > 0.0 && x < 1.0) // {
    description = "a number strictly between 0.0 and 1.0";
  };

  positive = types.addCheck types.number (x: x > 0.0) // {
    description = "a number greater than 0";
  };

  positiveInt = types.addCheck types.int (x: x > 0) // {
    description = "an integer greater than 0";
  };

  rgbaList =
    types.addCheck (types.listOf unit) (l: length l == 3 || length l == 4)
    // {
      description = "3 or 4 floats 0.0-1.0";
    };

  rgbaString = types.addCheck types.str (s: match "[-0-9. ]+" s != null) // {
    description = ''a string like "1.0 1.0 1.0 1.0"'';
  };

  rgba = types.either rgbaList rgbaString // {
    description = "3 or 4 floats 0.0-1.0, or a string like \"1.0 1.0 1.0 1.0\"";
  };
  gradient = types.submodule {
    options = {
      stops = lib.mkOption {
        type = types.addCheck (types.listOf hexColor) (l: length l <= 10) // {
          description = "up to 10 hex colour stops";
        };
        default = [ ];
        example = [
          "ff0000"
          "0000ff"
        ];
        description = "Colour stops of the gradient, in order. An empty list clears the gradient.";
      };
      angle = lib.mkOption {
        type = types.nullOr angle;
        default = null;
        example = "45deg";
        description = "Angle of the gradient in degrees.";
      };
    };
  };

  scrollerSize =
    types.either (types.addCheck types.number (x: x > 0.0 && x <= 1.0) // {
      description = "a proportion 0.0-1.0";
    })
      (types.addCheck types.str (s: match "[0-9]+px" s != null) // {
        description = "a fixed size like \"640px\"";
      })
    // {
      description = "a proportion in (0, 1] or a fixed size like \"640px\"";
    };

  scrollerSizes = types.listOf scrollerSize // {
    description = "comma separated proportions and/or fixed sizes";
  };

  lightAnchor = types.submodule {
    options = {
      x = lib.mkOption {
        type = between (-1.0) 1.0;
        example = 0.0;
        description = "Horizontal component, -1.0 to 1.0.";
      };
      y = lib.mkOption {
        type = between (-1.0) 1.0;
        example = 0.0;
        description = "Vertical component, -1.0 to 1.0.";
      };
    };
  };

  formatScrollerSize = v: if isString v then v else num v;
  formatScrollerSizes = v: concatStringsSep "," (map formatScrollerSize v);
  formatRgba = v: if isString v then v else joinNums v;

  mk =
    {
      key,
      path,
      type,
      description,
      example,
      argv,
    }:
    {
      inherit
        key
        path
        type
        description
        example
        argv
        ;
    };

  mkBool =
    key: path: description: example:
    mk {
      inherit
        key
        path
        description
        example
        ;
      type = types.bool;
      argv = v: [
        "config"
        key
        (bool v)
      ];
    };

  mkInt =
    key: path: description: example:
    mk {
      inherit
        key
        path
        description
        example
        ;
      type = types.int;
      argv = v: [
        "config"
        key
        (num v)
      ];
    };

  mkIntBetween =
    lo: hi: key: path: description: example:
    mk {
      inherit
        key
        path
        description
        example
        ;
      type = types.addCheck types.int (x: x >= lo && x <= hi) // {
        description = "an integer ${num lo}-${num hi}";
      };
      argv = v: [
        "config"
        key
        (num v)
      ];
    };

  mkFloat =
    key: path: type: description: example:
    mk {
      inherit
        key
        path
        type
        description
        example
        ;
      argv = v: [
        "config"
        key
        (num v)
      ];
    };

  mkStr =
    key: path: type: description: example:
    mk {
      inherit
        key
        path
        type
        description
        example
        ;
      argv = v: [
        "config"
        key
        (toString v)
      ];
    };

  mkEnum =
    key: path: values: description: example:
    mk {
      inherit
        key
        path
        description
        example
        ;
      type = types.enum values;
      argv = v: [
        "config"
        key
        v
      ];
    };

  mkRgba =
    key: path: description: example:
    mk {
      inherit
        key
        path
        description
        example
        ;
      type = rgba;
      argv = v: [
        "config"
        key
        (formatRgba v)
      ];
    };

  mkGradient =
    key: path: description:
    mk {
      inherit
        key
        path
        description
        ;
      type = gradient;
      example = {
        stops = [
          "ff0000"
          "0000ff"
        ];
        angle = "45deg";
      };
      argv =
        v:
        [
          "config"
          key
        ]
        ++ (
          if v.stops == [ ] then
            [ "clear" ]
          else
            v.stops ++ lib.optional (v.angle != null) v.angle
        );
    };

  animationTypes = [
    "geometry"
    "resize"
    "fade_in"
    "fade_out"
    "fade_in_layer"
    "fade_out_layer"
    "workspace_slide"
  ];

  animationEntries = lib.concatMap (
    t:
    let
      base = [
        "animations"
        "types"
        t
      ];
      withValue =
        field: type: description: example: render:
        mk {
          key = "animation";
          path = base ++ [ field ];
          inherit type description example;
          argv = render;
        };
    in
    [
      (withValue "bezier" types.str "Bezier curve used by this animation type. Set to \"\" to fall back to the default curve." "default" (
        v: [
          "config"
          "animation"
          t
          "bezier"
          v
        ]
      ))
      (withValue "spring" types.str "Spring curve used by this animation type; takes priority over the bezier curve. Set to \"\" to clear it." "bouncy" (
        v: [
          "config"
          "animation"
          t
          "spring"
          v
        ]
      ))
      (withValue "duration" positiveInt "Duration of this animation type in milliseconds." 180 (
        v: [
          "config"
          "animation"
          t
          "duration"
          (num v)
        ]
      ))
      (withValue "enabled" types.bool "Whether this animation type is animated." true (
        v: [
          "config"
          "animation"
          t
          "enabled"
          (bool v)
        ]
      ))
    ]
  ) animationTypes;
in
{
  # `doorsctl config` keys that exist as aliases upstream. They are not exposed
  # as separate options, so the parity check ignores them.
  aliases = [
    "focus_follows_mouse"
    "scroller_default_proportion"
    "scroller_proportion_preset"
  ];

  inherit animationTypes;

  entries =
    [
      # geometry
      (mkInt "border_width" [ "layout" "border-width" ] "Border width around windows, in pixels." 2)
      (mkInt "window_gap" [ "layout" "window-gap" ] "Gap between tiled windows, in pixels." 10)

      # layout behaviour
      (mkBool "borderless_monocle" [ "layout" "borderless-monocle" ] "Remove borders in monocle layout." false)
      (mkBool "borderless_singleton" [ "layout" "borderless-singleton" ] "Remove borders from singleton windows." false)
      (mkBool "smart_gaps" [ "layout" "smart-gaps" ] "Disable window gaps when only one tiled window is on the desktop." false)
      (mkBool "smart_borders" [ "layout" "smart-borders" ] "Hide window borders when only one tiled window is on the desktop." false)
      (mkBool "respect_tiled_min_size" [ "layout" "respect-tiled-min-size" ] "Never shrink tiled windows below their configured minimum size." true)
      (mkBool "focus_wrapping" [ "layout" "focus-wrapping" ] "Wrap directional focus around when there is no window in the target direction." false)
      (mkBool "hide_lone_tab" [ "layout" "hide-lone-tab" ] "Hide the tab bar when a tabbed container holds only one window." false)
      (mkBool "gapless_monocle" [ "layout" "gapless-monocle" ] "Rearrange monocle windows to fill gaps when windows are closed." false)
      (mkIntBetween 0 2147483647 "monocle_top_padding" [ "layout" "monocle-padding" "top" ] "Top padding for monocle layout, in pixels." 0)
      (mkIntBetween 0 2147483647 "monocle_right_padding" [ "layout" "monocle-padding" "right" ] "Right padding for monocle layout, in pixels." 0)
      (mkIntBetween 0 2147483647 "monocle_bottom_padding" [ "layout" "monocle-padding" "bottom" ] "Bottom padding for monocle layout, in pixels." 0)
      (mkIntBetween 0 2147483647 "monocle_left_padding" [ "layout" "monocle-padding" "left" ] "Left padding for monocle layout, in pixels." 0)
      (mkBool "pointer_follows_focus" [ "layout" "pointer-follows-focus" ] "Warp the pointer to follow window focus changes." false)
      (mkBool "record_history" [ "layout" "record-history" ] "Record focus history for cycling through previous windows." true)
      (mkBool "allow_tearing" [ "layout" "allow-tearing" ] "Allow eligible windows to use tearing page flips." false)
      (mkBool "auto_float_dialogs" [ "layout" "auto-float-dialogs" ] "Float windows that advertise themselves as dialogs via xdg-dialog-v1." true)
      (mkBool "enable_minimize" [ "layout" "enable-minimize" ] "Advertise and honour minimize requests." true)
      (mkBool "enable_maximize" [ "layout" "enable-maximize" ] "Advertise and honour maximize requests." true)
      (mkBool "minimize_to_scratchpad" [ "layout" "minimize-to-scratchpad" ] "Send minimized windows to the scratchpad instead of hiding them." false)
      (mkBool "scratchpad_restore_to_origin" [ "layout" "scratchpad-restore-to-origin" ] "Restore scratchpad windows to the desktop they were sent from." false)
      (mkFloat "split_ratio" [ "layout" "split-ratio" ] exclusive "Default split ratio for new containers." 0.5)
      (mkEnum "focus_on_activate" [ "layout" "focus-on-activate" ] [ "focus" "none" "smart" "urgent" ] "How window activation requests are handled." "focus")
      (mkEnum "decoration_mode" [ "layout" "decoration-mode" ] [ "none" "tabs" "always" "csd" ] "How window decorations and tab bars are drawn." "always")
      (mkEnum "automatic_scheme" [ "layout" "automatic-scheme" ] [ "longest_side" "alternate" "spiral" ] "Automatic tiling scheme for new windows." "longest_side")
      (mkEnum "initial_polarity" [ "layout" "initial-polarity" ] [ "first_child" "second_child" ] "Initial insertion side for new windows." "second_child")
      (mkEnum "focus_follows_pointer" [ "layout" "focus-follows-pointer" ] [ "no" "yes" "always" ] "Whether keyboard focus follows the pointer cursor." "no")
      (mkIntBetween 0 2 "ignore_ewmh_fullscreen" [ "layout" "ignore-ewmh-fullscreen" ] "EWMH fullscreen handling: 0 = honor, 1 = ignore, 2 = auto." 0)

      # dimming
      (mkBool "dim_inactive" [ "dim" "inactive" ] "Darken everything except the focused window." false)
      (mkBool "dim_below" [ "dim" "below" ] "Darken the area behind every window." false)
      (mkFloat "dim_strength" [ "dim" "strength" ] unit "Opacity of the dim scrim." 0.1)
      (mkRgba "dim_color" [ "dim" "color" ] "Colour of the dim scrim." [ 0.0 0.0 0.0 1.0 ])

      # borders
      (mkStr "normal_border_color" [ "border" "normal" "color" ] hexColor "Border colour of unfocused windows, or the base colour when a gradient is set." "444444")
      (mkStr "active_border_color" [ "border" "active" "color" ] hexColor "Border colour of windows active on unfocused desktops." "666666")
      (mkStr "focused_border_color" [ "border" "focused" "color" ] hexColor "Border colour of the focused window." "ffffff")
      (mkStr "presel_feedback_color" [ "border" "presel-feedback-color" ] hexColor "Colour of the preselection feedback indicator." "4d9eff")
      (mkStr "tiling_drag_indicator_color" [ "border" "tiling-drag-indicator-color" ] hexColor "Colour of the tiling drag overlay indicator." "4d9eff4d")
      (mkGradient "normal_border_gradient" [ "border" "normal" "gradient" ] "Gradient applied to unfocused window borders.")
      (mkGradient "active_border_gradient" [ "border" "active" "gradient" ] "Gradient applied to active unfocused window borders.")
      (mkGradient "focused_border_gradient" [ "border" "focused" "gradient" ] "Gradient applied to the focused window border.")
      (mkGradient "normal_border_gradient2" [ "border" "normal" "gradient2" ] "Secondary gradient blended into the normal border gradient.")
      (mkGradient "active_border_gradient2" [ "border" "active" "gradient2" ] "Secondary gradient blended into the active border gradient.")
      (mkGradient "focused_border_gradient2" [ "border" "focused" "gradient2" ] "Secondary gradient blended into the focused border gradient.")
      (mkFloat "normal_border_gradient_lerp" [ "border" "normal" "gradient-lerp" ] unit "Blend factor between the primary and secondary normal border gradients." 0.5)
      (mkFloat "active_border_gradient_lerp" [ "border" "active" "gradient-lerp" ] unit "Blend factor between the primary and secondary active border gradients." 0.5)
      (mkFloat "focused_border_gradient_lerp" [ "border" "focused" "gradient-lerp" ] unit "Blend factor between the primary and secondary focused border gradients." 0.5)

      # text and tabs
      (mkStr "text_font" [ "text" "font" ] types.str "Font used for tab text." "JetBrainsMono Nerd Font")
      (mkStr "text_height" [ "text" "height" ] positiveInt "Height of tab text, in pixels." 12)
      (mkRgba "tab_color_bar_bg" [ "text" "tab-color" "bar-bg" ] "Background colour of the tab bar." [ 0.1 0.1 0.1 1.0 ])
      (mkRgba "tab_color_bg" [ "text" "tab-color" "bg" ] "Background colour of inactive tabs." [ 0.1 0.1 0.1 1.0 ])
      (mkRgba "tab_color_bg_active" [ "text" "tab-color" "bg-active" ] "Background colour of the active tab." [ 0.2 0.2 0.2 1.0 ])
      (mkRgba "tab_color_text" [ "text" "tab-color" "text" ] "Colour of inactive tab text." [ 1.0 1.0 1.0 1.0 ])
      (mkRgba "tab_color_text_active" [ "text" "tab-color" "text-active" ] "Colour of active tab text." [ 1.0 1.0 1.0 1.0 ])
      (mkRgba "tab_color_sep" [ "text" "tab-color" "sep" ] "Colour of the tab separator." [ 0.4 0.4 0.4 1.0 ])

      # scroller
      (mkBool "scroller_always_center_single_column" [ "scroller" "always-center-single-column" ] "Centre a single column in the scroller layout." false)
      (mkEnum "scroller_center_focused_column" [ "scroller" "center-focused-column" ] [ "never" "always" "on-overflow" ] "Whether the scroller layout keeps the focused column centred." "never")
      (mkIntBetween 0 2147483647 "scroller_left_strut" [ "scroller" "struts" "left" ] "Left strut in the scroller layout, in pixels." 0)
      (mkIntBetween 0 2147483647 "scroller_right_strut" [ "scroller" "struts" "right" ] "Right strut in the scroller layout, in pixels." 0)
      (mkIntBetween 0 2147483647 "scroller_top_strut" [ "scroller" "struts" "top" ] "Top strut in the scroller layout, in pixels." 0)
      (mkIntBetween 0 2147483647 "scroller_bottom_strut" [ "scroller" "struts" "bottom" ] "Bottom strut in the scroller layout, in pixels." 0)
			(mkIntBetween 2 5 "scroller_pan_fingers" [ "scroller" "pan-fingers" ] "Number of fingers that pan the scroller view." 3)
			(mkIntBetween 0 100 "scroller_snap_percent" [ "scroller" "snap-percent" ] "Sets the minimum percent needed to pan before the scroller prefers the next neighbor." 30)
      (mk {
        key = "scroller_default_column_width";
        path = [
          "scroller"
          "default-column-width"
        ];
        type = scrollerSize;
        example = 0.5;
        description = "Width new scroller columns get, as a proportion of the working area or a fixed size.";
        argv = v: [
          "config"
          "scroller_default_column_width"
          (formatScrollerSize v)
        ];
      })
      (mk {
        key = "scroller_preset_column_widths";
        path = [
          "scroller"
          "preset-column-widths"
        ];
        type = scrollerSizes;
        example = [
          0.33
          0.5
          0.67
        ];
        description = "Preset column widths cycled by the scroller layout.";
        argv = v: [
          "config"
          "scroller_preset_column_widths"
          (formatScrollerSizes v)
        ];
      })
      (mk {
        key = "scroller_preset_window_heights";
        path = [
          "scroller"
          "preset-window-heights"
        ];
        type = scrollerSizes;
        example = [
          0.33
          0.5
          0.67
        ];
        description = "Preset window heights cycled within a scroller column.";
        argv = v: [
          "config"
          "scroller_preset_window_heights"
          (formatScrollerSizes v)
        ];
      })

      # blur
      (mkBool "blur_enabled" [ "blur" "enabled" ] "Enable background blur." true)
      (mkEnum "blur_algorithm" [ "blur" "algorithm" ] [ "none" "kawase" "gaussian" "box" "refraction" "lens_refraction" ] "Blur algorithm." "kawase")
      (mkFloat "blur_radius" [ "blur" "radius" ] positive "Blur radius." 8.0)
      (mkBool "blur_full_res" [ "blur" "full-res" ] "Blur at full resolution instead of downsampling." false)
      (mkIntBetween 1 10 "blur_passes" [ "blur" "passes" ] "Number of blur passes." 2)
      (mkIntBetween 1 8 "blur_downsample" [ "blur" "downsample" ] "Downsample factor used while blurring." 4)
      (mkFloat "blur_offset" [ "blur" "offset" ] (between 0.0 8.0) "Blur offset." 1.0)
      (mkFloat "blur_saturation" [ "blur" "saturation" ] (between 0.0 3.0) "Saturation applied to blurred regions." 1.0)
      (mkFloat "blur_vibrancy" [ "blur" "vibrancy" ] unit "Vibrancy applied to blurred regions." 0.0)
      (mkFloat "blur_vibrancy_darkness" [ "blur" "vibrancy-darkness" ] unit "Darkness of the vibrancy effect." 0.0)
      (mkFloat "blur_noise_strength" [ "blur" "noise-strength" ] unit "Amount of noise added to blurred regions." 0.0)
      (mkFloat "blur_brightness" [ "blur" "brightness" ] (between 0.5 2.0) "Brightness applied to blurred regions." 1.0)
      (mkFloat "blur_contrast" [ "blur" "contrast" ] (between 0.5 2.0) "Contrast applied to blurred regions." 1.0)

      # refraction
      (mkFloat "refraction_strength" [ "refraction" "strength" ] (between 0.0 30.0) "Strength of the refraction effect." 0.0)
      (mkFloat "refraction_edge_size_px" [ "refraction" "edge-size-px" ] (between 0.0 400.0) "Size of the refracting edge, in pixels." 0.0)
      (mkFloat "refraction_corner_radius_px" [ "refraction" "corner-radius-px" ] (between 0.0 400.0) "Corner radius of the refracting edge, in pixels." 0.0)
      (mkFloat "refraction_normal_pow" [ "refraction" "normal-pow" ] (between 0.0 8.0) "Exponent applied to the refraction surface normal." 0.0)
      (mkFloat "refraction_rgb_fringing" [ "refraction" "rgb-fringing" ] unit "Amount of RGB fringing in the refraction effect." 0.0)
      (mkFloat "refraction_offset" [ "refraction" "offset" ] (between 0.0 8.0) "Offset of the refraction effect." 0.0)
      (mkIntBetween 0 1 "refraction_texture_repeat_mode" [ "refraction" "texture-repeat-mode" ] "Texture repeat mode used by refraction (0 or 1)." 0)

      # mica
      (mkBool "mica_enabled" [ "mica" "enabled" ] "Enable the mica material." true)
      (mkFloat "mica_tint_strength" [ "mica" "tint-strength" ] unit "Strength of the mica tint." 0.0)
      (mkRgba "mica_tint" [ "mica" "tint" ] "Tint colour of the mica material." [ 0.0 0.0 0.0 1.0 ])

      # acrylic
      (mkRgba "acrylic_tint" [ "acrylic" "tint" ] "Tint colour of the acrylic material." [ 0.0 0.0 0.0 1.0 ])
      (mkFloat "acrylic_tint_strength" [ "acrylic" "tint-strength" ] unit "Strength of the acrylic tint." 0.0)
      (mkFloat "acrylic_noise_strength" [ "acrylic" "noise-strength" ] unit "Amount of noise added by the acrylic material." 0.0)
      (mkIntBetween 0 10 "acrylic_blur_passes" [ "acrylic" "blur-passes" ] "Number of blur passes used by the acrylic material." 2)
      (mk {
        key = "acrylic_light_anchor";
        path = [
          "acrylic"
          "light-anchor"
        ];
        type = lightAnchor;
        example = {
          x = 0.0;
          y = 0.0;
        };
        description = "Anchor of the acrylic light highlight, -1.0 to 1.0 on each axis.";
        argv = v: [
          "config"
          "acrylic_light_anchor"
          (num v.x)
          (num v.y)
        ];
      })

      # shadow
      (mkFloat "shadow_size" [ "shadow" "size" ] (between 0.0 100.0) "Shadow size, in pixels." 10.0)
      (mkFloat "shadow_offset_x" [ "shadow" "offset-x" ] (between (-100.0) 100.0) "Horizontal shadow offset, in pixels." 0.0)
      (mkFloat "shadow_offset_y" [ "shadow" "offset-y" ] (between (-100.0) 100.0) "Vertical shadow offset, in pixels." 0.0)
      (mkRgba "shadow_color" [ "shadow" "color" ] "Shadow colour." [ 0.0 0.0 0.0 1.0 ])

      # screen shader
      (mkBool "screen_shader_enabled" [ "shader" "enabled" ] "Enable the screen shader." false)
      (mkEnum "screen_shader" [ "shader" "shader" ] [ "none" "grayscale" "invert" "sepia" "nightlight" ] "Built-in screen shader to apply." "none")
      (mkStr "screen_shader_file" [ "shader" "file" ] types.path "Path to a screen shader fragment to load." "/path/to/shader.frag")

      # animations
      (mkBool "enable_animations" [ "animations" "enabled" ] "Enable animations." true)
      (mkEnum "workspace_anim_direction" [ "animations" "workspace-direction" ] [ "vertical" "horizontal" ] "Direction of the workspace switch animation." "vertical")
      (mkBool "workspace_anim_slide_up" [ "animations" "workspace-slide-up" ] "Slide the workspace animation upwards." false)
      (mkStr "animation_bezier" [ "animations" "bezier" ] types.str "Default bezier curve used by animations." "default")
      (mk {
        key = "animation_duration";
        path = [
          "animations"
          "duration"
        ];
        type = positiveInt;
        example = 180;
        description = "Default animation duration in milliseconds.";
        argv = v: [
          "config"
          "animation_duration"
          (num v)
        ];
      })

      # idle power management
      (mkIntBetween 0 86400 "idle_timeout" [ "idle" "timeout" ] "Idle timeout in seconds before DPMS acts; 0 disables it." 300)
      (mkBool "idle_dpms" [ "idle" "dpms" ] "Turn outputs off on idle timeout." true)
      (mkBool "realtime_scheduling" [ "idle" "realtime-scheduling" ] "Request real-time scheduling for the compositor." false)
    ]
    ++ animationEntries;
}

{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flakelight.url = "github:nix-community/flakelight";
  };
  outputs = {flakelight, ...} @ inputs: let
    commonPackages = pkgs:
      with pkgs; [
        ((wlroots_0_20.overrideAttrs {
          version = "0.21.0";
          src = let
            rev = "a94cd29eb13fc2fb68f2fe2d053fef29fb6ae712";
          in
            pkgs.fetchFromGitLab {
              domain = "gitlab.freedesktop.org";
              owner = "wlroots";
              repo = "wlroots";
              inherit rev;
              hash = "sha256-4CSASFBiQFjTHw/73sKKqKLcVYOaRBxaNT7bBAaxHVM=";
            };
        }).override {enableXWayland = true;})

        wayland
        libxcb-wm
        wayland-protocols
        libxkbcommon
        libinput
        pixman
        libxcb
        libGL
        cairo
        pango.dev
        wayland-scanner
        # include <drm_fourcc.h>
        libdrm.dev
      ];

    makeDoors = pkgs:
      pkgs.stdenv.mkDerivation {
        pname = "doors";
        version = "0.0.0";

        src = ./.;

        nativeBuildInputs = with pkgs; [
          pkg-config
          meson
          ninja
          perl
        ];

        NIX_CFLAGS_COMPILE = "-I${pkgs.libdrm.dev}/include/libdrm";

        buildInputs = commonPackages pkgs;
        patchPhase = ''
          # it needs to be an executable to make patchShebangs run
          chmod +x src/shaders/embed.pl
          patchShebangs src/shaders/embed.pl

          # this should be handled by package maintainer not your buildscript D:
          substituteInPlace meson.build \
                  --replace-fail "/usr/share/wayland-sessions" "${placeholder "out"}/share/wayland-sessions"
        '';

        passthru.providedSessions = ["doors"];

        meta = {
          description = "Wayland compositor based on bswpm, with floating, tiling and scrolling layouts.";
          homepage = "https://github.com/dy-tea/doors";
          license = pkgs.lib.licenses.gpl3;
          maintainers = ["anispwyn"];
        };
      };
  in
    flakelight ./. {
      inherit inputs;
      systems = ["x86_64-linux"];
      packages = rec {
        default = {pkgs, ...}: makeDoors pkgs;
        doors = default;
        doorsctl = {pkgs, ...}:
          (makeDoors pkgs).overrideAttrs (oldAttrs: {
            pname = "doorsctl";
            postInstall = ''
              find $out -type f -not -name doorsctl -delete
              find $out -type d -empty -delete
            '';
            meta =
              oldAttrs.meta
              // {
                description = "Control client for the doors Wayland compositor";
              };
          });
      };
    };
}

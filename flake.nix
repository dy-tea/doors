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
            rev = "28d11d058053fb8596c18c01bb83592a884e02b6";
          in
            pkgs.fetchFromGitLab {
              domain = "gitlab.freedesktop.org";
              owner = "wlroots";
              repo = "wlroots";
              inherit rev;
              hash = "sha256-QJ4WrRB46Sn1GyYLvTdEtZAQd1FNAVTDC2OyH4iRadk=";
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
  in
    flakelight ./. {
      inherit inputs;
      systems = ["x86_64-linux"];
      package = {
        pkgs,
        stdenv,
        lib,
        ...
      }:
        stdenv.mkDerivation {
          pname = "bwm";
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

          meta = {
            description = "Wayland compositor based on bswpm, with floating, tiling and scrolling layouts.";
            homepage = "https://github.com/dy-tea/bwm";
            license = lib.licenses.gpl3;
            maintainers = ["anispwyn"];
          };
        };
    };
}

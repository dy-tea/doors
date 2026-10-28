{
  description = "A wayland compositor based on bswpm, with floating, tiling and scrolling layouts.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs?ref=nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    wlroots-git = {
      url = "https://gitlab.freedesktop.org/wlroots/wlroots/-/archive/a94cd29eb13fc2fb68f2fe2d053fef29fb6ae712/wlroots-a94cd29eb13fc2fb68f2fe2d053fef29fb6ae712.tar.gz";
      flake = false;
    };
  };

  outputs =
    inputs@{ self, flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = [ "x86_64-linux" ];

      flake.nixosModules.doors = import ./nix/modules/nixosModule.nix self;
      flake.homeModules.doors = import ./nix/modules/homeModule.nix self;

      perSystem =
        { pkgs, system, lib, self', ... }:
        let
          commonPackages = with pkgs; [
            (
              (wlroots_0_20.overrideAttrs {
                version = "git";
                src = inputs.wlroots-git;
              }).override
                { enableXWayland = true; }
            )

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
            vulkan-loader
            shaderc.dev
            libdrm.dev
            pcre2
          ];

          doors = pkgs.stdenv.mkDerivation {
            pname = "doors";
            version = "git";
            src = ./.;

            nativeBuildInputs = with pkgs; [
              pkg-config
              meson
              ninja
              perl
            ];
            buildInputs = commonPackages;

            NIX_CFLAGS_COMPILE = "-I${pkgs.libdrm.dev}/include/libdrm";

            patchPhase = ''
              chmod +x src/shaders/embed.pl
              patchShebangs src/shaders/embed.pl
              substituteInPlace meson.build \
              --replace-fail "/usr/share/wayland-sessions" "${placeholder "out"}/share/wayland-sessions"
            '';

            passthru.providedSessions = [ "doors" ];

            meta = {
              description = "A wayland compositor based on bswpm, with floating, tiling and scrolling layouts.";
              homepage = "https://github.com/dy-tea/doors";
              license = pkgs.lib.licenses.gpl3Only;
              maintainers = [
                "foxtrottt"
              ];
            };
          };

					fovva = pkgs.stdenv.mkDerivation (finalAttrs: {
						pname = "fovva";
						version = "1.1";

						src = pkgs.fetchFromGitHub {
							owner = "dy-tea";
							repo = "fovva";
							tag = "v${finalAttrs.version}";
							hash = "sha256-SV7Y6/sb+hK8WQgwpqvSdeVa0QceAaL9zPyluOTasGc=";
						};

						nativeBuildInputs = [ pkgs.vlang ];

						preBuild = ''
							export HOME=$TMPDIR
						'';

						buildPhase = ''
							runHook preBuild
							v -prod -o fovva .
							runHook postBuild
						'';

						installPhase = ''
							runHook preInstall
							install -Dm755 fovva $out/bin/fovva
							runHook postInstall
						'';

						meta = {
							description = "C formatter following the wlroots style";
							license = lib.licenses.gpl3Only;
						};
					});
        in
        {
          packages.default = doors;
          packages.doors = doors;
          packages.doorsctl = pkgs.stdenv.mkDerivation {
            pname = "doorsctl";
            version = "git";
            dontUnpack = true;
            buildPhase = "$CC ${./doorsctl}/doorsctl.c -o doors";
            installPhase = "install -Dm755 doors $out/bin/doorsctl";
          };
					packages.fovva = fovva;

          checks = {
            config-parity = import ./nix/checks/config-parity.nix {
              inherit pkgs;
              src = ./.;
              moduleData = ./nix/modules/upstream-settings.nix;
            };
            emit-smoke = import ./nix/checks/emit-smoke.nix {
              inherit pkgs;
              moduleData = ./nix/modules/upstream-settings.nix;
              settingsModule = ./nix/modules/settings.nix;
            };
          };

					devShells.default = pkgs.mkShell {
						buildInputs = with pkgs; [ nixfmt-tree fovva ];
					};
        };
    };
}

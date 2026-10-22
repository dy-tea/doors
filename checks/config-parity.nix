{
  pkgs,
  src,
  moduleData,
}:
let
  lib = pkgs.lib;

  upstream = import moduleData { inherit lib; };

  moduleKeys = pkgs.writeText "doors-module-keys.json" (
    builtins.toJSON (
      builtins.attrNames (
        builtins.listToAttrs (map (e: lib.nameValuePair e.key true) upstream.entries)
      )
    )
  );

  aliases = pkgs.writeText "doors-module-aliases.json" (builtins.toJSON upstream.aliases);
in
pkgs.runCommand "doors-config-parity" {
  nativeBuildInputs = with pkgs; [
    coreutils
    gnugrep
    gnused
    jq
  ];
} ''
  set -eu

  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT

  # settings_table entries, e.g. I("border_width", ...)
  grep -oE '[A-Z]\("[a-z0-9_]+"' ${src}/src/ipc/cmd/config.c \
    | sed -E 's/^[A-Z]\("//; s/"$//' > "$work/settings.txt"

  # cfg_extras entries: every quoted snake_case identifier in the table
  sed -n '/cfg_extras\[\] = {/,/^};/p' ${src}/src/ipc/cmd/config_list.c \
    | grep -oE '"[a-z0-9]+(_[a-z0-9]+)+"' | tr -d '"' > "$work/extras.txt"

  # special_settings names, including the ones not introspectable
  # (`animation`, `screen_shader_file`) and the aliases we filter below
  sed -n '/special_settings\[\] = {/,/^};/p' ${src}/src/ipc/cmd/config_special.c \
    | grep -oE '\{"[a-z0-9_]+"' | sed -E 's/^\{"//; s/"$//' > "$work/specials.txt"

  cat "$work/settings.txt" "$work/extras.txt" "$work/specials.txt" | sort -u > "$work/upstream-all.txt"
  jq -r '.[]' ${aliases} | sort -u > "$work/aliases.txt"

  comm -23 "$work/upstream-all.txt" "$work/aliases.txt" > "$work/upstream.txt"
  jq -r '.[]' ${moduleKeys} | sort -u > "$work/module.txt"

  if ! diff -u "$work/upstream.txt" "$work/module.txt"; then
    echo "" >&2
    echo "doors: modules/upstream-settings.nix does not match the upstream config keys." >&2
    echo "      Add or remove entries so every key is covered exactly once." >&2
    exit 1
  fi

  printf 'doors: all %s upstream config keys are covered\n' "$(wc -l < "$work/upstream.txt")" > "$out"
''

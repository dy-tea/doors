# Evaluates the module with every option set to its example value and asserts
# that the generated doorsrc emits exactly one line per entry, with the exact
# upstream key and no null values.
{
  pkgs,
  moduleData,
  settingsModule,
}:
let
  lib = pkgs.lib;

  upstream = import moduleData { inherit lib; };

  examples = lib.foldl (
    acc: e:
    lib.recursiveUpdate acc (
      lib.setAttrByPath e.path (if (e.example or null) == null then 0 else e.example)
    )
  ) { } upstream.entries;

  expected = builtins.toJSON (
    lib.foldl' (acc: e: acc // { ${e.key} = (acc.${e.key} or 0) + 1; }) { } upstream.entries
  );

  self = {
    packages.x86_64-linux.doors = null;
  };

  evaluated = lib.evalModules {
    modules = [
      (import settingsModule self)
      {
        programs.doors.enable = true;
        programs.doors.settings.config = examples;
      }
    ];
    specialArgs = {
      pkgs = {
        stdenv.hostPlatform.system = "x86_64-linux";
      };
    };
  };

  doorsrc = evaluated.config.programs.doors.generated.doorsrc;
  expectedJson = pkgs.writeText "doors-emit-expected.json" expected;
in
pkgs.runCommand "doors-emit-smoke" {
  nativeBuildInputs = with pkgs; [
    coreutils
    gawk
    gnugrep
    jq
  ];
} ''
  set -eu

  cat > config <<'DOORSRC'
${doorsrc}
DOORSRC

  awk '$1 == "doorsctl" && $2 == "config" { print $3 }' config \
    | sort | uniq -c | awk '{ print $2 " " $1 }' | sort > actual.txt

  jq -r 'to_entries[] | "\(.key) \(.value)"' ${expectedJson} | sort > expected.txt

  if ! diff -u expected.txt actual.txt; then
    echo "" >&2
    echo "doors: generated doorsrc does not emit exactly one line per configured setting." >&2
    exit 1
  fi

  if grep -nE '^doorsctl (config|output|env|desktop) .* null$' config; then
    echo "doors: generated doorsrc contains a null value" >&2
    exit 1
  fi

  printf 'doors: %s config lines emitted across %s upstream keys\n' \
    "$(grep -c '^doorsctl config ' config)" "$(wc -l < actual.txt)" > "$out"
''

self:
{
  config,
  lib,
  ...
}:
let
  cfg = config.programs.doors;
in
{
  imports = [ (import ./settings.nix self) ];

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];
    xdg.configFile."doors/doorsrc".text = cfg.generated.doorsrc;
    xdg.configFile."doors/doorshkrc".text = cfg.generated.doorshkrc;
  };
}

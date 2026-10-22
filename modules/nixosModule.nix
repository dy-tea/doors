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
    environment.systemPackages = [ cfg.package ];
    services.displayManager.sessionPackages = [ cfg.package ];
    environment.etc."doors/doorsrc".text = cfg.generated.doorsrc;
    environment.etc."doors/doorshkrc".text = cfg.generated.doorshkrc;
  };
}

# A NixOS VM where selected packages ship as SELF (SQLite) executables and
# run transparently via binfmt_misc.  `nix run .#self-vm`, then log in as
# root (empty password) and try:
#     file $(command -v hello-self) ; hello-self
#     self info $(command -v hello-self)
{
  lib,
  pkgs,
  selfify,
  elf2self,
  ...
}:
{
  imports = [ ./module.nix ];

  programs.self.enable = true;
  programs.self.mode = "memfd";

  # Demo payloads: a converted GNU hello and coreutils, plus the tools to
  # poke at them from inside the VM.
  # `hello` here is a SELF database (its bin/hello was converted by selfify),
  # yet it runs transparently via binfmt_misc.
  environment.systemPackages = [
    elf2self
    pkgs.sqlite
    pkgs.file
    (selfify pkgs.hello)
  ];

  # A boot-time proof: convert hello, run it through binfmt, assert output.
  systemd.services.self-demo = {
    description = "SELF binfmt self-test";
    wantedBy = [ "multi-user.target" ];
    after = [ "systemd-binfmt.service" ];
    serviceConfig = {
      Type = "oneshot";
      RemainAfterExit = true;
      StandardOutput = "journal+console";
      StandardError = "journal+console";
    };
    path = [
      elf2self
      pkgs.sqlite
      pkgs.coreutils
    ];
    script = ''
      set -uo pipefail
      cd /tmp
      echo "== binfmt registration:"
      cat /proc/sys/fs/binfmt_misc/self || echo "  (self not registered!)"
      cp ${pkgs.hello}/bin/hello hello.elf
      elf2self hello.elf hello.self
      chmod +x hello.self
      echo "== file(1) sees a database:"
      ${pkgs.file}/bin/file hello.self
      echo "== running ./hello.self via binfmt_misc:"
      ./hello.self; rc=$?
      echo "exit=$rc"
      out="$(./hello.self)"
      echo "output=[$out]"
      test "$out" = "Hello, world!" && echo "SELF-DEMO-OK" || echo "SELF-DEMO-FAIL"
    '';
  };

  # minimal, fast-booting VM
  users.users.root.password = "";
  services.getty.autologinUser = "root";
  virtualisation.vmVariant.virtualisation = {
    graphics = false;
    memorySize = 2048;
    diskSize = 4096;
  };
  system.stateVersion = lib.trivial.release;
}

{
  description = "SELF: an executable format that is a SQLite database (see DESIGN.md)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};

      elf2self = pkgs.callPackage ./nix/elf2self.nix { };
      self-exec = pkgs.callPackage ./nix/self-exec.nix { };
      selfify = drv: pkgs.callPackage ./nix/selfify.nix { inherit elf2self; } drv;
    in
    {
      packages.${system} = {
        inherit elf2self self-exec;
        default = self-exec;
        # a converted GNU hello, for `nix build .#hello-self`
        hello-self = selfify pkgs.hello;
      };

      # importable: `imports = [ selfdb.nixosModules.default ];` then set
      # programs.self.enable = true. (Pass self-exec via specialArgs, or use
      # the overlay below which puts self-exec in pkgs.)
      nixosModules.default =
        { ... }:
        {
          imports = [ ./nix/module.nix ];
          _module.args.self-exec = self-exec;
        };

      overlays.default = final: _prev: {
        self-exec = final.callPackage ./nix/self-exec.nix { };
        elf2self = final.callPackage ./nix/elf2self.nix { };
        selfify = drv: final.callPackage ./nix/selfify.nix { elf2self = final.elf2self; } drv;
      };

      # `nix run .#self-vm` -> a NixOS VM running SELF binaries via binfmt.
      nixosConfigurations.self-vm = nixpkgs.lib.nixosSystem {
        inherit system;
        modules = [
          (import ./nix/self-vm.nix)
          { _module.args = { inherit selfify elf2self self-exec; }; }
        ];
      };

      apps.${system}.self-vm = {
        type = "app";
        program = "${self.outputs.nixosConfigurations.self-vm.config.system.build.vm}/bin/run-nixos-vm";
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = [
          (pkgs.python3.withPackages (
            ps: with ps; [
              lief
              capstone
            ]
          ))
          pkgs.sqlite
          pkgs.sqldiff
          pkgs.sqlite-analyzer
          pkgs.gcc
          pkgs.pkg-config
          pkgs.binutils
          pkgs.patchelf
          pkgs.hyperfine
          pkgs.file
          pkgs.qemu
        ];
      };

      formatter.${system} = pkgs.nixfmt-tree;
    };
}

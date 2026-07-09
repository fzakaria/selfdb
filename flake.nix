{
  description = "SELF: an executable format that is a SQLite database (see DESIGN.md)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { nixpkgs, ... }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      # Packages (elf2self, self-exec, the NixOS self-vm) land here per the
      # roadmap in DESIGN.md §11. Dev shell only for now.
      devShells.${system}.default = pkgs.mkShell {
        packages = [
          # converter (M0): python + LIEF, reusing sqlelf's extractors
          (pkgs.python3.withPackages (
            ps: with ps; [
              lief
              capstone
            ]
          ))
          pkgs.sqlite # sqlite3 CLI + libsqlite3 for the loader
          pkgs.sqldiff
          pkgs.sqlite-analyzer

          # loader (M1/M2): plain C against libsqlite3
          pkgs.gcc
          pkgs.pkg-config

          # inspection / benchmarking
          pkgs.binutils # readelf/nm — the tools we're retiring, for diffing
          pkgs.patchelf
          pkgs.hyperfine
          pkgs.file

          # VM demo (M1+)
          pkgs.qemu
        ];
      };

      formatter.${system} = pkgs.nixfmt-tree;
    };
}

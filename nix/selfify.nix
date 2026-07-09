# selfify: rewrite a package's $out/bin ELF executables into SELF databases.
# Opt-in per package via an overlay:  hello-self = selfify hello;
{
  stdenv,
  elf2self,
  file,
}:
drv:
stdenv.mkDerivation {
  pname = "${drv.pname or "pkg"}-self";
  inherit (drv) version;
  nativeBuildInputs = [
    elf2self
    file
  ];
  # Copy the package, then convert every dynamically-linked ELF executable
  # in bin/ in place. Interpreters/scripts are left untouched.
  buildCommand = ''
    cp -r ${drv} $out
    chmod -R u+w $out
    for d in bin sbin libexec; do
      [ -d "$out/$d" ] || continue
      for f in "$out/$d"/*; do
        [ -f "$f" ] || continue
        if file -b "$f" | grep -q '^ELF .* executable'; then
          elf2self "$f" "$f.self"
          mv "$f.self" "$f"
          chmod +x "$f"
        fi
      done
    done
  '';
  meta = (drv.meta or { }) // {
    description = "${drv.meta.description or drv.pname} (SELF/SQLite executables)";
  };
}

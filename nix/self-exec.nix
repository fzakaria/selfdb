{
  stdenv,
  pkg-config,
  sqlite,
}:
stdenv.mkDerivation {
  pname = "self-exec";
  version = "0.1.0";
  src = ../loader;
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ sqlite ];
  makeFlags = [ "PREFIX=$(out)" ];
  installTargets = [ "install" ];
  meta.description = "binfmt_misc interpreter that runs SELF (SQLite) executables";
}

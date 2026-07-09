{ python3Packages }:
python3Packages.buildPythonApplication {
  pname = "selfconv";
  version = "0.1.0";
  pyproject = true;
  src = ../converter;
  build-system = [ python3Packages.setuptools ];
  dependencies = [ python3Packages.lief ];
  # No test suite packaged here; round-trip tests run from the devshell.
  doCheck = false;
  meta.description = "Converters between ELF and the SELF (SQLite) executable format";
}

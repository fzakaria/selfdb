# NixOS module: register SELF with binfmt_misc so ./foo.self "just runs".
{
  config,
  lib,
  self-exec,
  ...
}:
let
  cfg = config.programs.self;
in
{
  options.programs.self = {
    enable = lib.mkEnableOption "the SELF (SQLite) executable format";
    mode = lib.mkOption {
      type = lib.types.enum [
        "memfd"
        "native"
      ];
      default = "memfd";
      description = "self-exec loader mode (M1 memfd or M2 native).";
    };
    package = lib.mkOption {
      type = lib.types.package;
      default = self-exec;
      description = "The self-exec interpreter package.";
    };
  };

  config = lib.mkIf cfg.enable {
    # binfmt_misc match: SQLite files ('SQLite format 3\0') whose 4-byte
    # big-endian application_id at offset 68 is 'SELF' (0x53454C46). We
    # match a 72-byte window from offset 0 and mask out bytes 16..67, which
    # vary between databases, so ordinary SQLite files never match.
    boot.binfmt.registrations.self = {
      recognitionType = "magic";
      offset = 0;
      magicOrExtension =
        "\\x53\\x51\\x4c\\x69\\x74\\x65\\x20\\x66\\x6f\\x72\\x6d\\x61\\x74\\x20\\x33\\x00"
        + lib.concatStrings (lib.genList (_: "\\x00") 52) # bytes 16..67: don't-care
        + "\\x53\\x45\\x4c\\x46"; # bytes 68..71: 'SELF'
      mask =
        (lib.concatStrings (lib.genList (_: "\\xff") 16)) # 'SQLite format 3\0'
        + (lib.concatStrings (lib.genList (_: "\\x00") 52)) # ignored
        + "\\xff\\xff\\xff\\xff"; # application_id
      interpreter = "${cfg.package}/bin/self-exec";
      # Kernel argv to the interpreter (see fs/binfmt_misc.c load_misc_binary):
      #   [self-exec, <binary path>, <original argv[1:]>]
      # self-exec opens argv[1] and re-execs with argv[1:], so the target's
      # argv[0] becomes the (resolved) binary path -- basename() still
      # satisfies multi-call binaries like coreutils. We deliberately do NOT
      # set preserveArgvZero: it inserts the original argv[0] as an extra
      # leading operand that strict programs (e.g. GNU hello) reject.
      openBinary = false;
      preserveArgvZero = false;
      wrapInterpreterInShell = false;
    };

    # The loader reads SELF_MODE from the environment.
    environment.sessionVariables.SELF_MODE = cfg.mode;
    systemd.globalEnvironment.SELF_MODE = cfg.mode;
  };
}

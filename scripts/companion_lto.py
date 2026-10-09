"""Keep the Arduino linker consistent with the companion compile flags."""


def configure_lto(env):
    flags = env.ParseFlags(env.GetProjectOption("build_flags", ""))
    if "-flto" not in flags.get("CCFLAGS", []):
        return
    env.Replace(LINKFLAGS=[flag for flag in env["LINKFLAGS"] if flag != "-fno-lto"])
    env.AppendUnique(LINKFLAGS=["-flto"])


def configure_lto_before_link(source, target, env):
    configure_lto(env)


Import("env")  # noqa: F821 -- provided by PlatformIO
configure_lto(env)  # noqa: F821
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", configure_lto_before_link)  # noqa: F821

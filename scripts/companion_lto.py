"""Keep the Arduino linker consistent with the companion compile flags."""


def configure_lto(env):
    flags = env.ParseFlags(env.get("BUILD_FLAGS", []))
    if "-flto" not in flags.get("CCFLAGS", []):
        return
    env.Replace(LINKFLAGS=[flag for flag in env["LINKFLAGS"] if flag != "-fno-lto"])
    env.AppendUnique(LINKFLAGS=["-flto"])


Import("env")  # noqa: F821 -- provided by PlatformIO
configure_lto(env)  # noqa: F821

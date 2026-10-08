"""Checked SdFat enumeration-error patch; callers publish both files together.

Normal EOF leaves the directory error bits unchanged. Failed enumeration sets
READ_ERROR on the directory, which survives FsBaseFile discarding the entry.
This module has no import-time filesystem or PlatformIO side effects.
"""

MARKER = "// lila: distinguish directory enumeration failure from EOF."


def patch_fs_short_name(source: str) -> str:
    anchor = "  /** \\return value of writeError */\n  bool getWriteError() const {"
    addition = """  // lila: checked FAT alias; exFAT has no short filename.
  bool getShortName(char* name, size_t len) {
    if (!name || len < 13) return false;
    name[0] = 0;
    return m_fFile ? m_fFile->getSFN(name, len) != 0 : m_xFile != nullptr;
  }

"""
    if source.count(anchor) != 1 or source.count(addition) > 1:
        raise ValueError("SdFat short-name patch does not match pinned source")
    if addition in source:
        return source
    return source.replace(anchor, addition + anchor, 1)


def replace_once(source: str, old: str, new: str) -> str:
    original_count = source.count(old)
    patched_count = source.count(new)
    if original_count == 0 and patched_count == 1:
        return source
    if original_count != 1 or patched_count != 0:
        raise ValueError("SdFat enumeration patch does not match pinned source")
    return source.replace(old, new, 1)


def patch_function(source: str, signature: str, transform) -> str:
    start = source.index(signature)
    end = source.index("\n//------------------------------------------------------------------------------", start)
    function = source[start:end]
    patched = transform(function)
    return source[:start] + patched + source[end:]


def patch_fat(source: str) -> str:
    def enumeration(function: str) -> str:
        function = replace_once(function, """      if (dirFile->getError()) {
        DBG_FAIL_MACRO;
      }
      goto fail;""", """      if (!dirFile->getError() && !lfnOrd) return false;
      DBG_FAIL_MACRO;
      goto fail;""")
        function = replace_once(function, """    if (dir->name[0] == FAT_NAME_FREE) {
      goto fail;
    }""", """    if (dir->name[0] == FAT_NAME_FREE) {
      if (!lfnOrd) return false;
      goto fail;
    }""")
        return replace_once(function, "\nfail:\n  return false;", "\nfail:\n  " + MARKER +
                            "\n  dirFile->m_error |= READ_ERROR;\n  return false;")

    def cache_read(function: str) -> str:
        return replace_once(function, """  if (n != 0) {
    DBG_FAIL_MACRO;
  }""", """  if (n != 0) {
    """ + MARKER + """
    m_error |= READ_ERROR;
    DBG_FAIL_MACRO;
  }""")

    source = patch_function(source, "bool FatFile::openNext(", enumeration)
    return patch_function(source, "DirFat_t* FatFile::readDirCache()", cache_read)


def patch_exfat(source: str) -> str:
    def enumeration(function: str) -> str:
        return replace_once(function, "\nfail:\n  return false;", "\nfail:\n  " + MARKER +
                            "\n  dir->m_error |= READ_ERROR;\n  return false;")

    def private_open(function: str) -> str:
        function = replace_once(function, """    if (n == 0) {
      goto create;
    }""", """    if (n == 0) {
      if (!fname && !inSet) return false;
      if (!fname) goto fail;
      goto create;
    }""")
        function = replace_once(function, """        // Likely openNext call.
        DBG_WARN_MACRO;
        goto fail;""", """        if (!inSet) return false;
        goto fail;""")
        return replace_once(function, "\nfail:\n  // close file", "\nfail:\n  " + MARKER +
                            "\n  if (!fname) dir->m_error |= READ_ERROR;\n  // close file")

    source = patch_function(source, "bool ExFatFile::openNext(", enumeration)
    return patch_function(source, "bool ExFatFile::openPrivate(", private_open)

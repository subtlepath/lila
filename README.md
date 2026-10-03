<p align="center"><img src="src/images/Logo120.png" width="120" alt="lila, a small black pug"></p>

# lila

**An opinionated fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** for Xteink e-readers.

Named for Lila, a very small black pug who was a little awkward, and perfect.

## What makes lila opinionated

CrossPoint gives you a lot of controls. lila picks the defaults and designs around them, so the device feels finished
rather than configurable.

- **Every book opens to a page someone has carefully designed.** Leading, measure, justification, hyphenation and
  paragraph rhythm are tuned together, with Knuth-Plass line breaking and hand-edited Times and Helvetica bitmap fonts.
  Text size and other comfort controls stay.
- **The device wakes into your reading life.** Home is your Library: the book you're reading sits on top, ready to
  resume, with the rest of your shelf, progress and search beneath it.
- **You never hunt for your place.** Sleep shows the page you were on. Footnotes, notes, settings changes, sleep and
  restarts all bring you back to the same spot.
- **One menu language.** The reader menu keeps everyday actions on its first page, moves the rest a level down, and
  Back always returns to the page. Library and Settings follow the same rules.
- **Games at the table.** One reader hosts and up to five others join (or CPU players fill in), over ESP-NOW with no
  Wi-Fi network needed.

lila is early. It's developed on the Xteink X4; release builds also cover the X3, X4Pro, X4Classic, Seeed reTerminal
Sticky and M5PaperMono, but those get less testing.

## Everything else is CrossPoint

lila tracks CrossPoint's `develop` branch and merges it regularly, so most of what it does is CrossPoint's work: the
EPUB 2/3 engine (images, hyphenation, kerning, tables, CJK ruby, footnotes, bookmarks, StarDict dictionaries), `.epub`,
`.xtc`, `.txt` and `.bmp` support, touch reading, custom SD-card fonts, the Wi-Fi tools (file transfer, web settings,
WebDAV, Calibre wireless, OPDS), KOReader progress sync, and 34 UI languages including CJK and right-to-left scripts.
The [CrossPoint README](https://github.com/crosspoint-reader/crosspoint-reader#readme) has the full list.

To support that work, [fund CrossPoint's contributors](https://app.royalty.dev/crosspoint-reader/crosspoint-reader) or
buy an X3/X4 Developer Edition through [crosspointreader.com](https://crosspointreader.com).

Some things keep CrossPoint's names and services on purpose:

- **Your SD card stays compatible.** lila uses the same `/.crosspoint/` data folder, so moving over from CrossPoint
  doesn't need a fresh card. Caches whose format changed are rebuilt automatically.
- **Calibre** sends books through the CrossPoint Reader plugin, and **KOReader sync** defaults to CrossPoint's sync
  server.
- **Font downloads** come from CrossPoint's font repository.

---

## Install firmware

### Web installer

1. Download the firmware file for your device from [lila's releases](https://github.com/subtlepath/lila/releases).
2. Connect your device to your computer via USB-C and wake/unlock it.
3. Go to CrossPoint's web flasher at https://crosspointreader.com/#flash-tools, select your device, click
   "Custom .bin" and upload the firmware file.

### Updates

Once lila is installed, **Settings › System › Check for Updates** installs new lila releases over Wi-Fi.

### Going back to CrossPoint

Flash the official firmware from https://crosspointreader.com/#flash-tools. Your books stay on the SD card.

### USB-locked devices

Some Xteink units bought from third-party stores (e.g. AliExpress) ship with USB flashing locked. The Xteink Unlocker
only supports CrossPoint and CrossInk, and flashing other firmware onto a locked device can leave it stuck or bricked.
**Don't install lila on a USB-locked device.** CrossPoint's
[unlock notes](https://github.com/crosspoint-reader/crosspoint-reader#usb-locked-devices-xteink-unlocker) explain how
to tell whether yours is locked.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download the firmware file for your device from [lila's releases](https://github.com/subtlepath/lila/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash an X3 or X4:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

   Flash an Xteink X4Pro, Seeed reTerminal Sticky, or M5PaperMono:

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

On devices with external RAM enabled, copy `.ttf`, `.otf`, or `.ttc` files to the SD card and select them as reader fonts. Put one file in `/fonts/` or `/.fonts/`, or put one family's files in a subfolder. See the [SD card font guide](./docs/sd-card-fonts.md) for the folder layout and styles.

On other devices, convert the font to `.cpfont` first. `.cpfont` files also work on devices with external RAM enabled and have better performance. No firmware reflash is needed to add fonts.

To make `.cpfont` files:

1. Go to CrossPoint's font builder at https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [CrossPoint project scope](./SCOPE.md) (upstream's; lila's opinions are listed above)
- [Contributing docs](./docs/contributing/README.md)
- [Touch and UI development](./docs/contributing/touch-and-ui.md) - how to build new screens on the FreeInkUI activity bases (UiListActivity and friends), plus build envs for the non-Xteink touch devices

---

## Development quick start

### Prerequisites

- [pioarduino PlatformIO Core](https://github.com/pioarduino/platformio-core) or [VS Code + pioarduino IDE](https://github.com/pioarduino/pioarduino-vscode-ide)
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/subtlepath/lila
cd lila
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

Like CrossPoint, lila is aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card; lila keeps CrossPoint's folder name so cards move between the two. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

lila is a personal project with a point of view. Bug reports are welcome in
[Issues](https://github.com/subtlepath/lila/issues); ideas belong in
[Discussions](https://github.com/subtlepath/lila/discussions) first.

Please don't report lila bugs to CrossPoint. If a bug also happens on official CrossPoint firmware, report it there so
every fork gets the fix. lila sends general fixes upstream too.

---

## License

MIT, like CrossPoint. [LICENSE](./LICENSE) keeps CrossPoint's copyright notice alongside lila's.

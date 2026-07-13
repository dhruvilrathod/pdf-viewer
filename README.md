# PDFast

**A fast, lightweight, native Windows PDF viewer & editor** — compiled to a
**single self-contained `.exe`**. No installer, no .NET, no VC++ redistributable,
no external DLLs beyond stock Windows system libraries. Rendering and document
handling are provided by [MuPDF](https://mupdf.com/), statically linked.

The produced `PDFast.exe` is ~5.5 MB and depends only on DLLs already present on
every Windows 10/11 machine (`kernel32`, `user32`, `gdi32`, `gdiplus`,
`comctl32`, `comdlg32`, `shell32`, `ole32`, `uxtheme`, `dwmapi`, `msimg32`,
`winhttp`, `advapi32`). Hand it to anyone on a clean Windows 10/11 PC and it just
runs.

## Why PDFast?

I built PDFast because the PDF viewer built into **Microsoft Edge** stopped being
good. After Edge switched the underlying PDF engine, everyday viewing got slower
and noticeably buggier — and the setting that used to let you fall back to the
old, reliable viewer was quietly **removed**, so there's no longer a way to opt
out of the regression.

PDFast brings back what that experience used to be: a **fast, no-nonsense,
native** PDF viewer that opens instantly, scrolls smoothly, renders crisply, and
stays out of your way — plus the editing and document tools the built-in viewer
never had. It's a single tiny `.exe`, so there's nothing to install and nothing
to slow you down.

## Download & install

1. Go to the [**Releases**](../../releases) page and download `PDFast.exe` from
   the latest release.
2. Put it anywhere you like (Desktop, a tools folder, …) and double-click it.
   There is **nothing to install**.
3. On first run PDFast adds itself to the **Start menu** and to the right-click
   **"Open with"** list, and offers (once) to become your **default PDF app** —
   accept to open the Windows default-apps screen, or skip it. You can change
   this any time in Windows Settings → Apps → Default apps.

> **SmartScreen note:** because the download is not code-signed, Windows may show
> *"Windows protected your PC."* Click **More info → Run anyway**. This is normal
> for unsigned open-source software. (You can also right-click the downloaded
> file → **Properties → Unblock** before launching.)

### Runtime requirements

**None** beyond Windows 10 or 11. PDFast is a native Windows application — it does
not run on macOS or Linux.

## Features

**Viewing**
- Open via **File → Open**, **drag-and-drop**, the **Start menu**, right-click
  **"Open with → PDFast"**, or a command-line path.
- Opens **maximized** by default, with Windows 11 native chrome (rounded corners,
  Mica backdrop) where available.
- Smooth continuous scroll, zoom, **Fit Width**, **Fit Page**, page drop shadows.
- Multiple documents in **tabs** (drag to reorder, tabs shrink to fit).
- Crisp rendering on HiDPI displays (per-monitor DPI aware).

**Search & Print**
- **Find** text (Ctrl+F): incremental highlight of all matches, live counter,
  jump between matches (Enter / F3 / Shift+F3).
- **Print** (Ctrl+P): standard Windows print dialog, page-range printing.

**Annotate & fill forms**
- Highlight, freehand draw, and free-text boxes — all edited **inline**, no modal
  dialogs. Fill AcroForm fields directly on the page.

**Security**
- Open **password-protected** PDFs (inline password prompt).
- **Remove password / restrictions** from an encrypted PDF you can open.

**Automatic updates**
- On launch the app quietly checks GitHub for a newer release. If one exists it
  offers a **one-click update** — downloads the new build, swaps it in, and
  relaunches. Also available on demand via **Tools → Check for Updates**.

**Document tools** (toolbar → wrench menu)
- **Organize pages** — reorder (drag), rotate, delete, insert pages from other files.
- **Merge** several PDFs; **Split** by page ranges.
- **Resize** pages to A4.
- **Convert to read-only** (flatten each page to an image — text no longer selectable).
- **Compress** (downsample + recompress images via MuPDF's image rewriter — big
  wins on scanned documents).
- **Redact** — permanently remove marked content (not just cover it).
- After any of these, choose **Save** (overwrite) or **Save a Copy**.

## Building from source

### Build-time dependencies (not needed at runtime)

- **Visual Studio 2022 Build Tools** (or full VS 2022) with the *Desktop
  development with C++* workload — provides MSVC, the Windows SDK, and
  `cmake`/`rc`.
- **CMake** 3.20+ (bundled with recent VS, or from cmake.org).
- **Git for Windows** — used to fetch MuPDF's source (and, for CJK/full font
  coverage, MuPDF's `hexdump.sh` font generator, which needs `bash`).

Install quickly with winget:

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id Kitware.CMake
```

### Fetch MuPDF (vendored under third_party/, not committed)

```powershell
git clone --recurse-submodules --depth 1 https://github.com/ArtifexSoftware/mupdf.git third_party/mupdf
```

### Configure and build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target pdfviewer
```

The result is `build/Release/PDFast.exe`. (The internal CMake target is still
named `pdfviewer`; only the output binary is `PDFast.exe`.)

### Embedded fallback-font coverage (optional)

MuPDF embeds fallback fonts used only when a PDF doesn't embed its own. More
coverage = bigger `.exe`. Choose at configure time:

```powershell
# base14 (default, ~5.5 MB) : standard Latin PDF fonts
# cjk    (~34 MB)           : + Chinese/Japanese/Korean
# full   (~48 MB)           : + Noto (Arabic, Hebrew, Thai, emoji, …)
cmake -S . -B build -DPDFVIEWER_FONTS=cjk
```

## Publishing a release (for maintainers)

The in-app auto-updater checks the GitHub repo named in
[`src/updater.h`](src/updater.h) (`kGitHubRepo`) — make sure that matches your
repo before your first release.

1. Bump the version in [`src/version.h`](src/version.h) (e.g. `1.2.0`).
2. Build Release (above); the artifact is `build/Release/PDFast.exe`.
3. On GitHub: **Releases → Draft a new release**, create a tag that matches the
   version with a leading `v` (e.g. `v1.2.0`), and drag `PDFast.exe` into the
   assets box. **The asset must be a `.exe`** — that's what the updater
   downloads (it grabs the release's first `.exe` asset).
4. Publish. Everyone running an older build gets the update prompt on their next
   launch; the download link is
   `https://github.com/<you>/<repo>/releases/download/v1.2.0/PDFast.exe`.

> The updater only works from a build that already contains it, so include it in
> your **first** published release — otherwise the initial users have to update
> once by hand.

## License

This application is licensed under the **GNU Affero General Public License v3.0**
(see [`LICENSE`](LICENSE)). It statically links **MuPDF**, which is also AGPL v3.

**What this means if you redistribute it:** distributing the binary (e.g. a
public download) obliges you to make the **complete corresponding source code**
available under the AGPL — which this repository does. If instead you want to ship
a **closed-source** build, you must obtain a **commercial MuPDF license** from
[Artifex](https://artifex.com/); the AGPL does not permit closed-source
redistribution.

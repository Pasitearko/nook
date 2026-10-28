# Third-party software and design provenance

nook (星择) is an independently written Linux application. The UI takes functional and layout inspiration from [jimmgreen/pulse](https://github.com/jimmgreen/pulse), reviewed at commit `6468d399fc726761191c7c51ad2c97288dcaa40d` (Apache-2.0). No Pulse source files, screenshots, wallpapers, binary SDKs, or branded artwork are included. SVG icons in this repository are original simple geometric drawings.

The application source is Apache-2.0. Dependency licenses remain their own:

| Dependency | Use | License / source |
| --- | --- | --- |
| Qt Core, GUI, QML, Quick, Quick Controls, Concurrent, Network | Dynamically linked system libraries | LGPL-3.0 / GPL / commercial; https://doc.qt.io/qt-6/licensing.html |
| GLib / GIO | Files, operations and desktop integration | LGPL-2.1-or-later; https://gitlab.gnome.org/GNOME/glib |
| SQLite | Filename index, FTS5 | Public domain; https://sqlite.org/copyright.html |
| ICU | Unicode case handling / Han-to-Latin transliteration | Unicode-3.0 and bundled notices; https://unicode-org.github.io/icu/userguide/icu4c/faq.html |
| Poppler utilities (optional, separate system executable) | PDF metadata and page rendering | GPL-2.0-or-later and component notices; https://poppler.freedesktop.org/ |
| FFmpeg utilities (optional, separate system executable) | Media metadata and cover | License depends on distribution build; https://ffmpeg.org/legal.html |

The package uses system libraries and tools; it does not bundle or modify these dependencies. Their source, copyright notices and licenses are supplied by the distribution packages. Redistributors must satisfy the licenses of what they actually distribute, including LGPL library replacement/relinking and source obligations. Using dynamic linking alone does not waive these duties. Do not introduce GPL-only Qt modules without reviewing their effect on application distribution.

No permission to use upstream trademarks or imply upstream endorsement is asserted.

# Markdown compatibility dependency

MD4C is dynamically linked from the installed system package, with no copied
parser implementation. Qt Gui already uses this library on the workstation;
Asset Preview also calls its public event API to identify presentation-only HTML
in Markdown without mistaking code, escaped examples or link targets for tags.
The package is `md4c` / `libmd4c-dev`; no package was acquired during this change.
Upstream: <https://github.com/mity/md4c>. Workstation reference: MD4C 0.6.0.

The existing [runtime dependency notices](../THIRD_PARTY_NOTICES.md) cover the
other libraries. Release collection follows dynamic linkage to collect the
actual library and its package provenance.

## MD4C — MIT

Copyright (c) 2016-2026 Martin Mitáš

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

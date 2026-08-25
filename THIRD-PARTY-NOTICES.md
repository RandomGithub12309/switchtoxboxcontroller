# Third-party notices

## ViGEmClient (Nefarius)

This project ships `ViGEmClient.dll`, built unmodified from the official
source code of the ViGEm Client SDK, and declares its API in `main.cpp`
to load it at runtime.

- Project:   https://github.com/nefarius/ViGEmClient
- Version:   v1.16.18.0
- License:   MIT License
- Copyright: (c) 2017-2023 Nefarius Software Solutions e.U. and Contributors

```
MIT License

Copyright (c) 2018 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The complete unmodified ViGEmClient sources used for the build are included
under `vendor/ViGEmClient/`.

## ViGEmBus driver

The virtual gamepad driver (`ViGEmBus`) is installed separately by the user
and is not distributed with this project.

- Project: https://github.com/nefarius/ViGEmBus

## Protocol references

The Switch controller HID report layout and rumble encoding implemented in
this project are based on publicly documented reverse-engineering efforts:

- dekuNukem, "Nintendo Switch Reverse Engineering" (HID notes)
  https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering
- BetterJoy (MIT), reference for rumble/USB report handling
  https://github.com/Davidobot/BetterJoy

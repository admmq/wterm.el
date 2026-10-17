# message from the HUMAN

Hi, Adam here. This project is almost entirely LLM-generated. Works for me. Anyone, who wants to rewrite is welcome.

# wterm.el: Windows terminal for Emacs

A vterm-style terminal emulator for native Windows Emacs. Tested on Windows 11.

## Before installation
Install MSYS2. Then open MSYS2 UCRT64 console and install dependencies:
```bash
pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-emacs mingw-w64-ucrt-x86_64-libvterm mingw-w64-ucrt-x86_64-git
```

## Install from GitHub

```elisp
(use-package wterm
  :vc (:url "https://github.com/admmq/wterm.el.git"
       :branch "main"
       :rev :newest)
  :commands (wterm wterm-other-window wterm-compile))
```

## Use

`M-x wterm` opens a terminal running `wterm-shell` (pwsh by default, then
powershell, then cmd.exe). `C-u M-x wterm` asks which command to run.

Most keys go to the terminal. `C-c`, `C-x`, `C-u`, `C-g`, `C-h`, `M-x` and
`M-:` stay with Emacs.

| Key       | Action                                         |
| --------- | ---------------------------------------------- |
| `C-c C-c` | send Ctrl-C                                    |
| `C-c C-z` | send Ctrl-Z                                    |
| `C-q KEY` | send KEY as is (e.g. `C-q C-x`)                |
| `C-y`     | paste the latest kill                          |
| `M-w`     | copy the region                                |
| `C-c C-t` | copy mode: browse the scrollback, `q` to leave |
| `C-c C-l` | clear the scrollback and the screen            |

## Limitations

- Lines already in the scrollback are not re-wrapped when the window width
  changes.
- Mouse reporting to terminal programs is not implemented.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

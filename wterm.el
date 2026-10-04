;;; wterm.el --- Terminal emulator for Emacs on Windows -*- lexical-binding: t -*-

;; Version: 0.1
;; Package-Requires: ((emacs "28.1"))
;; Keywords: terminals, processes

;;; Commentary:

;; A vterm-like terminal for native Windows Emacs.
;;
;; Emacs on Windows has no pty support, so programs run through
;; `wterm-conpty.exe', which hosts them in a Windows pseudo console
;; (ConPTY) and talks to Emacs over pipes.  The escape sequences coming out
;; of the pseudo console are interpreted by libvterm inside a dynamic
;; module (`wterm-module.dll') that renders the screen into the buffer.
;;
;; M-x wterm starts `wterm-shell'.  With a prefix argument it asks for the
;; command to run.
;;
;; Keys are sent to the terminal, except for the ones in
;; `wterm-keymap-exceptions'.  C-c is a prefix:
;;   C-c C-c   send C-c            C-c C-t   copy mode
;;   C-c C-l   clear scrollback    C-q       send the next key verbatim
;; C-y pastes the latest kill into the terminal.

;;; Code:

(require 'subr-x)
(require 'ansi-color)
(require 'url-util)
(require 'compile)
(require 'seq)

(defgroup wterm nil
  "Terminal emulator for Windows."
  :group 'terminals)

(defconst wterm-install-directory
  (file-name-directory (or load-file-name buffer-file-name default-directory))
  "Directory containing wterm.el and its compiled parts.")

(defcustom wterm-shell
  (or (executable-find "pwsh") (executable-find "powershell") "cmd.exe")
  "Command line run by wterm"
  :type 'string)

(defcustom wterm-conpty-program
  (expand-file-name "wterm-conpty.exe" wterm-install-directory)
  "The ConPTY bridge program."
  :type 'file)

(defcustom wterm-max-scrollback 10000
  "Maximum number of scrollback lines kept in the buffer."
  :type 'integer)

(defcustom wterm-timer-delay 0.01
  "Minimum seconds between redraws while output is streaming in.
Output arriving after a pause, such as the reply to a key, is shown at
once."
  :type 'number)

(defcustom wterm-buffer-name "*wterm*"
  "Name of new terminal buffers."
  :type 'string)

(defcustom wterm-buffer-name-string nil
  "When non-nil, rename the buffer after the terminal title.
The value is a format string where %s is the title, e.g. \"wterm %s\"."
  :type '(choice (const :tag "Off" nil) string))

(defcustom wterm-kill-buffer-on-exit t
  "Kill the buffer when its process exits."
  :type 'boolean)

(defcustom wterm-environment
  '("TERM=xterm-256color" "COLORTERM=truecolor")
  "Extra environment variables for programs run in the terminal."
  :type '(repeat string))

(defcustom wterm-track-directory t
  "Follow the shell's working directory.
The shell has to report it with OSC 7 (file://host/path), OSC 9;9
\(Windows Terminal style) or OSC 51;A (vterm style)."
  :type 'boolean)

(defcustom wterm-bell t
  "Ring the Emacs bell when the terminal rings."
  :type 'boolean)

(defcustom wterm-keymap-exceptions
  '("C-c" "C-x" "C-u" "C-g" "C-h" "C-y" "M-x" "M-:" "M-w")
  "Keys that keep their Emacs binding instead of going to the terminal.
Changes take effect when wterm.el is reloaded."
  :type '(repeat string))

;;; Module

(defvar wterm-module-file (expand-file-name "wterm-module.dll" wterm-install-directory))

(defun wterm--built-p ()
  "Non-nil when the module and the ConPTY bridge exist."
  (and (file-exists-p wterm-module-file)
       (file-exists-p wterm-conpty-program)))

;;;###autoload
(defun wterm-compile ()
  "Build the wterm module and ConPTY bridge using MSYS2.
Installs gcc, make and libvterm with pacman if missing, then runs make
in the wterm directory.  The module is loaded when the build succeeds."
  (interactive)
  (let* ((msys (read-directory-name "MSYS2 installation directory: "
                                    "C:/" nil t "msys64"))
         (bash (expand-file-name "usr/bin/bash.exe" msys))
         (_ (unless (file-exists-p bash)
              (user-error "wterm: %s not found" bash)))
         (src (directory-file-name wterm-install-directory))
         (cmd (format "cd '%s' && mingw32-make.exe"
                      src))
         (shell-file-name bash)
         (shell-command-switch "-lc")
         (compilation-environment
          (append '("MSYSTEM=UCRT64" "CHERE_INVOKING=1")
                  compilation-environment))
         (default-directory wterm-install-directory)
         (buf (compilation-start cmd nil (lambda (_) "*wterm-compile*"))))
    (with-current-buffer buf
      (add-hook 'compilation-finish-functions #'wterm--compile-finished nil t))
    buf))

(defun wterm--compile-finished (buf status)
  "Load the module after a successful build in BUF; STATUS is the result."
  (when (buffer-live-p buf)
    (with-current-buffer buf
      (remove-hook 'compilation-finish-functions #'wterm--compile-finished t)))
  (if (and (string-prefix-p "finished" status) (wterm--built-p))
      (progn (wterm--load-module)
             (message "wterm: build finished, run M-x wterm"))
    (message "wterm: build failed, see %s" (buffer-name buf))))

(defun wterm--load-module ()
  "Load the module unless it is already loaded."
  (unless (featurep 'wterm-module)
    (module-load wterm-module-file)))

(defun wterm--ensure-module ()
  "Load the module, offering to build it first when it is missing."
  (cond ((featurep 'wterm-module))
        ((wterm--built-p) (wterm--load-module))
        ((y-or-n-p "wterm: module is not built.  Build it with MSYS2 now? ")
         (wterm-compile)
         (user-error "wterm: building, run `M-x wterm' again when it finishes"))
        (t (user-error "wterm: %s not found" wterm-module-file))))

(when (wterm--built-p)
  (wterm--load-module))

(declare-function wterm--new "wterm-module")
(declare-function wterm--write-input "wterm-module")
(declare-function wterm--redraw "wterm-module")
(declare-function wterm--set-size "wterm-module")
(declare-function wterm--key "wterm-module")
(declare-function wterm--char "wterm-module")
(declare-function wterm--paste "wterm-module")
(declare-function wterm--set-palette "wterm-module")
(declare-function wterm--clear-scrollback "wterm-module")
(declare-function wterm--pop-events "wterm-module")

;;; State

(defvar-local wterm--term nil "The module's terminal object.")
(defvar-local wterm--process nil "The process running in the terminal.")
(defvar-local wterm--redraw-timer nil)
(defvar-local wterm--last-redraw 0.0 "When the terminal was last redrawn.")
(defvar-local wterm--quick-redraws 0
  "Redraws in a row that came less than `wterm-timer-delay' apart.")
(defvar-local wterm--cursor-timer nil)
(defvar-local wterm-title nil "The title the terminal program last set.")
(defvar wterm-copy-mode)

;;; Keymaps

(defun wterm--special-keys ()
  "Function keys forwarded to the terminal."
  (append '(up down left right home end prior next insert delete deletechar
               backspace tab return escape
               kp-0 kp-1 kp-2 kp-3 kp-4 kp-5 kp-6 kp-7 kp-8 kp-9
               kp-multiply kp-add kp-separator kp-subtract kp-decimal
               kp-divide kp-enter)
          (mapcar (lambda (n) (intern (format "f%d" n))) (number-sequence 1 12))))

(defvar wterm-mode-map
  (let ((map (make-keymap)))
    (define-key map [remap self-insert-command] #'wterm--self-insert)
    (dolist (key '("RET" "TAB" "DEL"))
      (define-key map (kbd key) #'wterm--self-insert))
    ;; Control and meta combinations.
    (dolist (c (number-sequence ?a ?z))
      (dolist (prefix '("C-" "M-" "C-M-"))
        (let ((key (concat prefix (string c))))
          (unless (member key wterm-keymap-exceptions)
            (define-key map (kbd key) #'wterm--self-insert)))))
    (dolist (c (string-to-list "0123456789`~!@#$%^&*()-_=+[]{}\\|;'\",.<>/?"))
      (let ((key (concat "M-" (string c))))
        (unless (member key wterm-keymap-exceptions)
          (define-key map (kbd key) #'wterm--self-insert))))
    (define-key map (kbd "C-_") #'wterm--self-insert)
    (define-key map (kbd "C-/") #'wterm--self-insert)
    (define-key map (kbd "M-DEL") #'wterm--self-insert)
    ;; Function keys with any combination of modifiers.
    (dolist (k (wterm--special-keys))
      (dolist (mods '(() (shift) (control) (meta) (control shift)
                      (meta shift) (control meta) (control meta shift)))
        (define-key map (vector (event-convert-list (append mods (list k))))
                    #'wterm--self-insert)))
    ;; Emacs side commands.
    (define-key map (kbd "C-c C-c") #'wterm-send-C-c)
    (define-key map (kbd "C-c C-z") #'wterm-send-C-z)
    (define-key map (kbd "C-c C-t") #'wterm-copy-mode)
    (define-key map (kbd "C-c C-l") #'wterm-clear-scrollback)
    (define-key map (kbd "C-q") #'wterm-send-next-key)
    (define-key map (kbd "C-y") #'wterm-yank)
    (define-key map (kbd "M-w") #'wterm-copy-region-or-send)
    (define-key map [S-insert] #'wterm-yank)
    (define-key map [mouse-2] #'wterm-yank-primary)
    map)
  "Keymap for `wterm-mode'.")

(defvar wterm-copy-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map (kbd "C-c C-t") #'wterm-copy-mode)
    (define-key map (kbd "q") #'wterm-copy-mode)
    (define-key map (kbd "RET") #'wterm-copy-mode-done)
    (define-key map (kbd "M-w") #'wterm-copy-mode-done)
    map)
  "Keymap for `wterm-copy-mode'.")

;;; Mode

(define-derived-mode wterm-mode fundamental-mode "WTerm"
  "Major mode for wterm terminal buffers.

\\{wterm-mode-map}"
  (buffer-disable-undo)
  (setq buffer-read-only t)
  (setq-local truncate-lines t
              scroll-margin 0
              hscroll-margin 0
              hscroll-step 1
              scroll-conservatively 101
              show-trailing-whitespace nil
              display-line-numbers nil
              global-hl-line-mode nil
              bidi-paragraph-direction 'left-to-right
              bidi-inhibit-bpa t
              fringe-indicator-alist (cons '(truncation nil nil)
                                           fringe-indicator-alist))
  (add-hook 'kill-buffer-hook #'wterm--on-kill nil t))

;; Terminal programs expect every key, so evil's normal state gets in the way.
(with-eval-after-load 'evil
  (when (fboundp 'evil-set-initial-state)
    (evil-set-initial-state 'wterm-mode 'emacs)))

;;;###autoload
(defun wterm (&optional arg)
  "Start a terminal running `wterm-shell' in a new buffer.
With prefix ARG, ask for the command to run."
  (interactive "P")
  (wterm--ensure-module)
  (let* ((command (if arg
                      (read-shell-command "Run in terminal: " wterm-shell)
                    wterm-shell))
         (dir default-directory)
         (buf (generate-new-buffer wterm-buffer-name)))
    (pop-to-buffer-same-window buf)
    (with-current-buffer buf
      (setq default-directory
            (if (and (not (file-remote-p dir)) (file-directory-p dir))
                dir
              (expand-file-name "~/")))
      (wterm-mode)
      (wterm--start command))
    buf))

;;;###autoload
(defun wterm-other-window (&optional arg)
  "Like `wterm', but show the terminal in another window.
With prefix ARG, ask for the command to run."
  (interactive "P")
  (let ((buf (save-window-excursion (wterm arg))))
    (pop-to-buffer buf)
    buf))

(defun wterm--window-size ()
  "Size of the window showing the current buffer, as (COLS . ROWS)."
  (let ((win (get-buffer-window (current-buffer))))
    (if win
        (cons (max 1 (window-max-chars-per-line win))
              (max 1 (with-selected-window win (floor (window-screen-lines)))))
      '(80 . 24))))

(defun wterm--start (command)
  "Run COMMAND in the terminal of the current buffer."
  (let* ((size (wterm--window-size))
         (cols (car size))
         (rows (cdr size))
         (process-environment
          (append wterm-environment
                  (list (format "INSIDE_EMACS=%s,wterm" emacs-version))
                  process-environment))
         (process-adaptive-read-buffering nil)
         ;; Bigger pipes mean fewer, larger reads of the terminal output.
         (w32-pipe-buffer-size (* 64 1024))
         (inhibit-read-only t))
    (erase-buffer)
    (setq wterm--term (wterm--new rows cols wterm-max-scrollback))
    (wterm--set-palette wterm--term (wterm--palette))
    (setq wterm--process
          (make-process
           :name "wterm"
           :buffer (current-buffer)
           :command (list wterm-conpty-program (number-to-string rows)
                          (number-to-string cols) command)
           :connection-type 'pipe
           :coding '(utf-8-unix . no-conversion)
           :noquery t
           :filter #'wterm--filter
           :sentinel #'wterm--sentinel))
    (process-put wterm--process 'adjust-window-size-function
                 #'wterm--adjust-process-window-size)
    (wterm--redraw-now (current-buffer))))

(defun wterm--palette ()
  "The 16 ANSI colors of the current theme."
  (vconcat
   (mapcar (lambda (face) (face-foreground face nil 'default))
           '(ansi-color-black ansi-color-red ansi-color-green
             ansi-color-yellow ansi-color-blue ansi-color-magenta
             ansi-color-cyan ansi-color-white ansi-color-bright-black
             ansi-color-bright-red ansi-color-bright-green
             ansi-color-bright-yellow ansi-color-bright-blue
             ansi-color-bright-magenta ansi-color-bright-cyan
             ansi-color-bright-white))))

(defun wterm--update-palettes (&rest _)
  "Refresh the colors of all terminals after a theme change."
  (dolist (buf (buffer-list))
    (with-current-buffer buf
      (when (and (derived-mode-p 'wterm-mode) wterm--term)
        (wterm--set-palette wterm--term (wterm--palette))
        (wterm--schedule-redraw)))))

(add-hook 'enable-theme-functions #'wterm--update-palettes)
(add-hook 'disable-theme-functions #'wterm--update-palettes)

;;; Process I/O

(defun wterm--send-bytes (bytes)
  "Send BYTES, a unibyte string or nil, to the terminal's process."
  (when (and bytes (> (length bytes) 0) (process-live-p wterm--process))
    (process-send-string wterm--process bytes)))

(defun wterm--sanitize (string)
  "Replace raw bytes in STRING with U+FFFD."
  (apply #'string (mapcar (lambda (c) (if (> c #x3FFF7F) #xFFFD c)) string)))

(defun wterm--filter (proc output)
  "Feed OUTPUT from PROC to the terminal and schedule a redraw."
  (let ((buf (process-buffer proc)))
    (when (buffer-live-p buf)
      (with-current-buffer buf
        (when wterm--term
          (wterm--send-bytes
           (condition-case nil
               (wterm--write-input wterm--term output)
             (error (wterm--write-input wterm--term
                                        (wterm--sanitize output)))))
          (wterm--schedule-redraw))))))

(defun wterm--schedule-redraw ()
  "Redraw now, or soon if output is streaming in."
  (unless wterm--redraw-timer
    (let ((wait (- (+ wterm--last-redraw wterm-timer-delay) (float-time))))
      (cond
       ((<= wait 0)
        (setq wterm--quick-redraws 0)
        (wterm--redraw-now (current-buffer)))
       ;; A reply to a key often comes in a few pieces (PowerShell hides
       ;; the cursor, then draws the line): show each one at once.
       ((< wterm--quick-redraws 3)
        (setq wterm--quick-redraws (1+ wterm--quick-redraws))
        (wterm--redraw-now (current-buffer)))
       (t
        (setq wterm--redraw-timer
              (run-with-timer wait nil #'wterm--redraw-now
                              (current-buffer))))))))

(defun wterm--redraw-now (buf)
  "Render the terminal of BUF and move the windows showing it."
  (when (buffer-live-p buf)
    (with-current-buffer buf
      (when wterm--redraw-timer
        (cancel-timer wterm--redraw-timer)
        (setq wterm--redraw-timer nil))
      (when (and wterm--term (not wterm-copy-mode))
        (let ((inhibit-read-only t)
              (inhibit-modification-hooks t)
              start)
          (save-restriction
            (widen)
            (setq start (wterm--redraw wterm--term)))
          (setq wterm--last-redraw (float-time))
          (dolist (win (get-buffer-window-list buf nil t))
            (set-window-point win (point))
            (when start
              (set-window-start win start t))))
        (wterm--handle-events)))))

(defun wterm--handle-events ()
  (dolist (event (wterm--pop-events wterm--term))
    (pcase event
      (`(title . ,title)
       (setq wterm-title title)
       (when wterm-buffer-name-string
         (rename-buffer (format wterm-buffer-name-string title) t)))
      (`(cursor-visible . ,visible)
       (wterm--set-cursor-visible visible))
      (`(bell . ,_)
       (when wterm-bell (ding t)))
      (`(osc ,cmd . ,text)
       (wterm--handle-osc cmd text)))))

(defun wterm--set-cursor-visible (visible)
  "Show or hide the cursor as the terminal program asked.
Hiding is delayed a little: shells like PowerShell hide the cursor
while they repaint the prompt line and show it again right away, which
would make it flicker on every key."
  (when wterm--cursor-timer
    (cancel-timer wterm--cursor-timer)
    (setq wterm--cursor-timer nil))
  (if visible
      (kill-local-variable 'cursor-type)
    (setq wterm--cursor-timer
          (run-with-timer
           0.05 nil
           (lambda (buf)
             (when (buffer-live-p buf)
               (with-current-buffer buf
                 (setq wterm--cursor-timer nil)
                 (setq-local cursor-type nil))))
           (current-buffer)))))

(defun wterm--handle-osc (cmd text)
  "Handle OSC CMD with payload TEXT that libvterm left to us."
  (when wterm-track-directory
    (let ((dir (pcase cmd
                 (7 (and (string-match "\\`file://[^/]*\\(/.*\\)" text)
                         (url-unhex-string (match-string 1 text))))
                 (9 (and (string-prefix-p "9;" text) (substring text 2)))
                 (51 (and (string-match "\\`A[^:]*:\\(.*\\)" text)
                          (match-string 1 text))))))
      (when-let* ((dir (and dir (wterm--local-directory dir))))
        (setq default-directory dir)))))

(defun wterm--local-directory (path)
  "Turn PATH reported by a shell into an Emacs directory name, or nil."
  (let ((p (string-trim path "[\" \t]+" "[\" \t]+")))
    (when (string-match "\\`/\\([a-zA-Z]\\):?\\(/.*\\)?\\'" p)
      (setq p (concat (match-string 1 p) ":" (or (match-string 2 p) "/"))))
    (when (and (not (string-empty-p p)) (file-directory-p p))
      (file-name-as-directory (expand-file-name p)))))

(defun wterm--sentinel (proc event)
  "Clean up when PROC exits with EVENT."
  (let ((buf (process-buffer proc)))
    (when (buffer-live-p buf)
      (with-current-buffer buf
        (wterm--redraw-now buf)
        (setq wterm--term nil)
        (if wterm-kill-buffer-on-exit
            (kill-buffer buf)
          (let ((inhibit-read-only t))
            (save-excursion
              (goto-char (point-max))
              (insert "\n\nProcess " (string-trim-right event) "\n"))))))))

(defun wterm--on-kill ()
  (when wterm--redraw-timer
    (cancel-timer wterm--redraw-timer))
  (when wterm--cursor-timer
    (cancel-timer wterm--cursor-timer))
  (when (process-live-p wterm--process)
    (delete-process wterm--process)))

(defun wterm--adjust-process-window-size (process windows)
  "Resize the terminal of PROCESS to fit WINDOWS."
  (let ((buf (process-buffer process)))
    (when (and (buffer-live-p buf) (process-live-p process))
      (with-current-buffer buf
        (unless wterm-copy-mode
          (let ((size (funcall window-adjust-process-window-size-function
                               process windows)))
            (when size
              (wterm--resize (max 1 (cdr size)) (max 1 (car size))))))))
    ;; The pseudo console is resized through the bridge, not as a pty.
    nil))

(defun wterm--resize (rows cols)
  (when (and wterm--term (wterm--set-size wterm--term rows cols))
    (wterm--send-bytes (format "\377R%d;%d\n" rows cols))
    (wterm--schedule-redraw)))

;;; Input

(defun wterm--mod-bits (mods)
  (logior (if (memq 'shift mods) 1 0)
          (if (memq 'meta mods) 2 0)
          (if (memq 'control mods) 4 0)))

(defun wterm-send-event (event)
  "Send the input EVENT (a key) to the terminal."
  (when wterm--term
    (cond
     ((integerp event)
      (let ((char (logand event (lognot ?\M-\0)))
            (meta (/= 0 (logand event ?\M-\0))))
        (when (eq char (event-convert-list '(control ?/))) ; ^_ on terminals
          (setq char ?\C-_))
        (if (characterp char)
            (wterm--send-bytes
             (concat (and meta "\e") (encode-coding-string (string char) 'utf-8)))
          (wterm--send-bytes
           (wterm--char wterm--term (event-basic-type event)
                        (wterm--mod-bits (event-modifiers event)))))))
     ((symbolp event)
      (wterm--send-bytes
       (wterm--key wterm--term (symbol-name (event-basic-type event))
                   (wterm--mod-bits (event-modifiers event))))))))

(defun wterm--self-insert ()
  "Send the key that invoked this command to the terminal."
  (interactive)
  (wterm-send-event last-command-event))

(defun wterm-send-next-key ()
  "Read a key and send it to the terminal as is."
  (interactive)
  (wterm-send-event (let ((inhibit-quit t)) (read-event "Send key: "))))

(defun wterm-send-C-c ()
  "Send C-c to the terminal."
  (interactive)
  (wterm--send-bytes "\C-c"))

(defun wterm-send-C-z ()
  "Send C-z to the terminal."
  (interactive)
  (wterm--send-bytes "\C-z"))

(defun wterm-send-string (string &optional paste)
  "Send STRING to the terminal.
If PASTE is non-nil, send it as a bracketed paste when the program
supports that."
  (when wterm--term
    (let ((text (replace-regexp-in-string "\r?\n" "\r" string)))
      (when paste
        (wterm--send-bytes (wterm--paste wterm--term t)))
      (wterm--send-bytes (encode-coding-string text 'utf-8))
      (when paste
        (wterm--send-bytes (wterm--paste wterm--term nil))))))

(defun wterm-yank (&optional arg)
  "Paste the latest kill (or the ARGth one) into the terminal."
  (interactive "P")
  (wterm-send-string (current-kill (cond ((listp arg) 0)
                                         ((eq arg '-) -2)
                                         (t (1- arg))))
                     t))

(defun wterm-yank-primary (event)
  "Paste the primary selection, or the latest kill, at mouse EVENT."
  (interactive "e")
  (ignore event)
  (wterm-send-string (or (gui-get-primary-selection) (current-kill 0)) t))

(defun wterm-copy-region-or-send ()
  "Copy the region if it is active, otherwise send M-w."
  (interactive)
  (if (use-region-p)
      (kill-ring-save (region-beginning) (region-end))
    (wterm-send-event last-command-event)))

(defun wterm-clear-scrollback ()
  "Discard the scrollback and clear the screen."
  (interactive)
  (when wterm--term
    (wterm--clear-scrollback wterm--term)
    (wterm--send-bytes "\C-l")
    (wterm--schedule-redraw)))

;;; Copy mode

(define-minor-mode wterm-copy-mode
  "Freeze the terminal so its buffer can be browsed and copied.
Output is still processed and shows up when the mode is turned off."
  :lighter " Copy"
  :keymap wterm-copy-mode-map
  (unless (derived-mode-p 'wterm-mode)
    (setq wterm-copy-mode nil)
    (user-error "Not a wterm buffer"))
  (if wterm-copy-mode
      (use-local-map nil)
    (use-local-map wterm-mode-map)
    (deactivate-mark)
    (wterm--redraw-now (current-buffer))))

(defun wterm-copy-mode-done ()
  "Copy the region, if any, and leave copy mode."
  (interactive)
  (when (use-region-p)
    (kill-ring-save (region-beginning) (region-end)))
  (wterm-copy-mode -1))

(provide 'wterm)
;;; wterm.el ends here

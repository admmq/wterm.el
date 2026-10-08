/* wterm-module.c -- libvterm based terminal emulation for Emacs.

   The module owns a libvterm instance.  Emacs feeds it the bytes coming
   from the pseudo console and asks it to redraw; the module then renders
   the terminal into the current buffer.

   Buffer layout:

     scrollback line 0\n
     ...
     scrollback line N-1\n
     screen row 0\n
     ...
     screen row ROWS-1          <- no trailing newline

   Scrollback lines are rendered once, when they scroll off the top of the
   screen.  Screen rows are re-rendered when libvterm reports damage.  */

#include <emacs-module.h>
#include <vterm.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__declspec (dllexport) int plugin_is_GPL_compatible;

#define MAX_EVENTS 64
#define MAX_OSC_LEN 8192

/* Symbols and constants, kept as global references.  */
static emacs_value Qnil, Qt, Qface, Qlist, Qcons, Qinsert, Qput_text_property,
  Qdelete_region, Qgoto_char, Qforward_line, Qpoint, Qpoint_min, Qpoint_max,
  Qline_end_position, Qdefalias, Qprovide, Qbold, Qitalic, Qwave, Qtitle,
  Qbell, Qcursor_visible, Qosc, Kforeground, Kbackground, Kweight, Kslant,
  Kunderline, Kstyle, Kinverse_video, Kstrike_through, Snewline;

typedef struct
{
  int cols;
  VTermScreenCell cells[];
} SbLine;

typedef struct
{
  uint8_t r, g, b;
} Rgb;

typedef struct
{
  int cmd;                      /* OSC number */
  char *text;
} OscEvent;

typedef struct
{
  VTerm *vt;
  VTermScreen *screen;
  VTermState *state;
  int rows, cols;
  VTermScreenCell *rowbuf;      /* one screen row, scratch space */
  int sb_max;                   /* scrollback lines kept in the buffer */

  /* Changes not yet reflected in the buffer.  */
  SbLine **pending;             /* lines that scrolled off, oldest first */
  int n_pending, pending_cap;
  bool sb_cleared;
  int invalid_start, invalid_end;

  /* What the buffer currently contains.  */
  int buf_sb_lines;
  int buf_screen_lines;

  VTermPos cursor;
  bool cursor_visible, cursor_visible_changed;
  bool bell;

  char *title;
  bool title_changed;
  char title_buf[1024];
  size_t title_len;

  char osc_buf[MAX_OSC_LEN];
  size_t osc_len;
  OscEvent events[MAX_EVENTS];
  int n_events;

  /* Bytes to send back to the pseudo console.  */
  char *out;
  size_t out_len, out_cap;

  /* Colors 0-15 from the current theme.  */
  Rgb palette[16];
  bool palette_set[16];

  /* Emacs' default colors and the minimum contrast between the text and
     its background (0 for no adjustment).  */
  Rgb default_fg, default_bg;
  bool defaults_set;
  double min_contrast;
} Term;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */

typedef struct
{
  char *data;
  size_t len, cap;
} Buf;

static void
buf_put (Buf *b, const char *s, size_t n)
{
  if (b->len + n > b->cap)
    {
      size_t cap = b->cap ? b->cap * 2 : 256;
      while (cap < b->len + n)
        cap *= 2;
      b->data = realloc (b->data, cap);
      b->cap = cap;
    }
  memcpy (b->data + b->len, s, n);
  b->len += n;
}

static void
buf_utf8 (Buf *b, uint32_t c)
{
  char s[4];
  if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
    c = 0xFFFD;
  if (c < 0x80)
    {
      s[0] = (char) c;
      buf_put (b, s, 1);
    }
  else if (c < 0x800)
    {
      s[0] = (char) (0xC0 | (c >> 6));
      s[1] = (char) (0x80 | (c & 0x3F));
      buf_put (b, s, 2);
    }
  else if (c < 0x10000)
    {
      s[0] = (char) (0xE0 | (c >> 12));
      s[1] = (char) (0x80 | ((c >> 6) & 0x3F));
      s[2] = (char) (0x80 | (c & 0x3F));
      buf_put (b, s, 3);
    }
  else
    {
      s[0] = (char) (0xF0 | (c >> 18));
      s[1] = (char) (0x80 | ((c >> 12) & 0x3F));
      s[2] = (char) (0x80 | ((c >> 6) & 0x3F));
      s[3] = (char) (0x80 | (c & 0x3F));
      buf_put (b, s, 4);
    }
}

static emacs_value
call (emacs_env *env, emacs_value fn, ptrdiff_t n, emacs_value *args)
{
  return env->funcall (env, fn, n, args);
}

static emacs_value
call0 (emacs_env *env, emacs_value fn)
{
  return env->funcall (env, fn, 0, NULL);
}

static emacs_value
call1 (emacs_env *env, emacs_value fn, emacs_value a)
{
  return env->funcall (env, fn, 1, &a);
}

static emacs_value
call2 (emacs_env *env, emacs_value fn, emacs_value a, emacs_value b)
{
  emacs_value args[2] = { a, b };
  return env->funcall (env, fn, 2, args);
}

static intmax_t
point (emacs_env *env)
{
  return env->extract_integer (env, call0 (env, Qpoint));
}

static intmax_t
point_max (emacs_env *env)
{
  return env->extract_integer (env, call0 (env, Qpoint_max));
}

static void
goto_char (emacs_env *env, intmax_t pos)
{
  call1 (env, Qgoto_char, env->make_integer (env, pos));
}

static void
forward_line (emacs_env *env, intmax_t n)
{
  call1 (env, Qforward_line, env->make_integer (env, n));
}

static void
delete_region (emacs_env *env, intmax_t start, intmax_t end)
{
  if (end > start)
    call2 (env, Qdelete_region, env->make_integer (env, start),
           env->make_integer (env, end));
}

static bool
failed (emacs_env *env)
{
  return env->non_local_exit_check (env) != emacs_funcall_exit_return;
}

/* Start of the screen region: the beginning of the (buf_screen_lines)th
   line from the end of the buffer.  */
static intmax_t
screen_start (Term *t, emacs_env *env)
{
  if (t->buf_screen_lines == 0)
    return point_max (env);
  goto_char (env, point_max (env));
  forward_line (env, -(t->buf_screen_lines - 1));
  return point (env);
}

/* ------------------------------------------------------------------ */
/* Scrollback                                                          */

static bool
cell_blank (const VTermScreenCell *c)
{
  return (c->chars[0] == 0 || c->chars[0] == ' ')
    && VTERM_COLOR_IS_DEFAULT_BG (&c->bg)
    && !c->attrs.reverse && !c->attrs.underline && !c->attrs.strike;
}

static int
trimmed_len (const VTermScreenCell *cells, int n)
{
  while (n > 0 && cell_blank (&cells[n - 1]))
    n--;
  return n;
}

static void
free_pending (Term *t)
{
  for (int i = 0; i < t->n_pending; i++)
    free (t->pending[i]);
  t->n_pending = 0;
}

/* ------------------------------------------------------------------ */
/* libvterm callbacks                                                  */

static void
invalidate (Term *t, int start, int end)
{
  if (start < 0)
    start = 0;
  if (end > t->rows)
    end = t->rows;
  if (start >= end)
    return;
  if (start < t->invalid_start)
    t->invalid_start = start;
  if (end > t->invalid_end)
    t->invalid_end = end;
}

static int
cb_damage (VTermRect rect, void *user)
{
  invalidate (user, rect.start_row, rect.end_row);
  return 1;
}

static int
cb_moverect (VTermRect dest, VTermRect src, void *user)
{
  invalidate (user, dest.start_row, dest.end_row);
  invalidate (user, src.start_row, src.end_row);
  return 1;
}

static int
cb_movecursor (VTermPos pos, VTermPos oldpos, int visible, void *user)
{
  Term *t = user;
  (void) visible;
  t->cursor = pos;
  invalidate (t, oldpos.row, oldpos.row + 1);
  invalidate (t, pos.row, pos.row + 1);
  return 1;
}

static int
cb_settermprop (VTermProp prop, VTermValue *val, void *user)
{
  Term *t = user;
  switch (prop)
    {
    case VTERM_PROP_CURSORVISIBLE:
      if (t->cursor_visible != (bool) val->boolean)
        {
          t->cursor_visible = val->boolean;
          t->cursor_visible_changed = true;
        }
      invalidate (t, t->cursor.row, t->cursor.row + 1);
      break;
    case VTERM_PROP_TITLE:
      {
        VTermStringFragment f = val->string;
        if (f.initial)
          t->title_len = 0;
        if (t->title_len + f.len < sizeof t->title_buf)
          {
            memcpy (t->title_buf + t->title_len, f.str, f.len);
            t->title_len += f.len;
          }
        if (f.final)
          {
            free (t->title);
            t->title = malloc (t->title_len + 1);
            memcpy (t->title, t->title_buf, t->title_len);
            t->title[t->title_len] = '\0';
            t->title_changed = true;
          }
      }
      break;
    default:
      break;
    }
  return 1;
}

static int
cb_bell (void *user)
{
  ((Term *) user)->bell = true;
  return 1;
}

static int
cb_resize (int rows, int cols, void *user)
{
  Term *t = user;
  t->rows = rows;
  t->cols = cols;
  t->rowbuf = realloc (t->rowbuf, sizeof (VTermScreenCell) * cols);
  t->invalid_start = 0;
  t->invalid_end = rows;
  return 1;
}

static int
cb_sb_pushline (int cols, const VTermScreenCell *cells, void *user)
{
  Term *t = user;
  int n = trimmed_len (cells, cols);
  SbLine *line = malloc (sizeof (SbLine) + sizeof (VTermScreenCell) * n);

  line->cols = n;
  memcpy (line->cells, cells, sizeof (VTermScreenCell) * n);
  if (t->n_pending == t->sb_max)
    {
      /* More output than the buffer keeps anyway: drop the oldest.  */
      free (t->pending[0]);
      memmove (t->pending, t->pending + 1,
               sizeof (SbLine *) * (t->n_pending - 1));
      t->n_pending--;
    }
  if (t->n_pending == t->pending_cap)
    {
      t->pending_cap = t->pending_cap ? t->pending_cap * 2 : 64;
      t->pending = realloc (t->pending, sizeof (SbLine *) * t->pending_cap);
    }
  t->pending[t->n_pending++] = line;
  return 1;
}

/* There is deliberately no sb_popline callback.  When the window grows,
   ConPTY keeps its screen anchored at the top and adds blank rows at the
   bottom; it never brings lines back from its history.  libvterm has to do
   the same, or ConPTY's repaint would overwrite the restored lines.  */

static int
cb_sb_clear (void *user)
{
  Term *t = user;
  free_pending (t);
  t->sb_cleared = true;
  return 1;
}

static int
fb_osc (int command, VTermStringFragment frag, void *user)
{
  Term *t = user;
  if (frag.initial)
    t->osc_len = 0;
  if (t->osc_len + frag.len < MAX_OSC_LEN)
    {
      memcpy (t->osc_buf + t->osc_len, frag.str, frag.len);
      t->osc_len += frag.len;
    }
  if (frag.final && t->n_events < MAX_EVENTS)
    {
      OscEvent *e = &t->events[t->n_events++];
      e->cmd = command;
      e->text = malloc (t->osc_len + 1);
      memcpy (e->text, t->osc_buf, t->osc_len);
      e->text[t->osc_len] = '\0';
    }
  return 1;
}

static void
cb_output (const char *s, size_t len, void *user)
{
  Term *t = user;
  if (t->out_len + len > t->out_cap)
    {
      size_t cap = t->out_cap ? t->out_cap * 2 : 1024;
      while (cap < t->out_len + len)
        cap *= 2;
      t->out = realloc (t->out, cap);
      t->out_cap = cap;
    }
  memcpy (t->out + t->out_len, s, len);
  t->out_len += len;
}

static const VTermScreenCallbacks screen_callbacks = {
  .damage = cb_damage,
  .moverect = cb_moverect,
  .movecursor = cb_movecursor,
  .settermprop = cb_settermprop,
  .bell = cb_bell,
  .resize = cb_resize,
  .sb_pushline = cb_sb_pushline,
  .sb_clear = cb_sb_clear,
};

static const VTermStateFallbacks fallbacks = {
  .osc = fb_osc,
};

/* ------------------------------------------------------------------ */
/* Rendering                                                           */

static Rgb
color_rgb (Term *t, const VTermColor *c)
{
  VTermColor col = *c;

  if (VTERM_COLOR_IS_INDEXED (&col) && col.indexed.idx < 16
      && t->palette_set[col.indexed.idx])
    return t->palette[col.indexed.idx];
  vterm_screen_convert_color_to_rgb (t->screen, &col);
  return (Rgb) { col.rgb.red, col.rgb.green, col.rgb.blue };
}

static emacs_value
rgb_value (emacs_env *env, Rgb c)
{
  char s[8];
  snprintf (s, sizeof s, "#%02x%02x%02x", c.r, c.g, c.b);
  return env->make_string (env, s, 7);
}

static bool
parse_rgb (const char *s, Rgb *c)
{
  unsigned r, g, b;
  if (strlen (s) != 7 || sscanf (s, "#%2x%2x%2x", &r, &g, &b) != 3)
    return false;
  *c = (Rgb) { r, g, b };
  return true;
}

/* Contrast adjustment, using the WCAG definitions of relative luminance
   and contrast ratio.  */

static double srgb_linear[256];

static double
luminance (Rgb c)
{
  return 0.2126 * srgb_linear[c.r] + 0.7152 * srgb_linear[c.g]
    + 0.0722 * srgb_linear[c.b];
}

static double
contrast (double l1, double l2)
{
  return l1 > l2 ? (l1 + 0.05) / (l2 + 0.05) : (l2 + 0.05) / (l1 + 0.05);
}

static Rgb
mix (Rgb a, Rgb b, double k)
{
  return (Rgb) { lround (a.r + (b.r - a.r) * k),
                 lround (a.g + (b.g - a.g) * k),
                 lround (a.b + (b.b - a.b) * k) };
}

/* FG, moved towards black or white as little as possible so that it
   contrasts with BG by at least RATIO.  */
static Rgb
ensure_contrast (Rgb fg, Rgb bg, double ratio)
{
  static const Rgb black = { 0, 0, 0 }, white = { 255, 255, 255 };
  double lbg = luminance (bg);
  Rgb target;
  double lo = 0, hi = 1;

  if (contrast (luminance (fg), lbg) >= ratio)
    return fg;
  /* Keep FG on its side of BG (darker or lighter) unless only the other
     side gets far enough.  */
  bool darker = luminance (fg) <= lbg;
  if (contrast (darker ? 0 : 1, lbg) < ratio
      && contrast (darker ? 1 : 0, lbg) > contrast (darker ? 0 : 1, lbg))
    darker = !darker;
  target = darker ? black : white;
  if (contrast (luminance (target), lbg) < ratio)
    return target;
  for (int i = 0; i < 12; i++)
    {
      double k = (lo + hi) / 2;
      if (contrast (luminance (mix (fg, target, k)), lbg) >= ratio)
        hi = k;
      else
        lo = k;
    }
  return mix (fg, target, hi);
}

static bool
style_default (const VTermScreenCell *c)
{
  return VTERM_COLOR_IS_DEFAULT_FG (&c->fg) && VTERM_COLOR_IS_DEFAULT_BG (&c->bg)
    && !c->attrs.bold && !c->attrs.italic && !c->attrs.underline
    && !c->attrs.reverse && !c->attrs.strike;
}

static bool
style_eq (const VTermScreenCell *a, const VTermScreenCell *b)
{
  return a->attrs.bold == b->attrs.bold
    && a->attrs.italic == b->attrs.italic
    && a->attrs.underline == b->attrs.underline
    && a->attrs.reverse == b->attrs.reverse
    && a->attrs.strike == b->attrs.strike
    && vterm_color_is_equal (&a->fg, &b->fg)
    && vterm_color_is_equal (&a->bg, &b->bg);
}

static emacs_value
make_face (Term *t, emacs_env *env, const VTermScreenCell *c)
{
  emacs_value a[14];
  int n = 0;
  bool fg_default = VTERM_COLOR_IS_DEFAULT_FG (&c->fg);
  bool bg_default = VTERM_COLOR_IS_DEFAULT_BG (&c->bg);
  Rgb fg = fg_default ? t->default_fg : color_rgb (t, &c->fg);
  Rgb bg = bg_default ? t->default_bg : color_rgb (t, &c->bg);

  /* Theme colors are trusted to work together; adjust only when the
     terminal picked at least one of the two.  */
  if (t->min_contrast > 1 && t->defaults_set && !(fg_default && bg_default))
    {
      Rgb adjusted = ensure_contrast (fg, bg, t->min_contrast);
      if (adjusted.r != fg.r || adjusted.g != fg.g || adjusted.b != fg.b)
        {
          fg = adjusted;
          fg_default = false;
        }
    }
  if (!fg_default)
    {
      a[n++] = Kforeground;
      a[n++] = rgb_value (env, fg);
    }
  if (!bg_default)
    {
      a[n++] = Kbackground;
      a[n++] = rgb_value (env, bg);
    }
  if (c->attrs.bold)
    {
      a[n++] = Kweight;
      a[n++] = Qbold;
    }
  if (c->attrs.italic)
    {
      a[n++] = Kslant;
      a[n++] = Qitalic;
    }
  if (c->attrs.underline)
    {
      a[n++] = Kunderline;
      if (c->attrs.underline == VTERM_UNDERLINE_CURLY)
        a[n++] = call2 (env, Qlist, Kstyle, Qwave);
      else
        a[n++] = Qt;
    }
  if (c->attrs.reverse)
    {
      a[n++] = Kinverse_video;
      a[n++] = Qt;
    }
  if (c->attrs.strike)
    {
      a[n++] = Kstrike_through;
      a[n++] = Qt;
    }
  return call (env, Qlist, n, a);
}

/* Insert NCELLS cells at point, followed by a newline if NEWLINE.  */
static void
render_cells (Term *t, emacs_env *env, const VTermScreenCell *cells,
              int ncells, bool newline)
{
  emacs_value *args = malloc (sizeof (emacs_value) * (ncells + 2));
  int nargs = 0;
  Buf run = { 0 };
  intmax_t run_chars = 0;
  const VTermScreenCell *style = NULL;

  for (int col = 0; col < ncells;)
    {
      const VTermScreenCell *c = &cells[col];
      if (c->chars[0] == (uint32_t) -1)
        {
          col++;
          continue;
        }
      if (style && !style_eq (style, c))
        {
          emacs_value s = env->make_string (env, run.data, run.len);
          if (!style_default (style))
            {
              emacs_value pa[5] = { env->make_integer (env, 0),
                env->make_integer (env, run_chars), Qface,
                make_face (t, env, style), s };
              call (env, Qput_text_property, 5, pa);
            }
          args[nargs++] = s;
          run.len = 0;
          run_chars = 0;
        }
      style = c;
      if (c->chars[0] == 0)
        {
          buf_put (&run, " ", 1);
          run_chars++;
        }
      else
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c->chars[i]; i++)
          {
            buf_utf8 (&run, c->chars[i]);
            run_chars++;
          }
      col += c->width > 0 ? c->width : 1;
    }
  if (style && run.len > 0)
    {
      emacs_value s = env->make_string (env, run.data, run.len);
      if (!style_default (style))
        {
          emacs_value pa[5] = { env->make_integer (env, 0),
            env->make_integer (env, run_chars), Qface,
            make_face (t, env, style), s };
          call (env, Qput_text_property, 5, pa);
        }
      args[nargs++] = s;
    }
  if (newline)
    args[nargs++] = Snewline;
  if (nargs > 0)
    call (env, Qinsert, nargs, args);
  free (run.data);
  free (args);
}

static void
fetch_row (Term *t, int row)
{
  VTermPos pos = { row, 0 };
  for (pos.col = 0; pos.col < t->cols; pos.col++)
    vterm_screen_get_cell (t->screen, pos, &t->rowbuf[pos.col]);
}

static void
render_row (Term *t, emacs_env *env, int row)
{
  int n;
  fetch_row (t, row);
  n = trimmed_len (t->rowbuf, t->cols);
  /* Keep the cursor's cell inside the line so point can sit on it.  */
  if (row == t->cursor.row && t->cursor.col > n)
    n = t->cursor.col < t->cols ? t->cursor.col : t->cols;
  render_cells (t, env, t->rowbuf, n, row < t->rows - 1);
}

/* Number of buffer characters before column COL of screen row ROW.  */
static intmax_t
char_offset (Term *t, int row, int col)
{
  intmax_t off = 0;
  fetch_row (t, row);
  for (int c = 0; c < col && c < t->cols;)
    {
      const VTermScreenCell *cell = &t->rowbuf[c];
      if (cell->chars[0] == (uint32_t) -1)
        {
          c++;
          continue;
        }
      if (cell->chars[0] == 0)
        off++;
      else
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i]; i++)
          off++;
      c += cell->width > 0 ? cell->width : 1;
    }
  return off;
}

static void
redraw (Term *t, emacs_env *env)
{
  intmax_t s, b, e;

  if (t->sb_cleared)
    {
      if (t->buf_sb_lines > 0)
        {
          s = screen_start (t, env);
          delete_region (env, 1, s);
          t->buf_sb_lines = 0;
        }
      t->sb_cleared = false;
    }

  if (t->n_pending > 0)
    {
      goto_char (env, screen_start (t, env));
      for (int i = 0; i < t->n_pending; i++)
        {
          SbLine *line = t->pending[i];
          render_cells (t, env, line->cells, line->cols, true);
          if (failed (env))
            return;
        }
      t->buf_sb_lines += t->n_pending;
      free_pending (t);
    }

  if (t->buf_sb_lines > t->sb_max)
    {
      goto_char (env, 1);
      forward_line (env, t->buf_sb_lines - t->sb_max);
      delete_region (env, 1, point (env));
      t->buf_sb_lines = t->sb_max;
    }

  if (t->buf_screen_lines != t->rows)
    {
      s = screen_start (t, env);
      delete_region (env, s, point_max (env));
      goto_char (env, s);
      t->invalid_start = 0;
      t->invalid_end = t->rows;
    }
  else if (t->invalid_start < t->invalid_end)
    {
      goto_char (env, screen_start (t, env));
      forward_line (env, t->invalid_start);
      b = point (env);
      if (t->invalid_end >= t->rows)
        e = point_max (env);
      else
        {
          forward_line (env, t->invalid_end - t->invalid_start);
          e = point (env);
        }
      delete_region (env, b, e);
      goto_char (env, b);
    }
  for (int row = t->invalid_start; row < t->invalid_end; row++)
    {
      render_row (t, env, row);
      if (failed (env))
        return;
    }
  t->buf_screen_lines = t->rows;
  t->invalid_start = t->rows;
  t->invalid_end = 0;
}

/* ------------------------------------------------------------------ */
/* Lisp interface                                                      */

static void
term_finalize (void *ptr)
{
  Term *t = ptr;
  if (!t)
    return;
  free_pending (t);
  for (int i = 0; i < t->n_events; i++)
    free (t->events[i].text);
  vterm_free (t->vt);
  free (t->pending);
  free (t->rowbuf);
  free (t->out);
  free (t->title);
  free (t);
}

static Term *
get_term (emacs_env *env, emacs_value v)
{
  return env->get_user_ptr (env, v);
}

static emacs_value
take_output (Term *t, emacs_env *env)
{
  emacs_value s;
  if (t->out_len == 0)
    return Qnil;
  s = env->make_unibyte_string (env, t->out, t->out_len);
  t->out_len = 0;
  return s;
}

static char *
copy_string (emacs_env *env, emacs_value v, ptrdiff_t *len)
{
  ptrdiff_t size = 0;
  char *s;
  if (!env->copy_string_contents (env, v, NULL, &size))
    return NULL;
  s = malloc (size);
  if (!env->copy_string_contents (env, v, s, &size))
    {
      free (s);
      return NULL;
    }
  *len = size - 1;
  return s;
}

static emacs_value
Fwterm_new (emacs_env *env, ptrdiff_t nargs, emacs_value *args, void *data)
{
  int rows = (int) env->extract_integer (env, args[0]);
  int cols = (int) env->extract_integer (env, args[1]);
  int sb_max = (int) env->extract_integer (env, args[2]);
  Term *t;
  (void) nargs;
  (void) data;

  if (failed (env))
    return Qnil;
  if (rows < 1)
    rows = 1;
  if (cols < 1)
    cols = 1;
  if (sb_max < 1)
    sb_max = 1;

  t = calloc (1, sizeof (Term));
  t->vt = vterm_new (rows, cols);
  t->rows = rows;
  t->cols = cols;
  t->rowbuf = malloc (sizeof (VTermScreenCell) * cols);
  t->sb_max = sb_max;
  t->cursor_visible = true;
  t->invalid_start = rows;
  t->invalid_end = 0;

  vterm_set_utf8 (t->vt, 1);
  vterm_output_set_callback (t->vt, cb_output, t);
  t->screen = vterm_obtain_screen (t->vt);
  t->state = vterm_obtain_state (t->vt);
  vterm_screen_set_callbacks (t->screen, &screen_callbacks, t);
  vterm_screen_set_unrecognised_fallbacks (t->screen, &fallbacks, t);
  vterm_screen_set_damage_merge (t->screen, VTERM_DAMAGE_SCROLL);
  vterm_screen_enable_altscreen (t->screen, 1);
  /* ConPTY reflows its buffer on resize and then repaints the screen;
     reflowing here too keeps both layouts in agreement.  */
  vterm_screen_enable_reflow (t->screen, true);
  vterm_screen_reset (t->screen, 1);

  return env->make_user_ptr (env, term_finalize, t);
}

static emacs_value
Fwterm_write_input (emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data)
{
  Term *t = get_term (env, args[0]);
  ptrdiff_t len;
  char *bytes;
  (void) nargs;
  (void) data;

  if (!t || !(bytes = copy_string (env, args[1], &len)))
    return Qnil;
  vterm_input_write (t->vt, bytes, len);
  vterm_screen_flush_damage (t->screen);
  free (bytes);
  return take_output (t, env);
}

static emacs_value
Fwterm_redraw (emacs_env *env, ptrdiff_t nargs, emacs_value *args,
               void *data)
{
  Term *t = get_term (env, args[0]);
  intmax_t s, line_start, line_end, off;
  (void) nargs;
  (void) data;

  if (!t)
    return Qnil;
  redraw (t, env);
  if (failed (env))
    return Qnil;

  /* Put point on the cursor.  */
  s = screen_start (t, env);
  forward_line (env, t->cursor.row);
  line_start = point (env);
  line_end = env->extract_integer (env, call0 (env, Qline_end_position));
  off = char_offset (t, t->cursor.row, t->cursor.col);
  goto_char (env, line_start + off < line_end ? line_start + off : line_end);
  return env->make_integer (env, s);
}

static emacs_value
Fwterm_set_size (emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                 void *data)
{
  Term *t = get_term (env, args[0]);
  int rows = (int) env->extract_integer (env, args[1]);
  int cols = (int) env->extract_integer (env, args[2]);
  (void) nargs;
  (void) data;

  if (!t || failed (env) || rows < 1 || cols < 1)
    return Qnil;
  if (rows == t->rows && cols == t->cols)
    return Qnil;
  vterm_set_size (t->vt, rows, cols);
  vterm_screen_flush_damage (t->screen);
  return Qt;
}

static const struct
{
  const char *name;
  VTermKey key;
} key_names[] = {
  { "return", VTERM_KEY_ENTER },
  { "tab", VTERM_KEY_TAB },
  { "backspace", VTERM_KEY_BACKSPACE },
  { "escape", VTERM_KEY_ESCAPE },
  { "up", VTERM_KEY_UP },
  { "down", VTERM_KEY_DOWN },
  { "left", VTERM_KEY_LEFT },
  { "right", VTERM_KEY_RIGHT },
  { "insert", VTERM_KEY_INS },
  { "delete", VTERM_KEY_DEL },
  { "deletechar", VTERM_KEY_DEL },
  { "home", VTERM_KEY_HOME },
  { "end", VTERM_KEY_END },
  { "prior", VTERM_KEY_PAGEUP },
  { "next", VTERM_KEY_PAGEDOWN },
  { "kp-0", VTERM_KEY_KP_0 },
  { "kp-1", VTERM_KEY_KP_1 },
  { "kp-2", VTERM_KEY_KP_2 },
  { "kp-3", VTERM_KEY_KP_3 },
  { "kp-4", VTERM_KEY_KP_4 },
  { "kp-5", VTERM_KEY_KP_5 },
  { "kp-6", VTERM_KEY_KP_6 },
  { "kp-7", VTERM_KEY_KP_7 },
  { "kp-8", VTERM_KEY_KP_8 },
  { "kp-9", VTERM_KEY_KP_9 },
  { "kp-multiply", VTERM_KEY_KP_MULT },
  { "kp-add", VTERM_KEY_KP_PLUS },
  { "kp-separator", VTERM_KEY_KP_COMMA },
  { "kp-subtract", VTERM_KEY_KP_MINUS },
  { "kp-decimal", VTERM_KEY_KP_PERIOD },
  { "kp-divide", VTERM_KEY_KP_DIVIDE },
  { "kp-enter", VTERM_KEY_KP_ENTER },
};

/* (wterm--key TERM NAME MODS): NAME is a key name such as "up" or "f5",
   MODS a bitmask (1 shift, 2 meta, 4 control).  Returns bytes to send.  */
static emacs_value
Fwterm_key (emacs_env *env, ptrdiff_t nargs, emacs_value *args, void *data)
{
  Term *t = get_term (env, args[0]);
  int mods = (int) env->extract_integer (env, args[2]);
  ptrdiff_t len;
  char *name;
  VTermKey key = VTERM_KEY_NONE;
  (void) nargs;
  (void) data;

  if (!t || !(name = copy_string (env, args[1], &len)))
    return Qnil;
  if (name[0] == 'f' && name[1] >= '1' && name[1] <= '9')
    key = VTERM_KEY_FUNCTION (atoi (name + 1));
  else
    for (size_t i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
      if (strcmp (name, key_names[i].name) == 0)
        {
          key = key_names[i].key;
          break;
        }
  free (name);
  if (key != VTERM_KEY_NONE)
    vterm_keyboard_key (t->vt, key, (VTermModifier) (mods & 7));
  return take_output (t, env);
}

/* (wterm--char TERM CHAR MODS): encode a character with modifiers.  */
static emacs_value
Fwterm_char (emacs_env *env, ptrdiff_t nargs, emacs_value *args, void *data)
{
  Term *t = get_term (env, args[0]);
  intmax_t c = env->extract_integer (env, args[1]);
  int mods = (int) env->extract_integer (env, args[2]);
  (void) nargs;
  (void) data;

  if (!t || failed (env))
    return Qnil;
  vterm_keyboard_unichar (t->vt, (uint32_t) c, (VTermModifier) (mods & 7));
  return take_output (t, env);
}

/* (wterm--paste TERM START): bracketed paste start/end sequences, if the
   application asked for them.  */
static emacs_value
Fwterm_paste (emacs_env *env, ptrdiff_t nargs, emacs_value *args, void *data)
{
  Term *t = get_term (env, args[0]);
  (void) nargs;
  (void) data;

  if (!t)
    return Qnil;
  if (env->is_not_nil (env, args[1]))
    vterm_keyboard_start_paste (t->vt);
  else
    vterm_keyboard_end_paste (t->vt);
  return take_output (t, env);
}

static emacs_value
Fwterm_focus (emacs_env *env, ptrdiff_t nargs, emacs_value *args, void *data)
{
  Term *t = get_term (env, args[0]);
  (void) nargs;
  (void) data;

  if (!t)
    return Qnil;
  if (env->is_not_nil (env, args[1]))
    vterm_state_focus_in (t->state);
  else
    vterm_state_focus_out (t->state);
  return take_output (t, env);
}

/* Copy the "#rrggbb" string V into C.  Return false if V is nil or not
   such a string.  */
static bool
value_rgb (emacs_env *env, emacs_value v, Rgb *c)
{
  char s[16];
  ptrdiff_t size = sizeof s;

  if (!env->is_not_nil (env, v))
    return false;
  if (!env->copy_string_contents (env, v, s, &size))
    {
      env->non_local_exit_clear (env);
      return false;
    }
  return parse_rgb (s, c);
}

/* (wterm--set-palette TERM VECTOR): VECTOR holds 16 "#rrggbb" color
   strings (or nil) used for the ANSI colors 0-15.  */
static emacs_value
Fwterm_set_palette (emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                    void *data)
{
  Term *t = get_term (env, args[0]);
  (void) nargs;
  (void) data;

  if (!t)
    return Qnil;
  for (int i = 0; i < 16; i++)
    {
      emacs_value v = env->vec_get (env, args[1], i);
      if (failed (env))
        return Qnil;
      t->palette_set[i] = value_rgb (env, v, &t->palette[i]);
    }
  invalidate (t, 0, t->rows);
  return Qnil;
}

/* (wterm--set-contrast TERM FG BG RATIO): FG and BG are Emacs' default
   colors as "#rrggbb" strings, RATIO a float or nil for no adjustment.  */
static emacs_value
Fwterm_set_contrast (emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                     void *data)
{
  Term *t = get_term (env, args[0]);
  (void) nargs;
  (void) data;

  if (!t)
    return Qnil;
  t->defaults_set = value_rgb (env, args[1], &t->default_fg)
    && value_rgb (env, args[2], &t->default_bg);
  t->min_contrast = 0;
  if (env->is_not_nil (env, args[3]))
    {
      t->min_contrast = env->extract_float (env, args[3]);
      if (failed (env))
        {
          env->non_local_exit_clear (env);
          t->min_contrast = 0;
        }
    }
  invalidate (t, 0, t->rows);
  return Qnil;
}

static emacs_value
Fwterm_clear_scrollback (emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                         void *data)
{
  Term *t = get_term (env, args[0]);
  (void) nargs;
  (void) data;

  if (t)
    {
      free_pending (t);
      t->sb_cleared = true;
    }
  return Qnil;
}

/* (wterm--pop-events TERM): list of things that happened since the last
   call: (title . STRING), (cursor-visible . BOOL), (bell . t) and
   (osc NUMBER . STRING) for OSC sequences libvterm doesn't handle.  */
static emacs_value
Fwterm_pop_events (emacs_env *env, ptrdiff_t nargs, emacs_value *args,
                   void *data)
{
  Term *t = get_term (env, args[0]);
  emacs_value list = Qnil;
  (void) nargs;
  (void) data;

  if (!t)
    return Qnil;
  for (int i = t->n_events - 1; i >= 0; i--)
    {
      OscEvent *e = &t->events[i];
      emacs_value payload
        = call2 (env, Qcons, env->make_integer (env, e->cmd),
                 env->make_string (env, e->text, strlen (e->text)));
      if (failed (env))
        {
          /* Not valid UTF-8; drop it.  */
          env->non_local_exit_clear (env);
          free (e->text);
          continue;
        }
      list = call2 (env, Qcons, call2 (env, Qcons, Qosc, payload), list);
      free (e->text);
    }
  t->n_events = 0;
  if (t->bell)
    {
      list = call2 (env, Qcons, call2 (env, Qcons, Qbell, Qt), list);
      t->bell = false;
    }
  if (t->cursor_visible_changed)
    {
      list = call2 (env, Qcons,
                    call2 (env, Qcons, Qcursor_visible,
                           t->cursor_visible ? Qt : Qnil), list);
      t->cursor_visible_changed = false;
    }
  if (t->title_changed && t->title)
    {
      emacs_value title = env->make_string (env, t->title, strlen (t->title));
      if (failed (env))
        env->non_local_exit_clear (env);
      else
        list = call2 (env, Qcons, call2 (env, Qcons, Qtitle, title), list);
      t->title_changed = false;
    }
  return list;
}

/* ------------------------------------------------------------------ */
/* Initialization                                                      */

static emacs_value
global_sym (emacs_env *env, const char *name)
{
  return env->make_global_ref (env, env->intern (env, name));
}

static void
defun (emacs_env *env, const char *name, ptrdiff_t min, ptrdiff_t max,
       emacs_value (*fn) (emacs_env *, ptrdiff_t, emacs_value *, void *),
       const char *doc)
{
  emacs_value f = env->make_function (env, min, max, fn, doc, NULL);
  call2 (env, Qdefalias, env->intern (env, name), f);
}

__declspec (dllexport) int
emacs_module_init (struct emacs_runtime *rt)
{
  emacs_env *env;

  if (rt->size < (ptrdiff_t) sizeof *rt)
    return 1;
  env = rt->get_environment (rt);
  if (env->size < (ptrdiff_t) sizeof (struct emacs_env_28))
    return 2;

  Qnil = global_sym (env, "nil");
  Qt = global_sym (env, "t");
  Qface = global_sym (env, "face");
  Qlist = global_sym (env, "list");
  Qcons = global_sym (env, "cons");
  Qinsert = global_sym (env, "insert");
  Qput_text_property = global_sym (env, "put-text-property");
  Qdelete_region = global_sym (env, "delete-region");
  Qgoto_char = global_sym (env, "goto-char");
  Qforward_line = global_sym (env, "forward-line");
  Qpoint = global_sym (env, "point");
  Qpoint_min = global_sym (env, "point-min");
  Qpoint_max = global_sym (env, "point-max");
  Qline_end_position = global_sym (env, "line-end-position");
  Qdefalias = global_sym (env, "defalias");
  Qprovide = global_sym (env, "provide");
  Qbold = global_sym (env, "bold");
  Qitalic = global_sym (env, "italic");
  Qwave = global_sym (env, "wave");
  Qtitle = global_sym (env, "title");
  Qbell = global_sym (env, "bell");
  Qcursor_visible = global_sym (env, "cursor-visible");
  Qosc = global_sym (env, "osc");
  Kforeground = global_sym (env, ":foreground");
  Kbackground = global_sym (env, ":background");
  Kweight = global_sym (env, ":weight");
  Kslant = global_sym (env, ":slant");
  Kunderline = global_sym (env, ":underline");
  Kstyle = global_sym (env, ":style");
  Kinverse_video = global_sym (env, ":inverse-video");
  Kstrike_through = global_sym (env, ":strike-through");
  Snewline = env->make_global_ref (env, env->make_string (env, "\n", 1));

  for (int i = 0; i < 256; i++)
    {
      double v = i / 255.0;
      srgb_linear[i] = v <= 0.04045 ? v / 12.92 : pow ((v + 0.055) / 1.055, 2.4);
    }

  defun (env, "wterm--new", 3, 3, Fwterm_new,
         "Create a terminal: (wterm--new ROWS COLS SCROLLBACK).");
  defun (env, "wterm--write-input", 2, 2, Fwterm_write_input,
         "Feed process output to TERM.  Return bytes to send back, or nil.");
  defun (env, "wterm--redraw", 1, 1, Fwterm_redraw,
         "Render TERM into the current buffer.  Return the screen start.");
  defun (env, "wterm--set-size", 3, 3, Fwterm_set_size,
         "Resize TERM: (wterm--set-size TERM ROWS COLS).");
  defun (env, "wterm--key", 3, 3, Fwterm_key,
         "Encode special key NAME with MODS.  Return bytes to send.");
  defun (env, "wterm--char", 3, 3, Fwterm_char,
         "Encode character CHAR with MODS.  Return bytes to send.");
  defun (env, "wterm--paste", 2, 2, Fwterm_paste,
         "Return the bracketed paste start (START non-nil) or end sequence.");
  defun (env, "wterm--focus", 2, 2, Fwterm_focus,
         "Report focus in (non-nil) or out.  Return bytes to send.");
  defun (env, "wterm--set-palette", 2, 2, Fwterm_set_palette,
         "Set the 16 ANSI colors of TERM from a vector of color strings.");
  defun (env, "wterm--set-contrast", 4, 4, Fwterm_set_contrast,
         "Set TERM's default colors and minimum text contrast.");
  defun (env, "wterm--clear-scrollback", 1, 1, Fwterm_clear_scrollback,
         "Discard TERM's scrollback.");
  defun (env, "wterm--pop-events", 1, 1, Fwterm_pop_events,
         "Return and clear the events TERM collected.");

  call1 (env, Qprovide, env->intern (env, "wterm-module"));
  return 0;
}

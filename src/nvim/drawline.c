// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

// drawline.c: Functions for drawing window lines on the screen.
// This is the middle level, drawscreen.c is the top and grid.c the lower level.

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvim/arabic.h"
#include "nvim/ascii.h"
#include "nvim/buffer.h"
#include "nvim/charset.h"
#include "nvim/cursor.h"
#include "nvim/cursor_shape.h"
#include "nvim/decoration.h"
#include "nvim/decoration_provider.h"
#include "nvim/diff.h"
#include "nvim/drawline.h"
#include "nvim/drawscreen.h"
#include "nvim/eval.h"
#include "nvim/extmark_defs.h"
#include "nvim/fold.h"
#include "nvim/garray.h"
#include "nvim/globals.h"
#include "nvim/grid.h"
#include "nvim/highlight.h"
#include "nvim/highlight_group.h"
#include "nvim/indent.h"
#include "nvim/mark.h"
#include "nvim/match.h"
#include "nvim/mbyte.h"
#include "nvim/memline.h"
#include "nvim/memory.h"
#include "nvim/move.h"
#include "nvim/option.h"
#include "nvim/plines.h"
#include "nvim/pos.h"
#include "nvim/quickfix.h"
#include "nvim/sign.h"
#include "nvim/spell.h"
#include "nvim/state.h"
#include "nvim/statusline.h"
#include "nvim/strings.h"
#include "nvim/syntax.h"
#include "nvim/terminal.h"
#include "nvim/types.h"
#include "nvim/ui.h"
#include "nvim/vim.h"

#define MB_FILLER_CHAR '<'  // character used when a double-width character
                            // doesn't fit.

/// possible draw states in win_line(), drawn in sequence.
typedef enum {
  WL_START = 0,  // nothing done yet
  WL_CMDLINE,    // cmdline window column
  WL_FOLD,       // 'foldcolumn'
  WL_SIGN,       // column for signs
  WL_NR,         // line number
  WL_STC,        // 'statuscolumn'
  WL_BRI,        // 'breakindent'
  WL_SBR,        // 'showbreak' or 'diff'
  WL_LINE,       // text in the line
} LineDrawState;

/// structure with variables passed between win_line() and other functions
typedef struct {
  LineDrawState draw_state;  ///< what to draw next

  linenr_T lnum;             ///< line number to be drawn
  foldinfo_T foldinfo;       ///< fold info for this line

  int startrow;              ///< first row in the window to be drawn
  int row;                   ///< row in the window, excl w_winrow

  colnr_T vcol;              ///< virtual column, before wrapping
  int col;                   ///< visual column on screen, after wrapping
  int boguscols;             ///< nonexistent columns added to "col" to force wrapping
  int vcol_off;              ///< offset for concealed characters

  int off;                   ///< offset relative start of line

  int cul_attr;              ///< set when 'cursorline' active
  int line_attr;             ///< attribute for the whole line
  int line_attr_lowprio;     ///< low-priority attribute for the line

  int fromcol;               ///< start of inverting
  int tocol;                 ///< end of inverting

  long vcol_sbr;             ///< virtual column after showbreak
  bool need_showbreak;       ///< overlong line, skipping first x chars

  int char_attr;             ///< attributes for next character

  int n_extra;               ///< number of extra bytes
  char *p_extra;             ///< string of extra chars, plus NUL, only used
                             ///< when c_extra and c_final are NUL
  char *p_extra_free;        ///< p_extra buffer that needs to be freed
  int extra_attr;            ///< attributes for p_extra
  int c_extra;               ///< extra chars, all the same
  int c_final;               ///< final char, mandatory if set

  // saved "extra" items for when draw_state becomes WL_LINE (again)
  int saved_n_extra;
  char *saved_p_extra;
  int saved_c_extra;
  int saved_c_final;
  int saved_char_attr;

  char extra[57];            ///< sign, line number and 'fdc' must fit in here

  hlf_T diff_hlf;            ///< type of diff highlighting

  int n_virt_lines;          ///< nr of virtual lines
  int filler_lines;          ///< nr of filler lines to be drawn
  int filler_todo;           ///< nr of filler lines still to do + 1
  SignTextAttrs sattrs[SIGN_SHOW_MAX];  ///< sign attributes for the sign column
} winlinevars_T;

/// for line_putchar. Contains the state that needs to be remembered from
/// putting one character to the next.
typedef struct {
  const char *p;
  int prev_c;   ///< previous Arabic character
  int prev_c1;  ///< first composing char for prev_c
} LineState;
#define LINE_STATE(p) { p, 0, 0 }

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "drawline.c.generated.h"
#endif

/// Advance **color_cols
///
/// @return  true when there are columns to draw.
static bool advance_color_col(int vcol, int **color_cols)
{
  while (**color_cols >= 0 && vcol > **color_cols) {
    (*color_cols)++;
  }
  return **color_cols >= 0;
}

/// Used when 'cursorlineopt' contains "screenline": compute the margins between
/// which the highlighting is used.
static void margin_columns_win(win_T *wp, int *left_col, int *right_col)
{
  // cache previous calculations depending on w_virtcol
  static int saved_w_virtcol;
  static win_T *prev_wp;
  static int prev_left_col;
  static int prev_right_col;
  static int prev_col_off;

  int cur_col_off = win_col_off(wp);
  int width1;
  int width2;

  if (saved_w_virtcol == wp->w_virtcol && prev_wp == wp
      && prev_col_off == cur_col_off) {
    *right_col = prev_right_col;
    *left_col = prev_left_col;
    return;
  }

  width1 = wp->w_width_inner - cur_col_off;
  width2 = width1 + win_col_off2(wp);

  *left_col = 0;
  *right_col = width1;

  if (wp->w_virtcol >= (colnr_T)width1) {
    *right_col = width1 + ((wp->w_virtcol - width1) / width2 + 1) * width2;
  }
  if (wp->w_virtcol >= (colnr_T)width1 && width2 > 0) {
    *left_col = (wp->w_virtcol - width1) / width2 * width2 + width1;
  }

  // cache values
  prev_left_col = *left_col;
  prev_right_col = *right_col;
  prev_wp = wp;
  saved_w_virtcol = wp->w_virtcol;
  prev_col_off = cur_col_off;
}

/// Put a single char from an UTF-8 buffer into a line buffer.
///
/// Handles composing chars and arabic shaping state.
static int line_putchar(buf_T *buf, LineState *s, schar_T *dest, int maxcells, bool rl, int vcol)
{
  const char *p = s->p;
  int cells = utf_ptr2cells(p);
  int c_len = utfc_ptr2len(p);
  int u8c, u8cc[MAX_MCO];
  if (cells > maxcells) {
    return -1;
  }
  u8c = utfc_ptr2char(p, u8cc);
  if (*p == TAB) {
    cells = MIN(tabstop_padding(vcol, buf->b_p_ts, buf->b_p_vts_array), maxcells);
    for (int c = 0; c < cells; c++) {
      schar_from_ascii(dest[c], ' ');
    }
    goto done;
  } else if ((uint8_t)(*p) < 0x80 && u8cc[0] == 0) {
    schar_from_ascii(dest[0], *p);
    s->prev_c = u8c;
  } else {
    if (p_arshape && !p_tbidi && ARABIC_CHAR(u8c)) {
      // Do Arabic shaping.
      int pc, pc1, nc;
      int pcc[MAX_MCO];
      int firstbyte = (uint8_t)(*p);

      // The idea of what is the previous and next
      // character depends on 'rightleft'.
      if (rl) {
        pc = s->prev_c;
        pc1 = s->prev_c1;
        nc = utf_ptr2char(p + c_len);
        s->prev_c1 = u8cc[0];
      } else {
        pc = utfc_ptr2char(p + c_len, pcc);
        nc = s->prev_c;
        pc1 = pcc[0];
      }
      s->prev_c = u8c;

      u8c = arabic_shape(u8c, &firstbyte, &u8cc[0], pc, pc1, nc);
    } else {
      s->prev_c = u8c;
    }
    schar_from_cc(dest[0], u8c, u8cc);
  }
  if (cells > 1) {
    dest[1][0] = 0;
  }
done:
  s->p += c_len;
  return cells;
}

static inline void provider_err_virt_text(linenr_T lnum, char *err)
{
  Decoration err_decor = DECORATION_INIT;
  int hl_err = syn_check_group(S_LEN("ErrorMsg"));
  kv_push(err_decor.virt_text,
          ((VirtTextChunk){ .text = err,
                            .hl_id = hl_err }));
  err_decor.virt_text_width = (int)mb_string2cells(err);
  decor_add_ephemeral(lnum - 1, 0, lnum - 1, 0, &err_decor, 0, 0);
}

static void draw_virt_text(win_T *wp, buf_T *buf, int col_off, int *end_col, int max_col,
                           int win_row)
{
  DecorState *state = &decor_state;
  int right_pos = max_col;
  bool do_eol = state->eol_col > -1;
  for (size_t i = 0; i < kv_size(state->active); i++) {
    DecorRange *item = &kv_A(state->active, i);
    if (!(item->start_row == state->row
          && (kv_size(item->decor.virt_text) || item->decor.ui_watched))) {
      continue;
    }
    if (item->win_col == -1) {
      if (item->decor.virt_text_pos == kVTRightAlign) {
        right_pos -= item->decor.virt_text_width;
        item->win_col = right_pos;
      } else if (item->decor.virt_text_pos == kVTEndOfLine && do_eol) {
        item->win_col = state->eol_col;
      } else if (item->decor.virt_text_pos == kVTWinCol) {
        item->win_col = MAX(item->decor.col + col_off, 0);
      }
    }
    if (item->win_col < 0) {
      continue;
    }
    int col = 0;
    if (item->decor.ui_watched) {
      // send mark position to UI
      col = item->win_col;
      WinExtmark m = { (NS)item->ns_id, item->mark_id, win_row, col };
      kv_push(win_extmark_arr, m);
    }
    if (kv_size(item->decor.virt_text)) {
      col = draw_virt_text_item(buf, item->win_col, item->decor.virt_text,
                                item->decor.hl_mode, max_col, item->win_col - col_off);
    }
    item->win_col = -2;  // deactivate
    if (item->decor.virt_text_pos == kVTEndOfLine && do_eol) {
      state->eol_col = col + 1;
    }

    *end_col = MAX(*end_col, col);
  }
}

static int draw_virt_text_item(buf_T *buf, int col, VirtText vt, HlMode hl_mode, int max_col,
                               int vcol)
{
  LineState s = LINE_STATE("");
  int virt_attr = 0;
  size_t virt_pos = 0;

  while (col < max_col) {
    if (!*s.p) {
      if (virt_pos >= kv_size(vt)) {
        break;
      }
      virt_attr = 0;
      do {
        s.p = kv_A(vt, virt_pos).text;
        int hl_id = kv_A(vt, virt_pos).hl_id;
        virt_attr = hl_combine_attr(virt_attr,
                                    hl_id > 0 ? syn_id2attr(hl_id) : 0);
        virt_pos++;
      } while (!s.p && virt_pos < kv_size(vt));
      if (!s.p) {
        break;
      }
    }
    if (!*s.p) {
      continue;
    }
    int attr;
    bool through = false;
    if (hl_mode == kHlModeCombine) {
      attr = hl_combine_attr(linebuf_attr[col], virt_attr);
    } else if (hl_mode == kHlModeBlend) {
      through = (*s.p == ' ');
      attr = hl_blend_attrs(linebuf_attr[col], virt_attr, &through);
    } else {
      attr = virt_attr;
    }
    schar_T dummy[2];
    int cells = line_putchar(buf, &s, through ? dummy : &linebuf_char[col],
                             max_col - col, false, vcol);
    // If we failed to emit a char, we still need to put a space and advance.
    if (cells < 1) {
      schar_from_ascii(linebuf_char[col], ' ');
      cells = 1;
    }
    for (int c = 0; c < cells; c++) {
      linebuf_attr[col++] = attr;
    }
    if (col < max_col && linebuf_char[col][0] == 0) {
      // If the left half of a double-width char is overwritten,
      // change the right half to a space so that grid redraws properly,
      // but don't advance the current column.
      schar_from_ascii(linebuf_char[col], ' ');
    }
    vcol += cells;
  }
  return col;
}

/// Return true if CursorLineSign highlight is to be used.
static bool use_cursor_line_highlight(win_T *wp, linenr_T lnum)
{
  return wp->w_p_cul
         && lnum == wp->w_cursorline
         && (wp->w_p_culopt_flags & CULOPT_NBR);
}

/// Setup for drawing the 'foldcolumn', if there is one.
static void handle_foldcolumn(win_T *wp, winlinevars_T *wlv)
{
  int fdc = compute_foldcolumn(wp, 0);
  if (fdc <= 0) {
    return;
  }

  // Allocate a buffer, "wlv->extra[]" may already be in use.
  xfree(wlv->p_extra_free);
  wlv->p_extra_free = xmalloc(MAX_MCO * (size_t)fdc + 1);
  wlv->n_extra = (int)fill_foldcolumn(wlv->p_extra_free, wp, wlv->foldinfo, wlv->lnum);
  wlv->p_extra_free[wlv->n_extra] = NUL;
  wlv->p_extra = wlv->p_extra_free;
  wlv->c_extra = NUL;
  wlv->c_final = NUL;
  if (use_cursor_line_highlight(wp, wlv->lnum)) {
    wlv->char_attr = win_hl_attr(wp, HLF_CLF);
  } else {
    wlv->char_attr = win_hl_attr(wp, HLF_FC);
  }
}

/// Fills the foldcolumn at "p" for window "wp".
/// Only to be called when 'foldcolumn' > 0.
///
/// @param[out] p  Char array to write into
/// @param lnum    Absolute current line number
/// @param closed  Whether it is in 'foldcolumn' mode
///
/// Assume monocell characters
/// @return number of chars added to \param p
size_t fill_foldcolumn(char *p, win_T *wp, foldinfo_T foldinfo, linenr_T lnum)
{
  int i = 0;
  int level;
  int first_level;
  int fdc = compute_foldcolumn(wp, 0);    // available cell width
  size_t char_counter = 0;
  int symbol = 0;
  int len = 0;
  bool closed = foldinfo.fi_lines > 0;
  // Init to all spaces.
  memset(p, ' ', MAX_MCO * (size_t)fdc + 1);

  level = foldinfo.fi_level;

  // If the column is too narrow, we start at the lowest level that
  // fits and use numbers to indicate the depth.
  first_level = level - fdc - closed + 1;
  if (first_level < 1) {
    first_level = 1;
  }

  for (i = 0; i < MIN(fdc, level); i++) {
    if (foldinfo.fi_lnum == lnum
        && first_level + i >= foldinfo.fi_low_level) {
      symbol = wp->w_p_fcs_chars.foldopen;
    } else if (first_level == 1) {
      symbol = wp->w_p_fcs_chars.foldsep;
    } else if (first_level + i <= 9) {
      symbol = '0' + first_level + i;
    } else {
      symbol = '>';
    }

    len = utf_char2bytes(symbol, &p[char_counter]);
    char_counter += (size_t)len;
    if (first_level + i >= level) {
      i++;
      break;
    }
  }

  if (closed) {
    if (symbol != 0) {
      // rollback previous write
      char_counter -= (size_t)len;
      memset(&p[char_counter], ' ', (size_t)len);
    }
    len = utf_char2bytes(wp->w_p_fcs_chars.foldclosed, &p[char_counter]);
    char_counter += (size_t)len;
  }

  return MAX(char_counter + (size_t)(fdc - i), (size_t)fdc);
}

/// Get information needed to display the sign in line "wlv->lnum" in window "wp".
/// If "nrcol" is true, the sign is going to be displayed in the number column.
/// Otherwise the sign is going to be displayed in the sign column.
static void get_sign_display_info(bool nrcol, win_T *wp, winlinevars_T *wlv, int sign_idx,
                                  int sign_cul_attr)
{
  // Draw cells with the sign value or blank.
  wlv->c_extra = ' ';
  wlv->c_final = NUL;
  if (nrcol) {
    wlv->n_extra = number_width(wp) + 1;
  } else {
    if (use_cursor_line_highlight(wp, wlv->lnum)) {
      wlv->char_attr = win_hl_attr(wp, HLF_CLS);
    } else {
      wlv->char_attr = win_hl_attr(wp, HLF_SC);
    }
    wlv->n_extra = win_signcol_width(wp);
  }

  if (wlv->row == wlv->startrow + wlv->filler_lines && wlv->filler_todo <= 0) {
    SignTextAttrs *sattr = sign_get_attr(sign_idx, wlv->sattrs, wp->w_scwidth);
    if (sattr != NULL) {
      wlv->p_extra = sattr->text;
      if (wlv->p_extra != NULL) {
        wlv->c_extra = NUL;
        wlv->c_final = NUL;

        if (nrcol) {
          int width = number_width(wp) - 2;
          size_t n;
          for (n = 0; (int)n < width; n++) {
            wlv->extra[n] = ' ';
          }
          wlv->extra[n] = NUL;
          snprintf(wlv->extra + n, sizeof(wlv->extra) - n, "%s ", wlv->p_extra);
          wlv->p_extra = wlv->extra;
          wlv->n_extra = (int)strlen(wlv->p_extra);
        } else {
          size_t symbol_blen = strlen(wlv->p_extra);

          // TODO(oni-link): Is sign text already extended to
          // full cell width?
          assert((size_t)win_signcol_width(wp) >= mb_string2cells(wlv->p_extra));
          // symbol(s) bytes + (filling spaces) (one byte each)
          wlv->n_extra = (int)symbol_blen + win_signcol_width(wp) -
                         (int)mb_string2cells(wlv->p_extra);

          assert(sizeof(wlv->extra) > symbol_blen);
          memset(wlv->extra, ' ', sizeof(wlv->extra));
          memcpy(wlv->extra, wlv->p_extra, symbol_blen);

          wlv->p_extra = wlv->extra;
          wlv->p_extra[wlv->n_extra] = NUL;
        }
      }

      if (use_cursor_line_highlight(wp, wlv->lnum) && sign_cul_attr > 0) {
        wlv->char_attr = sign_cul_attr;
      } else {
        wlv->char_attr = sattr->hl_id ? syn_id2attr(sattr->hl_id) : 0;
      }
    }
  }
}

/// Returns width of the signcolumn that should be used for the whole window
///
/// @param wp window we want signcolumn width from
/// @return max width of signcolumn (cell unit)
///
/// @note Returns a constant for now but hopefully we can improve neovim so that
///       the returned value width adapts to the maximum number of marks to draw
///       for the window
/// TODO(teto)
int win_signcol_width(win_T *wp)
{
  // 2 is vim default value
  return 2;
}

static inline void get_line_number_str(win_T *wp, linenr_T lnum, char *buf, size_t buf_len)
{
  long num;
  char *fmt = "%*ld ";

  if (wp->w_p_nu && !wp->w_p_rnu) {
    // 'number' + 'norelativenumber'
    num = (long)lnum;
  } else {
    // 'relativenumber', don't use negative numbers
    num = labs((long)get_cursor_rel_lnum(wp, lnum));
    if (num == 0 && wp->w_p_nu && wp->w_p_rnu) {
      // 'number' + 'relativenumber'
      num = lnum;
      fmt = "%-*ld ";
    }
  }

  snprintf(buf, buf_len, fmt, number_width(wp), num);
}

/// Return true if CursorLineNr highlight is to be used for the number column.
/// - 'cursorline' must be set
/// - "wlv->lnum" must be the cursor line
/// - 'cursorlineopt' has "number"
/// - don't highlight filler lines (when in diff mode)
/// - When line is wrapped and 'cursorlineopt' does not have "line", only highlight the line number
///   itself on the first screenline of the wrapped line, otherwise highlight the number column of
///   all screenlines of the wrapped line.
static bool use_cursor_line_nr(win_T *wp, winlinevars_T *wlv)
{
  return wp->w_p_cul
         && wlv->lnum == wp->w_cursorline
         && (wp->w_p_culopt_flags & CULOPT_NBR)
         && (wlv->row == wlv->startrow + wlv->filler_lines
             || (wlv->row > wlv->startrow + wlv->filler_lines
                 && (wp->w_p_culopt_flags & CULOPT_LINE)));
}

static int get_line_number_attr(win_T *wp, winlinevars_T *wlv)
{
  if (use_cursor_line_nr(wp, wlv)) {
    // TODO(vim): Can we use CursorLine instead of CursorLineNr
    // when CursorLineNr isn't set?
    return win_hl_attr(wp, HLF_CLN);
  }

  if (wp->w_p_rnu) {
    if (wlv->lnum < wp->w_cursor.lnum) {
      // Use LineNrAbove
      return win_hl_attr(wp, HLF_LNA);
    }
    if (wlv->lnum > wp->w_cursor.lnum) {
      // Use LineNrBelow
      return win_hl_attr(wp, HLF_LNB);
    }
  }

  return win_hl_attr(wp, HLF_N);
}

/// Display the absolute or relative line number.  After the first row fill with
/// blanks when the 'n' flag isn't in 'cpo'.
static void handle_lnum_col(win_T *wp, winlinevars_T *wlv, int num_signs, int sign_idx,
                            int sign_num_attr, int sign_cul_attr)
{
  if ((wp->w_p_nu || wp->w_p_rnu)
      && (wlv->row == wlv->startrow + wlv->filler_lines
          || vim_strchr(p_cpo, CPO_NUMCOL) == NULL)) {
    // If 'signcolumn' is set to 'number' and a sign is present
    // in "lnum", then display the sign instead of the line
    // number.
    if (*wp->w_p_scl == 'n' && *(wp->w_p_scl + 1) == 'u' && num_signs > 0) {
      get_sign_display_info(true, wp, wlv, sign_idx, sign_cul_attr);
    } else {
      // Draw the line number (empty space after wrapping).
      if (wlv->row == wlv->startrow + wlv->filler_lines) {
        get_line_number_str(wp, wlv->lnum, wlv->extra, sizeof(wlv->extra));
        if (wp->w_skipcol > 0) {
          for (wlv->p_extra = wlv->extra; *wlv->p_extra == ' '; wlv->p_extra++) {
            *wlv->p_extra = '-';
          }
        }
        if (wp->w_p_rl) {                       // reverse line numbers
          // like rl_mirror_ascii(), but keep the space at the end
          char *p2 = skipwhite(wlv->extra);
          p2 = skiptowhite(p2) - 1;
          for (char *p1 = skipwhite(wlv->extra); p1 < p2; p1++, p2--) {
            const char t = *p1;
            *p1 = *p2;
            *p2 = t;
          }
        }
        wlv->p_extra = wlv->extra;
        wlv->c_extra = NUL;
      } else {
        wlv->c_extra = ' ';
      }
      wlv->c_final = NUL;
      wlv->n_extra = number_width(wp) + 1;
      if (sign_num_attr > 0) {
        wlv->char_attr = sign_num_attr;
      } else {
        wlv->char_attr = get_line_number_attr(wp, wlv);
      }
    }
  }
}

/// Prepare and build the 'statuscolumn' string for line "lnum" in window "wp".
/// Fill "stcp" with the built status column string and attributes.
/// This can be called three times per win_line(), once for virt_lines, once for
/// the start of the buffer line "lnum" and once for the wrapped lines.
///
/// @param[out] stcp  Status column attributes
static void get_statuscol_str(win_T *wp, linenr_T lnum, int virtnum, statuscol_T *stcp)
{
  // When called for the first non-filler row of line "lnum" set num v:vars
  long relnum = virtnum == 0 ? labs(get_cursor_rel_lnum(wp, lnum)) : -1;

  // When a buffer's line count has changed, make a best estimate for the full
  // width of the status column by building with "w_nrwidth_line_count". Add
  // potentially truncated width and rebuild before drawing anything.
  if (wp->w_statuscol_line_count != wp->w_nrwidth_line_count) {
    wp->w_statuscol_line_count = wp->w_nrwidth_line_count;
    set_vim_var_nr(VV_VIRTNUM, 0);
    build_statuscol_str(wp, wp->w_nrwidth_line_count, 0, stcp);
    if (stcp->truncate > 0) {
      // Add truncated width to avoid unnecessary redraws
      int addwidth = MIN(stcp->truncate, MAX_NUMBERWIDTH - wp->w_nrwidth);
      stcp->truncate = 0;
      stcp->width += addwidth;
      wp->w_nrwidth += addwidth;
      wp->w_nrwidth_width = wp->w_nrwidth;
      wp->w_valid &= ~VALID_WCOL;
    }
  }
  set_vim_var_nr(VV_VIRTNUM, virtnum);

  int width = build_statuscol_str(wp, lnum, relnum, stcp);
  // Force a redraw in case of error or when truncated
  if (*wp->w_p_stc == NUL || (stcp->truncate > 0 && wp->w_nrwidth < MAX_NUMBERWIDTH)) {
    if (stcp->truncate) {  // Avoid truncating 'statuscolumn'
      wp->w_nrwidth = MIN(MAX_NUMBERWIDTH, wp->w_nrwidth + stcp->truncate);
      wp->w_nrwidth_width = wp->w_nrwidth;
    } else {  // 'statuscolumn' reset due to error
      wp->w_nrwidth_line_count = 0;
      wp->w_nrwidth = (wp->w_p_nu || wp->w_p_rnu) * number_width(wp);
    }
    wp->w_redr_statuscol = true;
    return;
  }

  // Reset text/highlight pointer and current attr for new line
  stcp->textp = stcp->text;
  stcp->hlrecp = stcp->hlrec;
  stcp->cur_attr = stcp->num_attr;
  stcp->text_end = stcp->text + strlen(stcp->text);

  int fill = stcp->width - width;
  if (fill > 0) {
    // Fill up with ' '
    memset(stcp->text_end, ' ', (size_t)fill);
    *(stcp->text_end += fill) = NUL;
  }
}

/// Get information needed to display the next segment in the 'statuscolumn'.
/// If not yet at the end, prepare for next segment and decrement "wlv->draw_state".
///
/// @param stcp  Status column attributes
/// @param[in,out]  wlv
static void get_statuscol_display_info(statuscol_T *stcp, winlinevars_T *wlv)
{
  wlv->c_extra = NUL;
  wlv->c_final = NUL;
  do {
    wlv->draw_state = WL_STC;
    wlv->char_attr = stcp->cur_attr;
    wlv->p_extra = stcp->textp;
    wlv->n_extra =
      (int)((stcp->hlrecp->start ? stcp->hlrecp->start : stcp->text_end) - stcp->textp);
    // Prepare for next highlight section if not yet at the end
    if (stcp->textp + wlv->n_extra < stcp->text_end) {
      int hl = stcp->hlrecp->userhl;
      stcp->textp = stcp->hlrecp->start;
      stcp->cur_attr = hl < 0 ? syn_id2attr(-hl) : stcp->num_attr;
      stcp->hlrecp++;
      wlv->draw_state = WL_STC - 1;
    }
    // Skip over empty highlight sections
  } while (wlv->n_extra == 0 && stcp->textp < stcp->text_end);
  if (wlv->n_extra > 0) {
    static char transbuf[(MAX_NUMBERWIDTH + 9 + 9 * 2) * MB_MAXBYTES + 1];
    wlv->n_extra = (int)transstr_buf(wlv->p_extra, wlv->n_extra, transbuf, sizeof transbuf, true);
    wlv->p_extra = transbuf;
  }
}

static void handle_breakindent(win_T *wp, winlinevars_T *wlv)
{
  if (wp->w_briopt_sbr && wlv->draw_state == WL_BRI - 1
      && *get_showbreak_value(wp) != NUL) {
    // draw indent after showbreak value
    wlv->draw_state = WL_BRI;
  } else if (wp->w_briopt_sbr && wlv->draw_state == WL_SBR) {
    // after the showbreak, draw the breakindent
    wlv->draw_state = WL_BRI - 1;
  }

  // draw 'breakindent': indent wrapped text accordingly
  if (wlv->draw_state == WL_BRI - 1 && wlv->n_extra == 0) {
    wlv->draw_state = WL_BRI;
    // if wlv->need_showbreak is set, breakindent also applies
    if (wp->w_p_bri && (wlv->row != wlv->startrow || wlv->need_showbreak)
        && wlv->filler_lines == 0) {
      wlv->char_attr = 0;
      if (wlv->diff_hlf != (hlf_T)0) {
        wlv->char_attr = win_hl_attr(wp, (int)wlv->diff_hlf);
      }
      wlv->p_extra = NULL;
      wlv->c_extra = ' ';
      wlv->c_final = NUL;
      wlv->n_extra = get_breakindent_win(wp, ml_get_buf(wp->w_buffer, wlv->lnum, false));
      if (wlv->row == wlv->startrow) {
        wlv->n_extra -= win_col_off2(wp);
        if (wlv->n_extra < 0) {
          wlv->n_extra = 0;
        }
      }
      if (wp->w_skipcol > 0 && wp->w_p_wrap && wp->w_briopt_sbr) {
        wlv->need_showbreak = false;
      }
      // Correct end of highlighted area for 'breakindent',
      // required wen 'linebreak' is also set.
      if (wlv->tocol == wlv->vcol) {
        wlv->tocol += wlv->n_extra;
      }
    }
  }
}

static void handle_showbreak_and_filler(win_T *wp, winlinevars_T *wlv)
{
  if (wlv->filler_todo > wlv->filler_lines - wlv->n_virt_lines) {
    // TODO(bfredl): check this doesn't inhibit TUI-style
    //               clear-to-end-of-line.
    wlv->c_extra = ' ';
    wlv->c_final = NUL;
    if (wp->w_p_rl) {
      wlv->n_extra = wlv->col + 1;
    } else {
      wlv->n_extra = wp->w_grid.cols - wlv->col;
    }
    wlv->char_attr = 0;
  } else if (wlv->filler_todo > 0) {
    // Draw "deleted" diff line(s)
    if (char2cells(wp->w_p_fcs_chars.diff) > 1) {
      wlv->c_extra = '-';
      wlv->c_final = NUL;
    } else {
      wlv->c_extra = wp->w_p_fcs_chars.diff;
      wlv->c_final = NUL;
    }
    if (wp->w_p_rl) {
      wlv->n_extra = wlv->col + 1;
    } else {
      wlv->n_extra = wp->w_grid.cols - wlv->col;
    }
    wlv->char_attr = win_hl_attr(wp, HLF_DED);
  }

  char *const sbr = get_showbreak_value(wp);
  if (*sbr != NUL && wlv->need_showbreak) {
    // Draw 'showbreak' at the start of each broken line.
    wlv->p_extra = sbr;
    wlv->c_extra = NUL;
    wlv->c_final = NUL;
    wlv->n_extra = (int)strlen(sbr);
    wlv->char_attr = win_hl_attr(wp, HLF_AT);
    if (wp->w_skipcol == 0 || !wp->w_p_wrap) {
      wlv->need_showbreak = false;
    }
    wlv->vcol_sbr = wlv->vcol + mb_charlen(sbr);
    // Correct end of highlighted area for 'showbreak',
    // required when 'linebreak' is also set.
    if (wlv->tocol == wlv->vcol) {
      wlv->tocol += wlv->n_extra;
    }
    // Combine 'showbreak' with 'cursorline', prioritizing 'showbreak'.
    if (wlv->cul_attr) {
      wlv->char_attr = hl_combine_attr(wlv->cul_attr, wlv->char_attr);
    }
  }
}

static void apply_cursorline_highlight(win_T *wp, winlinevars_T *wlv)
{
  wlv->cul_attr = win_hl_attr(wp, HLF_CUL);
  HlAttrs ae = syn_attr2entry(wlv->cul_attr);
  // We make a compromise here (#7383):
  //  * low-priority CursorLine if fg is not set
  //  * high-priority ("same as Vim" priority) CursorLine if fg is set
  if (ae.rgb_fg_color == -1 && ae.cterm_fg_color == 0) {
    wlv->line_attr_lowprio = wlv->cul_attr;
  } else {
    if (!(State & MODE_INSERT) && bt_quickfix(wp->w_buffer)
        && qf_current_entry(wp) == wlv->lnum) {
      wlv->line_attr = hl_combine_attr(wlv->cul_attr, wlv->line_attr);
    } else {
      wlv->line_attr = wlv->cul_attr;
    }
  }
}

static bool check_mb_utf8(int *c, int *u8cc)
{
  if (utf_char2len(*c) > 1) {
    *u8cc = 0;
    *c = 0xc0;
    return true;
  }
  return false;
}

static colnr_T get_trailcol(win_T *wp, const char *ptr, const char *line)
{
  colnr_T trailcol = MAXCOL;
  // find start of trailing whitespace
  if (wp->w_p_lcs_chars.trail) {
    trailcol = (colnr_T)strlen(ptr);
    while (trailcol > 0 && ascii_iswhite(ptr[trailcol - 1])) {
      trailcol--;
    }
    trailcol += (colnr_T)(ptr - line);
  }

  return trailcol;
}

static colnr_T get_leadcol(win_T *wp, const char *ptr, const char *line)
{
  colnr_T leadcol = 0;

  // find end of leading whitespace
  if (wp->w_p_lcs_chars.lead || wp->w_p_lcs_chars.leadmultispace != NULL) {
    leadcol = 0;
    while (ascii_iswhite(ptr[leadcol])) {
      leadcol++;
    }
    if (ptr[leadcol] == NUL) {
      // in a line full of spaces all of them are treated as trailing
      leadcol = 0;
    } else {
      // keep track of the first column not filled with spaces
      leadcol += (colnr_T)(ptr - line + 1);
    }
  }

  return leadcol;
}

/// Start a screen line at column zero.
static void win_line_start(win_T *wp, winlinevars_T *wlv, bool save_extra)
{
  wlv->col = 0;
  wlv->off = 0;

  if (wp->w_p_rl) {
    // Rightleft window: process the text in the normal direction, but put
    // it in linebuf_char[wlv.off] from right to left.  Start at the
    // rightmost column of the window.
    wlv->col = wp->w_grid.cols - 1;
    wlv->off += wlv->col;
  }

  if (save_extra) {
    // reset the drawing state for the start of a wrapped line
    wlv->draw_state = WL_START;
    wlv->saved_n_extra = wlv->n_extra;
    wlv->saved_p_extra = wlv->p_extra;
    wlv->saved_c_extra = wlv->c_extra;
    wlv->saved_c_final = wlv->c_final;
    wlv->saved_char_attr = wlv->char_attr;

    wlv->n_extra = 0;
  }
}

/// Called when wlv->draw_state is set to WL_LINE.
static void win_line_continue(winlinevars_T *wlv)
{
  if (wlv->saved_n_extra > 0) {
    // Continue item from end of wrapped line.
    wlv->n_extra = wlv->saved_n_extra;
    wlv->c_extra = wlv->saved_c_extra;
    wlv->c_final = wlv->saved_c_final;
    wlv->p_extra = wlv->saved_p_extra;
    wlv->char_attr = wlv->saved_char_attr;
  } else {
    wlv->char_attr = 0;
  }
}

/// Display line "lnum" of window 'wp' on the screen.
/// wp->w_virtcol needs to be valid.
///
/// @param lnum         line to display
/// @param startrow     first row relative to window grid
/// @param endrow       last grid row to be redrawn
/// @param nochange     not updating for changed text
/// @param number_only  only update the number column
/// @param foldinfo     fold info for this line
/// @param[in, out] providers  decoration providers active this line
///                            items will be disables if they cause errors
///                            or explicitly return `false`.
///
/// @return             the number of last row the line occupies.
int win_line(win_T *wp, linenr_T lnum, int startrow, int endrow, bool nochange, bool number_only,
             foldinfo_T foldinfo, DecorProviders *providers, char **provider_err)
{
  winlinevars_T wlv;                  // variables passed between functions

  int c = 0;                          // init for GCC
  long vcol_prev = -1;                // "wlv.vcol" of previous character
  char *line;                         // current line
  char *ptr;                          // current position in "line"
  ScreenGrid *grid = &wp->w_grid;     // grid specific to the window

  static char *at_end_str = "";       // used for p_extra when displaying curwin->w_p_lcs_chars.eol
                                      // at end-of-line
  bool has_fold = foldinfo.fi_level != 0 && foldinfo.fi_lines > 0;

  int n_attr = 0;                       // chars with special attr
  int saved_attr2 = 0;                  // char_attr saved for n_attr
  int n_attr3 = 0;                      // chars with overruling special attr
  int saved_attr3 = 0;                  // char_attr saved for n_attr3

  int n_skip = 0;                       // nr of chars to skip for 'nowrap'

  int fromcol_prev = -2;                // start of inverting after cursor
  bool noinvcur = false;                // don't invert the cursor
  bool lnum_in_visual_area = false;
  pos_T pos;
  ptrdiff_t v;

  bool attr_pri = false;                // char_attr has priority
  bool area_highlighting = false;       // Visual or incsearch highlighting in this line
  int vi_attr = 0;                      // attributes for Visual and incsearch highlighting
  int area_attr = 0;                    // attributes desired by highlighting
  int search_attr = 0;                  // attributes desired by 'hlsearch'
  int vcol_save_attr = 0;               // saved attr for 'cursorcolumn'
  int syntax_attr = 0;                  // attributes desired by syntax
  bool has_syntax = false;              // this buffer has syntax highl.
  int save_did_emsg;
  int eol_hl_off = 0;                   // 1 if highlighted char after EOL
  bool draw_color_col = false;          // highlight colorcolumn
  int *color_cols = NULL;               // pointer to according columns array
  bool has_spell = false;               // this buffer has spell checking
#define SPWORDLEN 150
  char nextline[SPWORDLEN * 2];         // text with start of the next line
  int nextlinecol = 0;                  // column where nextline[] starts
  int nextline_idx = 0;                 // index in nextline[] where next line
                                        // starts
  int spell_attr = 0;                   // attributes desired by spelling
  int word_end = 0;                     // last byte with same spell_attr
  static linenr_T checked_lnum = 0;     // line number for "checked_col"
  static int checked_col = 0;           // column in "checked_lnum" up to which
                                        // there are no spell errors
  static int cap_col = -1;              // column to check for Cap word
  static linenr_T capcol_lnum = 0;      // line number where "cap_col"
  int cur_checked_col = 0;              // checked column for current line
  int extra_check = 0;                  // has syntax or linebreak
  int multi_attr = 0;                   // attributes desired by multibyte
  int mb_l = 1;                         // multi-byte byte length
  int mb_c = 0;                         // decoded multi-byte character
  bool mb_utf8 = false;                 // screen char is UTF-8 char
  int u8cc[MAX_MCO];                    // composing UTF-8 chars
  int change_start = MAXCOL;            // first col of changed area
  int change_end = -1;                  // last col of changed area
  bool in_multispace = false;           // in multiple consecutive spaces
  int multispace_pos = 0;               // position in lcs-multispace string
  int line_attr_save;
  int line_attr_lowprio_save;
  int prev_c = 0;                       // previous Arabic character
  int prev_c1 = 0;                      // first composing char for prev_c

  bool search_attr_from_match = false;  // if search_attr is from :match
  bool has_decor = false;               // this buffer has decoration
  int win_col_offset = 0;               // offset for window columns

  char buf_fold[FOLD_TEXT_LEN];         // Hold value returned by get_foldtext

  bool area_active = false;

  // 'cursorlineopt' has "screenline" and cursor is in this line
  bool cul_screenline = false;
  // margin columns for the screen line, needed for when 'cursorlineopt'
  // contains "screenline"
  int left_curline_col = 0;
  int right_curline_col = 0;

  int match_conc      = 0;              ///< cchar for match functions
  bool on_last_col    = false;
  int syntax_flags    = 0;
  int syntax_seqnr    = 0;
  int prev_syntax_id  = 0;
  int conceal_attr    = win_hl_attr(wp, HLF_CONCEAL);
  bool is_concealing  = false;
  int did_wcol        = false;
  int old_boguscols = 0;
#define VCOL_HLC (wlv.vcol - wlv.vcol_off)
#define FIX_FOR_BOGUSCOLS \
  { \
    wlv.n_extra += wlv.vcol_off; \
    wlv.vcol -= wlv.vcol_off; \
    wlv.vcol_off = 0; \
    wlv.col -= wlv.boguscols; \
    old_boguscols = wlv.boguscols; \
    wlv.boguscols = 0; \
  }

  if (startrow > endrow) {              // past the end already!
    return startrow;
  }

  CLEAR_FIELD(wlv);

  wlv.lnum = lnum;
  wlv.foldinfo = foldinfo;
  wlv.startrow = startrow;
  wlv.row = startrow;
  wlv.fromcol = -10;
  wlv.tocol = MAXCOL;
  wlv.vcol_sbr = -1;

  buf_T *buf = wp->w_buffer;
  bool end_fill = (lnum == buf->b_ml.ml_line_count + 1);

  if (!number_only) {
    // To speed up the loop below, set extra_check when there is linebreak,
    // trailing white space and/or syntax processing to be done.
    extra_check = wp->w_p_lbr;
    if (syntax_present(wp) && !wp->w_s->b_syn_error && !wp->w_s->b_syn_slow
        && !has_fold && !end_fill) {
      // Prepare for syntax highlighting in this line.  When there is an
      // error, stop syntax highlighting.
      save_did_emsg = did_emsg;
      did_emsg = false;
      syntax_start(wp, lnum);
      if (did_emsg) {
        wp->w_s->b_syn_error = true;
      } else {
        did_emsg = save_did_emsg;
        if (!wp->w_s->b_syn_slow) {
          has_syntax = true;
          extra_check = true;
        }
      }
    }

    has_decor = decor_redraw_line(wp, lnum - 1, &decor_state);

    decor_providers_invoke_line(wp, providers, lnum - 1, &has_decor, provider_err);

    if (*provider_err) {
      provider_err_virt_text(lnum, *provider_err);
      has_decor = true;
      *provider_err = NULL;
    }

    if (has_decor) {
      extra_check = true;
    }

    // Check for columns to display for 'colorcolumn'.
    color_cols = wp->w_buffer->terminal ? NULL : wp->w_p_cc_cols;
    if (color_cols != NULL) {
      draw_color_col = advance_color_col(VCOL_HLC, &color_cols);
    }

    if (wp->w_p_spell
        && !has_fold
        && !end_fill
        && *wp->w_s->b_p_spl != NUL
        && !GA_EMPTY(&wp->w_s->b_langp)
        && *(char **)(wp->w_s->b_langp.ga_data) != NULL) {
      // Prepare for spell checking.
      has_spell = true;
      extra_check = true;

      // Get the start of the next line, so that words that wrap to the next
      // line are found too: "et<line-break>al.".
      // Trick: skip a few chars for C/shell/Vim comments
      nextline[SPWORDLEN] = NUL;
      if (lnum < wp->w_buffer->b_ml.ml_line_count) {
        line = ml_get_buf(wp->w_buffer, lnum + 1, false);
        spell_cat_line(nextline + SPWORDLEN, line, SPWORDLEN);
      }

      // When a word wrapped from the previous line the start of the current
      // line is valid.
      if (lnum == checked_lnum) {
        cur_checked_col = checked_col;
      }
      checked_lnum = 0;

      // When there was a sentence end in the previous line may require a
      // word starting with capital in this line.  In line 1 always check
      // the first word.
      if (lnum != capcol_lnum) {
        cap_col = -1;
      }
      if (lnum == 1) {
        cap_col = 0;
      }
      capcol_lnum = 0;
    }

    // handle Visual active in this window
    if (VIsual_active && wp->w_buffer == curwin->w_buffer) {
      pos_T *top, *bot;

      if (ltoreq(curwin->w_cursor, VIsual)) {
        // Visual is after curwin->w_cursor
        top = &curwin->w_cursor;
        bot = &VIsual;
      } else {
        // Visual is before curwin->w_cursor
        top = &VIsual;
        bot = &curwin->w_cursor;
      }
      lnum_in_visual_area = (lnum >= top->lnum && lnum <= bot->lnum);
      if (VIsual_mode == Ctrl_V) {
        // block mode
        if (lnum_in_visual_area) {
          wlv.fromcol = wp->w_old_cursor_fcol;
          wlv.tocol = wp->w_old_cursor_lcol;
        }
      } else {
        // non-block mode
        if (lnum > top->lnum && lnum <= bot->lnum) {
          wlv.fromcol = 0;
        } else if (lnum == top->lnum) {
          if (VIsual_mode == 'V') {       // linewise
            wlv.fromcol = 0;
          } else {
            getvvcol(wp, top, (colnr_T *)&wlv.fromcol, NULL, NULL);
            if (gchar_pos(top) == NUL) {
              wlv.tocol = wlv.fromcol + 1;
            }
          }
        }
        if (VIsual_mode != 'V' && lnum == bot->lnum) {
          if (*p_sel == 'e' && bot->col == 0
              && bot->coladd == 0) {
            wlv.fromcol = -10;
            wlv.tocol = MAXCOL;
          } else if (bot->col == MAXCOL) {
            wlv.tocol = MAXCOL;
          } else {
            pos = *bot;
            if (*p_sel == 'e') {
              getvvcol(wp, &pos, (colnr_T *)&wlv.tocol, NULL, NULL);
            } else {
              getvvcol(wp, &pos, NULL, NULL, (colnr_T *)&wlv.tocol);
              wlv.tocol++;
            }
          }
        }
      }

      // Check if the char under the cursor should be inverted (highlighted).
      if (!highlight_match && lnum == curwin->w_cursor.lnum && wp == curwin
          && cursor_is_block_during_visual(*p_sel == 'e')) {
        noinvcur = true;
      }

      // if inverting in this line set area_highlighting
      if (wlv.fromcol >= 0) {
        area_highlighting = true;
        vi_attr = win_hl_attr(wp, HLF_V);
      }
      // handle 'incsearch' and ":s///c" highlighting
    } else if (highlight_match
               && wp == curwin
               && !has_fold
               && lnum >= curwin->w_cursor.lnum
               && lnum <= curwin->w_cursor.lnum + search_match_lines) {
      if (lnum == curwin->w_cursor.lnum) {
        getvcol(curwin, &(curwin->w_cursor),
                (colnr_T *)&wlv.fromcol, NULL, NULL);
      } else {
        wlv.fromcol = 0;
      }
      if (lnum == curwin->w_cursor.lnum + search_match_lines) {
        pos.lnum = lnum;
        pos.col = search_match_endcol;
        getvcol(curwin, &pos, (colnr_T *)&wlv.tocol, NULL, NULL);
      }
      // do at least one character; happens when past end of line
      if (wlv.fromcol == wlv.tocol && search_match_endcol) {
        wlv.tocol = wlv.fromcol + 1;
      }
      area_highlighting = true;
      vi_attr = win_hl_attr(wp, HLF_I);
    }
  }

  int bg_attr = win_bg_attr(wp);

  int linestatus = 0;
  wlv.filler_lines = diff_check_with_linestatus(wp, lnum, &linestatus);
  if (wlv.filler_lines < 0 || linestatus < 0) {
    if (wlv.filler_lines == -1 || linestatus == -1) {
      if (diff_find_change(wp, lnum, &change_start, &change_end)) {
        wlv.diff_hlf = HLF_ADD;             // added line
      } else if (change_start == 0) {
        wlv.diff_hlf = HLF_TXD;             // changed text
      } else {
        wlv.diff_hlf = HLF_CHD;             // changed line
      }
    } else {
      wlv.diff_hlf = HLF_ADD;               // added line
    }
    if (linestatus == 0) {
      wlv.filler_lines = 0;
    }
    area_highlighting = true;
  }
  VirtLines virt_lines = KV_INITIAL_VALUE;
  wlv.n_virt_lines = decor_virt_lines(wp, lnum, &virt_lines, has_fold);
  wlv.filler_lines += wlv.n_virt_lines;
  if (lnum == wp->w_topline) {
    wlv.filler_lines = wp->w_topfill;
    wlv.n_virt_lines = MIN(wlv.n_virt_lines, wlv.filler_lines);
  }
  wlv.filler_todo = wlv.filler_lines;

  // Cursor line highlighting for 'cursorline' in the current window.
  if (wp->w_p_cul && wp->w_p_culopt_flags != CULOPT_NBR && lnum == wp->w_cursorline
      // Do not show the cursor line in the text when Visual mode is active,
      // because it's not clear what is selected then.
      && !(wp == curwin && VIsual_active)) {
    cul_screenline = (wp->w_p_wrap && (wp->w_p_culopt_flags & CULOPT_SCRLINE));
    if (!cul_screenline) {
      apply_cursorline_highlight(wp, &wlv);
    } else {
      margin_columns_win(wp, &left_curline_col, &right_curline_col);
    }
    area_highlighting = true;
  }

  HlPriId line_id = { 0 };
  HlPriId sign_cul = { 0 };
  HlPriId sign_num = { 0 };
  // TODO(bfredl, vigoux): line_attr should not take priority over decoration!
  int num_signs = buf_get_signattrs(buf, wlv.lnum, wlv.sattrs, &sign_num, &line_id, &sign_cul);
  decor_redraw_signs(buf, wlv.lnum - 1, &num_signs, wlv.sattrs, &sign_num, &line_id, &sign_cul);

  int sign_cul_attr = 0;
  int sign_num_attr = 0;
  statuscol_T statuscol = { 0 };
  if (*wp->w_p_stc != NUL) {
    // Draw the 'statuscolumn' if option is set.
    statuscol.draw = true;
    statuscol.sattrs = wlv.sattrs;
    statuscol.foldinfo = foldinfo;
    statuscol.width = win_col_off(wp) - (cmdwin_type != 0 && wp == curwin);
    statuscol.use_cul = use_cursor_line_highlight(wp, lnum);
    statuscol.sign_cul_id = statuscol.use_cul ? sign_cul.hl_id : 0;
    statuscol.num_attr = sign_num.hl_id ? syn_id2attr(sign_num.hl_id)
                                        : get_line_number_attr(wp, &wlv);
  } else {
    if (sign_cul.hl_id > 0) {
      sign_cul_attr = syn_id2attr(sign_cul.hl_id);
    }
    if (sign_num.hl_id > 0) {
      sign_num_attr = syn_id2attr(sign_num.hl_id);
    }
  }
  if (line_id.hl_id > 0) {
    wlv.line_attr = syn_id2attr(line_id.hl_id);
  }

  // Highlight the current line in the quickfix window.
  if (bt_quickfix(wp->w_buffer) && qf_current_entry(wp) == lnum) {
    wlv.line_attr = win_hl_attr(wp, HLF_QFL);
  }

  if (wlv.line_attr_lowprio || wlv.line_attr) {
    area_highlighting = true;
  }

  if (cul_screenline) {
    line_attr_save = wlv.line_attr;
    line_attr_lowprio_save = wlv.line_attr_lowprio;
  }

  line = end_fill ? "" : ml_get_buf(wp->w_buffer, lnum, false);
  ptr = line;

  if (has_spell && !number_only) {
    // For checking first word with a capital skip white space.
    if (cap_col == 0) {
      cap_col = (int)getwhitecols(line);
    }

    // To be able to spell-check over line boundaries copy the end of the
    // current line into nextline[].  Above the start of the next line was
    // copied to nextline[SPWORDLEN].
    if (nextline[SPWORDLEN] == NUL) {
      // No next line or it is empty.
      nextlinecol = MAXCOL;
      nextline_idx = 0;
    } else {
      v = (long)strlen(line);
      if (v < SPWORDLEN) {
        // Short line, use it completely and append the start of the
        // next line.
        nextlinecol = 0;
        memmove(nextline, line, (size_t)v);
        STRMOVE(nextline + v, nextline + SPWORDLEN);
        nextline_idx = (int)v + 1;
      } else {
        // Long line, use only the last SPWORDLEN bytes.
        nextlinecol = (int)v - SPWORDLEN;
        memmove(nextline, line + nextlinecol, SPWORDLEN);  // -V1086
        nextline_idx = SPWORDLEN + 1;
      }
    }
  }

  colnr_T trailcol = MAXCOL;  // start of trailing spaces
  colnr_T leadcol = 0;        // start of leading spaces

  int lcs_eol_one = wp->w_p_lcs_chars.eol;     // 'eol'  until it's been used
  int lcs_prec_todo = wp->w_p_lcs_chars.prec;  // 'prec' until it's been used

  if (wp->w_p_list && !has_fold && !end_fill) {
    if (wp->w_p_lcs_chars.space
        || wp->w_p_lcs_chars.multispace != NULL
        || wp->w_p_lcs_chars.leadmultispace != NULL
        || wp->w_p_lcs_chars.trail
        || wp->w_p_lcs_chars.lead
        || wp->w_p_lcs_chars.nbsp) {
      extra_check = true;
    }
    trailcol = get_trailcol(wp, ptr, line);
    leadcol = get_leadcol(wp, ptr, line);
  }

  // 'nowrap' or 'wrap' and a single line that doesn't fit: Advance to the
  // first character to be displayed.
  if (wp->w_p_wrap) {
    v = wp->w_skipcol;
  } else {
    v = wp->w_leftcol;
  }
  if (v > 0 && !number_only) {
    char *prev_ptr = ptr;
    chartabsize_T cts;
    int charsize = 0;

    init_chartabsize_arg(&cts, wp, lnum, wlv.vcol, line, ptr);
    while (cts.cts_vcol < v && *cts.cts_ptr != NUL) {
      charsize = win_lbr_chartabsize(&cts, NULL);
      cts.cts_vcol += charsize;
      prev_ptr = cts.cts_ptr;
      MB_PTR_ADV(cts.cts_ptr);
    }
    wlv.vcol = cts.cts_vcol;
    ptr = cts.cts_ptr;
    clear_chartabsize_arg(&cts);

    // When:
    // - 'cuc' is set, or
    // - 'colorcolumn' is set, or
    // - 'virtualedit' is set, or
    // - the visual mode is active,
    // the end of the line may be before the start of the displayed part.
    if (wlv.vcol < v && (wp->w_p_cuc
                         || draw_color_col
                         || virtual_active()
                         || (VIsual_active && wp->w_buffer == curwin->w_buffer))) {
      wlv.vcol = (colnr_T)v;
    }

    // Handle a character that's not completely on the screen: Put ptr at
    // that character but skip the first few screen characters.
    if (wlv.vcol > v) {
      wlv.vcol -= charsize;
      ptr = prev_ptr;
      // If the character fits on the screen, don't need to skip it.
      // Except for a TAB.
      if (utf_ptr2cells(ptr) >= charsize || *ptr == TAB) {
        n_skip = (int)(v - wlv.vcol);
      }
    }

    // Adjust for when the inverted text is before the screen,
    // and when the start of the inverted text is before the screen.
    if (wlv.tocol <= wlv.vcol) {
      wlv.fromcol = 0;
    } else if (wlv.fromcol >= 0 && wlv.fromcol < wlv.vcol) {
      wlv.fromcol = wlv.vcol;
    }

    // When w_skipcol is non-zero, first line needs 'showbreak'
    if (wp->w_p_wrap) {
      wlv.need_showbreak = true;
    }
    // When spell checking a word we need to figure out the start of the
    // word and if it's badly spelled or not.
    if (has_spell) {
      size_t len;
      colnr_T linecol = (colnr_T)(ptr - line);
      hlf_T spell_hlf = HLF_COUNT;

      pos = wp->w_cursor;
      wp->w_cursor.lnum = lnum;
      wp->w_cursor.col = linecol;
      len = spell_move_to(wp, FORWARD, true, true, &spell_hlf);

      // spell_move_to() may call ml_get() and make "line" invalid
      line = ml_get_buf(wp->w_buffer, lnum, false);
      ptr = line + linecol;

      if (len == 0 || (int)wp->w_cursor.col > ptr - line) {
        // no bad word found at line start, don't check until end of a
        // word
        spell_hlf = HLF_COUNT;
        word_end = (int)(spell_to_word_end(ptr, wp) - line + 1);
      } else {
        // bad word found, use attributes until end of word
        assert(len <= INT_MAX);
        word_end = wp->w_cursor.col + (int)len + 1;

        // Turn index into actual attributes.
        if (spell_hlf != HLF_COUNT) {
          spell_attr = highlight_attr[spell_hlf];
        }
      }
      wp->w_cursor = pos;

      // Need to restart syntax highlighting for this line.
      if (has_syntax) {
        syntax_start(wp, lnum);
      }
    }
  }

  // Correct highlighting for cursor that can't be disabled.
  // Avoids having to check this for each character.
  if (wlv.fromcol >= 0) {
    if (noinvcur) {
      if ((colnr_T)wlv.fromcol == wp->w_virtcol) {
        // highlighting starts at cursor, let it start just after the
        // cursor
        fromcol_prev = wlv.fromcol;
        wlv.fromcol = -1;
      } else if ((colnr_T)wlv.fromcol < wp->w_virtcol) {
        // restart highlighting after the cursor
        fromcol_prev = wp->w_virtcol;
      }
    }
    if (wlv.fromcol >= wlv.tocol) {
      wlv.fromcol = -1;
    }
  }

  if (!number_only && !has_fold && !end_fill) {
    v = ptr - line;
    area_highlighting |= prepare_search_hl_line(wp, lnum, (colnr_T)v,
                                                &line, &screen_search_hl, &search_attr,
                                                &search_attr_from_match);
    ptr = line + v;  // "line" may have been updated
  }

  win_line_start(wp, &wlv, false);

  // won't highlight after TERM_ATTRS_MAX columns
  int term_attrs[TERM_ATTRS_MAX] = { 0 };
  if (wp->w_buffer->terminal) {
    terminal_get_line_attributes(wp->w_buffer->terminal, wp, lnum, term_attrs);
    extra_check = true;
  }

  int sign_idx = 0;
  int virt_line_index;
  int virt_line_offset = -1;
  // Repeat for the whole displayed line.
  for (;;) {
    int has_match_conc = 0;  ///< match wants to conceal
    int decor_conceal = 0;

    bool did_decrement_ptr = false;

    // Skip this quickly when working on the text.
    if (wlv.draw_state != WL_LINE) {
      if (cul_screenline) {
        wlv.cul_attr = 0;
        wlv.line_attr = line_attr_save;
        wlv.line_attr_lowprio = line_attr_lowprio_save;
      }

      if (wlv.draw_state == WL_CMDLINE - 1 && wlv.n_extra == 0) {
        wlv.draw_state = WL_CMDLINE;
        if (cmdwin_type != 0 && wp == curwin) {
          // Draw the cmdline character.
          wlv.n_extra = 1;
          wlv.c_extra = cmdwin_type;
          wlv.c_final = NUL;
          wlv.char_attr = win_hl_attr(wp, HLF_AT);
        }
      }

      if (wlv.draw_state == WL_FOLD - 1 && wlv.n_extra == 0) {
        if (wlv.filler_todo > 0) {
          int index = wlv.filler_todo - (wlv.filler_lines - wlv.n_virt_lines);
          if (index > 0) {
            virt_line_index = (int)kv_size(virt_lines) - index;
            assert(virt_line_index >= 0);
            virt_line_offset = kv_A(virt_lines, virt_line_index).left_col ? 0 : win_col_off(wp);
          }
        }
        if (!virt_line_offset) {
          // Skip the column states if there is a "virt_left_col" line.
          wlv.draw_state = WL_BRI - 1;
        } else if (statuscol.draw) {
          // Skip fold, sign and number states if 'statuscolumn' is set.
          wlv.draw_state = WL_STC - 1;
        }
      }

      if (wlv.draw_state == WL_FOLD - 1 && wlv.n_extra == 0) {
        wlv.draw_state = WL_FOLD;
        handle_foldcolumn(wp, &wlv);
      }

      // sign column, this is hit until sign_idx reaches count
      if (wlv.draw_state == WL_SIGN - 1 && wlv.n_extra == 0) {
        // Show the sign column when desired.
        wlv.draw_state = WL_SIGN;
        if (wp->w_scwidth > 0) {
          get_sign_display_info(false, wp, &wlv, sign_idx, sign_cul_attr);
          sign_idx++;
          if (sign_idx < wp->w_scwidth) {
            wlv.draw_state = WL_SIGN - 1;
          } else {
            sign_idx = 0;
          }
        }
      }

      if (wlv.draw_state == WL_NR - 1 && wlv.n_extra == 0) {
        // Show the line number, if desired.
        wlv.draw_state = WL_NR;
        handle_lnum_col(wp, &wlv, num_signs, sign_idx, sign_num_attr, sign_cul_attr);
      }

      if (wlv.draw_state == WL_STC - 1 && wlv.n_extra == 0) {
        wlv.draw_state = WL_STC;
        // Draw the 'statuscolumn' if option is set.
        if (statuscol.draw) {
          if (statuscol.textp == NULL) {
            v = (ptr - line);
            get_statuscol_str(wp, lnum, wlv.row - startrow - wlv.filler_lines, &statuscol);
            if (!end_fill) {
              // Get the line again as evaluating 'statuscolumn' may free it.
              line = ml_get_buf(wp->w_buffer, lnum, false);
              ptr = line + v;
            }
            if (wp->w_redr_statuscol) {
              break;
            }
          }
          get_statuscol_display_info(&statuscol, &wlv);
        }
      }

      if (wlv.draw_state == WL_STC && wlv.n_extra == 0) {
        win_col_offset = wlv.off;
      }

      // Check if 'breakindent' applies and show it.
      // May change wlv.draw_state to WL_BRI or WL_BRI - 1.
      if (wlv.n_extra == 0) {
        handle_breakindent(wp, &wlv);
      }

      if (wlv.draw_state == WL_SBR - 1 && wlv.n_extra == 0) {
        wlv.draw_state = WL_SBR;
        handle_showbreak_and_filler(wp, &wlv);
      }

      if (wlv.draw_state == WL_LINE - 1 && wlv.n_extra == 0) {
        sign_idx = 0;
        wlv.draw_state = WL_LINE;
        if (has_decor && wlv.row == startrow + wlv.filler_lines) {
          // hide virt_text on text hidden by 'nowrap'
          decor_redraw_col(wp, wlv.vcol, wlv.off, true, &decor_state);
        }
        win_line_continue(&wlv);  // use wlv.saved_ values
      }
    }

    if (cul_screenline && wlv.draw_state == WL_LINE
        && wlv.vcol >= left_curline_col
        && wlv.vcol < right_curline_col) {
      apply_cursorline_highlight(wp, &wlv);
    }

    // When still displaying '$' of change command, stop at cursor
    if (((dollar_vcol >= 0
          && wp == curwin
          && lnum == wp->w_cursor.lnum
          && wlv.vcol >= (long)wp->w_virtcol)
         || (number_only && wlv.draw_state > WL_STC))
        && wlv.filler_todo <= 0) {
      draw_virt_text(wp, buf, win_col_offset, &wlv.col, grid->cols, wlv.row);
      grid_put_linebuf(grid, wlv.row, 0, wlv.col, -grid->cols, wp->w_p_rl, wp, bg_attr, false);
      // Pretend we have finished updating the window.  Except when
      // 'cursorcolumn' is set.
      if (wp->w_p_cuc) {
        wlv.row = wp->w_cline_row + wp->w_cline_height;
      } else {
        wlv.row = grid->rows;
      }
      break;
    }

    if (wlv.draw_state == WL_LINE
        && has_fold
        && wlv.col == win_col_offset
        && wlv.n_extra == 0
        && wlv.row == startrow + wlv.filler_lines) {
      wlv.char_attr = win_hl_attr(wp, HLF_FL);

      linenr_T lnume = lnum + foldinfo.fi_lines - 1;
      memset(buf_fold, ' ', FOLD_TEXT_LEN);
      wlv.p_extra = get_foldtext(wp, lnum, lnume, foldinfo, buf_fold);
      wlv.n_extra = (int)strlen(wlv.p_extra);

      if (wlv.p_extra != buf_fold) {
        xfree(wlv.p_extra_free);
        wlv.p_extra_free = wlv.p_extra;
      }
      wlv.c_extra = NUL;
      wlv.c_final = NUL;
      wlv.p_extra[wlv.n_extra] = NUL;

      // Get the line again as evaluating 'foldtext' may free it.
      line = ml_get_buf(wp->w_buffer, lnum, false);
      ptr = line + v;
    }

    if (wlv.draw_state == WL_LINE
        && has_fold
        && wlv.col < grid->cols
        && wlv.n_extra == 0
        && wlv.row == startrow + wlv.filler_lines) {
      // fill rest of line with 'fold'
      wlv.c_extra = wp->w_p_fcs_chars.fold;
      wlv.c_final = NUL;

      wlv.n_extra = wp->w_p_rl ? (wlv.col + 1) : (grid->cols - wlv.col);
    }

    if (wlv.draw_state == WL_LINE
        && has_fold
        && wlv.col >= grid->cols
        && wlv.n_extra != 0
        && wlv.row == startrow + wlv.filler_lines) {
      // Truncate the folding.
      wlv.n_extra = 0;
    }

    if (wlv.draw_state == WL_LINE && (area_highlighting || has_spell)) {
      // handle Visual or match highlighting in this line
      if (wlv.vcol == wlv.fromcol
          || (wlv.vcol + 1 == wlv.fromcol && wlv.n_extra == 0
              && utf_ptr2cells(ptr) > 1)
          || ((int)vcol_prev == fromcol_prev
              && vcol_prev < wlv.vcol               // not at margin
              && wlv.vcol < wlv.tocol)) {
        area_attr = vi_attr;                    // start highlighting
        if (area_highlighting) {
          area_active = true;
        }
      } else if (area_attr != 0 && (wlv.vcol == wlv.tocol
                                    || (noinvcur && wlv.vcol == wp->w_virtcol))) {
        area_attr = 0;                          // stop highlighting
        area_active = false;
      }

      if (!wlv.n_extra) {
        // Check for start/end of 'hlsearch' and other matches.
        // After end, check for start/end of next match.
        // When another match, have to check for start again.
        v = (ptr - line);
        search_attr = update_search_hl(wp, lnum, (colnr_T)v, &line, &screen_search_hl,
                                       &has_match_conc, &match_conc, lcs_eol_one,
                                       &on_last_col, &search_attr_from_match);
        ptr = line + v;  // "line" may have been changed

        // Do not allow a conceal over EOL otherwise EOL will be missed
        // and bad things happen.
        if (*ptr == NUL) {
          has_match_conc = 0;
        }
      }

      if (wlv.diff_hlf != (hlf_T)0) {
        if (wlv.diff_hlf == HLF_CHD && ptr - line >= change_start
            && wlv.n_extra == 0) {
          wlv.diff_hlf = HLF_TXD;                   // changed text
        }
        if (wlv.diff_hlf == HLF_TXD && ptr - line > change_end
            && wlv.n_extra == 0) {
          wlv.diff_hlf = HLF_CHD;                   // changed line
        }
        wlv.line_attr = win_hl_attr(wp, (int)wlv.diff_hlf);
        // Overlay CursorLine onto diff-mode highlight.
        if (wlv.cul_attr) {
          wlv.line_attr = 0 != wlv.line_attr_lowprio  // Low-priority CursorLine
            ? hl_combine_attr(hl_combine_attr(wlv.cul_attr, wlv.line_attr),
                              hl_get_underline())
            : hl_combine_attr(wlv.line_attr, wlv.cul_attr);
        }
      }

      // Decide which of the highlight attributes to use.
      attr_pri = true;

      if (area_attr != 0) {
        wlv.char_attr = hl_combine_attr(wlv.line_attr, area_attr);
        if (!highlight_match) {
          // let search highlight show in Visual area if possible
          wlv.char_attr = hl_combine_attr(search_attr, wlv.char_attr);
        }
      } else if (search_attr != 0) {
        wlv.char_attr = hl_combine_attr(wlv.line_attr, search_attr);
      } else if (wlv.line_attr != 0
                 && ((wlv.fromcol == -10 && wlv.tocol == MAXCOL)
                     || wlv.vcol < wlv.fromcol
                     || vcol_prev < fromcol_prev
                     || wlv.vcol >= wlv.tocol)) {
        // Use wlv.line_attr when not in the Visual or 'incsearch' area
        // (area_attr may be 0 when "noinvcur" is set).
        wlv.char_attr = wlv.line_attr;
      } else {
        attr_pri = false;
        if (has_syntax) {
          wlv.char_attr = syntax_attr;
        } else {
          wlv.char_attr = 0;
        }
      }
    }

    // Get the next character to put on the screen.
    //
    // The "p_extra" points to the extra stuff that is inserted to
    // represent special characters (non-printable stuff) and other
    // things.  When all characters are the same, c_extra is used.
    // If c_final is set, it will compulsorily be used at the end.
    // "p_extra" must end in a NUL to avoid utfc_ptr2len() reads past
    // "p_extra[n_extra]".
    // For the '$' of the 'list' option, n_extra == 1, p_extra == "".
    if (wlv.n_extra > 0) {
      if (wlv.c_extra != NUL || (wlv.n_extra == 1 && wlv.c_final != NUL)) {
        c = (wlv.n_extra == 1 && wlv.c_final != NUL) ? wlv.c_final : wlv.c_extra;
        mb_c = c;               // doesn't handle non-utf-8 multi-byte!
        mb_utf8 = check_mb_utf8(&c, u8cc);
      } else {
        assert(wlv.p_extra != NULL);
        c = (uint8_t)(*wlv.p_extra);
        mb_c = c;
        // If the UTF-8 character is more than one byte:
        // Decode it into "mb_c".
        mb_l = utfc_ptr2len(wlv.p_extra);
        mb_utf8 = false;
        if (mb_l > wlv.n_extra) {
          mb_l = 1;
        } else if (mb_l > 1) {
          mb_c = utfc_ptr2char(wlv.p_extra, u8cc);
          mb_utf8 = true;
          c = 0xc0;
        }
        if (mb_l == 0) {          // at the NUL at end-of-line
          mb_l = 1;
        }

        // If a double-width char doesn't fit display a '>' in the last column.
        if ((wp->w_p_rl ? (wlv.col <= 0) : (wlv.col >= grid->cols - 1))
            && utf_char2cells(mb_c) == 2) {
          c = '>';
          mb_c = c;
          mb_l = 1;
          (void)mb_l;
          multi_attr = win_hl_attr(wp, HLF_AT);

          if (wlv.cul_attr) {
            multi_attr = 0 != wlv.line_attr_lowprio
              ? hl_combine_attr(wlv.cul_attr, multi_attr)
              : hl_combine_attr(multi_attr, wlv.cul_attr);
          }

          // put the pointer back to output the double-width
          // character at the start of the next line.
          wlv.n_extra++;
          wlv.p_extra--;
        } else {
          wlv.n_extra -= mb_l - 1;
          wlv.p_extra += mb_l - 1;
        }
        wlv.p_extra++;
      }
      wlv.n_extra--;
    } else if (foldinfo.fi_lines > 0) {
      // skip writing the buffer line itself
      c = NUL;
    } else {
      int c0;

      // Get a character from the line itself.
      c0 = c = (uint8_t)(*ptr);
      mb_c = c;
      // If the UTF-8 character is more than one byte: Decode it
      // into "mb_c".
      mb_l = utfc_ptr2len(ptr);
      mb_utf8 = false;
      if (mb_l > 1) {
        mb_c = utfc_ptr2char(ptr, u8cc);
        // Overlong encoded ASCII or ASCII with composing char
        // is displayed normally, except a NUL.
        if (mb_c < 0x80) {
          c0 = c = mb_c;
        }
        mb_utf8 = true;

        // At start of the line we can have a composing char.
        // Draw it as a space with a composing char.
        if (utf_iscomposing(mb_c)) {
          for (int i = MAX_MCO - 1; i > 0; i--) {
            u8cc[i] = u8cc[i - 1];
          }
          u8cc[0] = mb_c;
          mb_c = ' ';
        }
      }

      if ((mb_l == 1 && c >= 0x80)
          || (mb_l >= 1 && mb_c == 0)
          || (mb_l > 1 && (!vim_isprintc(mb_c)))) {
        // Illegal UTF-8 byte: display as <xx>.
        // Non-BMP character : display as ? or fullwidth ?.
        transchar_hex(wlv.extra, mb_c);
        if (wp->w_p_rl) {  // reverse
          rl_mirror_ascii(wlv.extra);
        }

        wlv.p_extra = wlv.extra;
        c = (uint8_t)(*wlv.p_extra);
        mb_c = mb_ptr2char_adv((const char **)&wlv.p_extra);
        mb_utf8 = (c >= 0x80);
        wlv.n_extra = (int)strlen(wlv.p_extra);
        wlv.c_extra = NUL;
        wlv.c_final = NUL;
        if (area_attr == 0 && search_attr == 0) {
          n_attr = wlv.n_extra + 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_8);
          saved_attr2 = wlv.char_attr;               // save current attr
        }
      } else if (mb_l == 0) {        // at the NUL at end-of-line
        mb_l = 1;
      } else if (p_arshape && !p_tbidi && ARABIC_CHAR(mb_c)) {
        // Do Arabic shaping.
        int pc, pc1, nc;
        int pcc[MAX_MCO];

        // The idea of what is the previous and next
        // character depends on 'rightleft'.
        if (wp->w_p_rl) {
          pc = prev_c;
          pc1 = prev_c1;
          nc = utf_ptr2char(ptr + mb_l);
          prev_c1 = u8cc[0];
        } else {
          pc = utfc_ptr2char(ptr + mb_l, pcc);
          nc = prev_c;
          pc1 = pcc[0];
        }
        prev_c = mb_c;

        mb_c = arabic_shape(mb_c, &c, &u8cc[0], pc, pc1, nc);
      } else {
        prev_c = mb_c;
      }
      // If a double-width char doesn't fit display a '>' in the
      // last column; the character is displayed at the start of the
      // next line.
      if ((wp->w_p_rl ? (wlv.col <= 0) : (wlv.col >= grid->cols - 1))
          && utf_char2cells(mb_c) == 2) {
        c = '>';
        mb_c = c;
        mb_utf8 = false;
        mb_l = 1;
        multi_attr = win_hl_attr(wp, HLF_AT);
        // Put pointer back so that the character will be
        // displayed at the start of the next line.
        ptr--;
        did_decrement_ptr = true;
      } else if (*ptr != NUL) {
        ptr += mb_l - 1;
      }

      // If a double-width char doesn't fit at the left side display a '<' in
      // the first column.  Don't do this for unprintable characters.
      if (n_skip > 0 && mb_l > 1 && wlv.n_extra == 0) {
        wlv.n_extra = 1;
        wlv.c_extra = MB_FILLER_CHAR;
        wlv.c_final = NUL;
        c = ' ';
        if (area_attr == 0 && search_attr == 0) {
          n_attr = wlv.n_extra + 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_AT);
          saved_attr2 = wlv.char_attr;             // save current attr
        }
        mb_c = c;
        mb_utf8 = false;
        mb_l = 1;
      }
      ptr++;

      if (extra_check) {
        bool no_plain_buffer = (wp->w_s->b_p_spo_flags & SPO_NPBUFFER) != 0;
        bool can_spell = !no_plain_buffer;

        // Get syntax attribute, unless still at the start of the line
        // (double-wide char that doesn't fit).
        v = (ptr - line);
        if (has_syntax && v > 0) {
          // Get the syntax attribute for the character.  If there
          // is an error, disable syntax highlighting.
          save_did_emsg = did_emsg;
          did_emsg = false;

          syntax_attr = get_syntax_attr((colnr_T)v - 1,
                                        has_spell ? &can_spell : NULL, false);

          if (did_emsg) {  // -V547
            wp->w_s->b_syn_error = true;
            has_syntax = false;
          } else {
            did_emsg = save_did_emsg;
          }

          if (wp->w_s->b_syn_slow) {
            has_syntax = false;
          }

          // Need to get the line again, a multi-line regexp may
          // have made it invalid.
          line = ml_get_buf(wp->w_buffer, lnum, false);
          ptr = line + v;

          if (!attr_pri) {
            if (wlv.cul_attr) {
              wlv.char_attr = 0 != wlv.line_attr_lowprio
                ? hl_combine_attr(wlv.cul_attr, syntax_attr)
                : hl_combine_attr(syntax_attr, wlv.cul_attr);
            } else {
              wlv.char_attr = syntax_attr;
            }
          } else {
            wlv.char_attr = hl_combine_attr(syntax_attr, wlv.char_attr);
          }
          // no concealing past the end of the line, it interferes
          // with line highlighting.
          if (c == NUL) {
            syntax_flags = 0;
          } else {
            syntax_flags = get_syntax_info(&syntax_seqnr);
          }
        } else if (!attr_pri) {
          wlv.char_attr = 0;
        }

        if (has_decor && v > 0) {
          bool selected = (area_active || (area_highlighting && noinvcur
                                           && wlv.vcol == wp->w_virtcol));
          int extmark_attr = decor_redraw_col(wp, (colnr_T)v - 1, wlv.off,
                                              selected, &decor_state);
          if (extmark_attr != 0) {
            if (!attr_pri) {
              wlv.char_attr = hl_combine_attr(wlv.char_attr, extmark_attr);
            } else {
              wlv.char_attr = hl_combine_attr(extmark_attr, wlv.char_attr);
            }
          }

          decor_conceal = decor_state.conceal;
          if (decor_conceal && decor_state.conceal_char) {
            decor_conceal = 2;  // really??
          }

          can_spell = TRISTATE_TO_BOOL(decor_state.spell, can_spell);
        }

        // Check spelling (unless at the end of the line).
        // Only do this when there is no syntax highlighting, the
        // @Spell cluster is not used or the current syntax item
        // contains the @Spell cluster.
        v = (ptr - line);
        if (has_spell && v >= word_end && v > cur_checked_col) {
          spell_attr = 0;
          if (c != 0 && ((!has_syntax && !no_plain_buffer) || can_spell)) {
            char *prev_ptr;
            char *p;
            int len;
            hlf_T spell_hlf = HLF_COUNT;
            prev_ptr = ptr - mb_l;
            v -= mb_l - 1;

            // Use nextline[] if possible, it has the start of the
            // next line concatenated.
            if ((prev_ptr - line) - nextlinecol >= 0) {
              p = nextline + ((prev_ptr - line) - nextlinecol);
            } else {
              p = prev_ptr;
            }
            cap_col -= (int)(prev_ptr - line);
            size_t tmplen = spell_check(wp, p, &spell_hlf, &cap_col, nochange);
            assert(tmplen <= INT_MAX);
            len = (int)tmplen;
            word_end = (int)v + len;

            // In Insert mode only highlight a word that
            // doesn't touch the cursor.
            if (spell_hlf != HLF_COUNT
                && (State & MODE_INSERT)
                && wp->w_cursor.lnum == lnum
                && wp->w_cursor.col >=
                (colnr_T)(prev_ptr - line)
                && wp->w_cursor.col < (colnr_T)word_end) {
              spell_hlf = HLF_COUNT;
              spell_redraw_lnum = lnum;
            }

            if (spell_hlf == HLF_COUNT && p != prev_ptr
                && (p - nextline) + len > nextline_idx) {
              // Remember that the good word continues at the
              // start of the next line.
              checked_lnum = lnum + 1;
              checked_col = (int)((p - nextline) + len - nextline_idx);
            }

            // Turn index into actual attributes.
            if (spell_hlf != HLF_COUNT) {
              spell_attr = highlight_attr[spell_hlf];
            }

            if (cap_col > 0) {
              if (p != prev_ptr
                  && (p - nextline) + cap_col >= nextline_idx) {
                // Remember that the word in the next line
                // must start with a capital.
                capcol_lnum = lnum + 1;
                cap_col = (int)((p - nextline) + cap_col
                                - nextline_idx);
              } else {
                // Compute the actual column.
                cap_col += (int)(prev_ptr - line);
              }
            }
          }
        }
        if (spell_attr != 0) {
          if (!attr_pri) {
            wlv.char_attr = hl_combine_attr(wlv.char_attr, spell_attr);
          } else {
            wlv.char_attr = hl_combine_attr(spell_attr, wlv.char_attr);
          }
        }

        if (wp->w_buffer->terminal) {
          wlv.char_attr = hl_combine_attr(term_attrs[wlv.vcol], wlv.char_attr);
        }

        // Found last space before word: check for line break.
        if (wp->w_p_lbr && c0 == c && vim_isbreak(c)
            && !vim_isbreak((int)(*ptr))) {
          int mb_off = utf_head_off(line, ptr - 1);
          char *p = ptr - (mb_off + 1);
          chartabsize_T cts;

          init_chartabsize_arg(&cts, wp, lnum, wlv.vcol, line, p);
          wlv.n_extra = win_lbr_chartabsize(&cts, NULL) - 1;

          // We have just drawn the showbreak value, no need to add
          // space for it again.
          if (wlv.vcol == wlv.vcol_sbr) {
            wlv.n_extra -= mb_charlen(get_showbreak_value(wp));
            if (wlv.n_extra < 0) {
              wlv.n_extra = 0;
            }
          }
          if (on_last_col && c != TAB) {
            // Do not continue search/match highlighting over the
            // line break, but for TABs the highlighting should
            // include the complete width of the character
            search_attr = 0;
          }

          if (c == TAB && wlv.n_extra + wlv.col > grid->cols) {
            wlv.n_extra = tabstop_padding(wlv.vcol, wp->w_buffer->b_p_ts,
                                          wp->w_buffer->b_p_vts_array) - 1;
          }
          wlv.c_extra = mb_off > 0 ? MB_FILLER_CHAR : ' ';
          wlv.c_final = NUL;
          if (ascii_iswhite(c)) {
            if (c == TAB) {
              // See "Tab alignment" below.
              FIX_FOR_BOGUSCOLS;
            }
            if (!wp->w_p_list) {
              c = ' ';
            }
          }
          clear_chartabsize_arg(&cts);
        }

        in_multispace = c == ' ' && ((ptr > line + 1 && ptr[-2] == ' ') || *ptr == ' ');
        if (!in_multispace) {
          multispace_pos = 0;
        }

        // 'list': Change char 160 to 'nbsp' and space to 'space'.
        // But not when the character is followed by a composing
        // character (use mb_l to check that).
        if (wp->w_p_list
            && ((((c == 160 && mb_l == 1)
                  || (mb_utf8
                      && ((mb_c == 160 && mb_l == 2)
                          || (mb_c == 0x202f && mb_l == 3))))
                 && wp->w_p_lcs_chars.nbsp)
                || (c == ' '
                    && mb_l == 1
                    && (wp->w_p_lcs_chars.space
                        || (in_multispace && wp->w_p_lcs_chars.multispace != NULL))
                    && ptr - line >= leadcol
                    && ptr - line <= trailcol))) {
          if (in_multispace && wp->w_p_lcs_chars.multispace != NULL) {
            c = wp->w_p_lcs_chars.multispace[multispace_pos++];
            if (wp->w_p_lcs_chars.multispace[multispace_pos] == NUL) {
              multispace_pos = 0;
            }
          } else {
            c = (c == ' ') ? wp->w_p_lcs_chars.space : wp->w_p_lcs_chars.nbsp;
          }
          n_attr = 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_0);
          saved_attr2 = wlv.char_attr;  // save current attr
          mb_c = c;
          mb_utf8 = check_mb_utf8(&c, u8cc);
        }

        if (c == ' ' && ((trailcol != MAXCOL && ptr > line + trailcol)
                         || (leadcol != 0 && ptr < line + leadcol))) {
          if (leadcol != 0 && in_multispace && ptr < line + leadcol
              && wp->w_p_lcs_chars.leadmultispace != NULL) {
            c = wp->w_p_lcs_chars.leadmultispace[multispace_pos++];
            if (wp->w_p_lcs_chars.leadmultispace[multispace_pos] == NUL) {
              multispace_pos = 0;
            }
          } else if (ptr > line + trailcol && wp->w_p_lcs_chars.trail) {
            c = wp->w_p_lcs_chars.trail;
          } else if (ptr < line + leadcol && wp->w_p_lcs_chars.lead) {
            c = wp->w_p_lcs_chars.lead;
          } else if (leadcol != 0 && wp->w_p_lcs_chars.space) {
            c = wp->w_p_lcs_chars.space;
          }

          n_attr = 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_0);
          saved_attr2 = wlv.char_attr;  // save current attr
          mb_c = c;
          mb_utf8 = check_mb_utf8(&c, u8cc);
        }
      }

      // Handling of non-printable characters.
      if (!vim_isprintc(c)) {
        // when getting a character from the file, we may have to
        // turn it into something else on the way to putting it on the screen.
        if (c == TAB && (!wp->w_p_list || wp->w_p_lcs_chars.tab1)) {
          int tab_len = 0;
          long vcol_adjusted = wlv.vcol;  // removed showbreak length
          char *const sbr = get_showbreak_value(wp);

          // Only adjust the tab_len, when at the first column after the
          // showbreak value was drawn.
          if (*sbr != NUL && wlv.vcol == wlv.vcol_sbr && wp->w_p_wrap) {
            vcol_adjusted = wlv.vcol - mb_charlen(sbr);
          }
          // tab amount depends on current column
          tab_len = tabstop_padding((colnr_T)vcol_adjusted,
                                    wp->w_buffer->b_p_ts,
                                    wp->w_buffer->b_p_vts_array) - 1;

          if (!wp->w_p_lbr || !wp->w_p_list) {
            wlv.n_extra = tab_len;
          } else {
            char *p;
            int saved_nextra = wlv.n_extra;

            if (wlv.vcol_off > 0) {
              // there are characters to conceal
              tab_len += wlv.vcol_off;
            }
            // boguscols before FIX_FOR_BOGUSCOLS macro from above.
            if (wp->w_p_lcs_chars.tab1 && old_boguscols > 0
                && wlv.n_extra > tab_len) {
              tab_len += wlv.n_extra - tab_len;
            }

            if (tab_len > 0) {
              // If wlv.n_extra > 0, it gives the number of chars
              // to use for a tab, else we need to calculate the
              // width for a tab.
              int tab2_len = utf_char2len(wp->w_p_lcs_chars.tab2);
              int len = tab_len * tab2_len;
              if (wp->w_p_lcs_chars.tab3) {
                len += utf_char2len(wp->w_p_lcs_chars.tab3) - tab2_len;
              }
              if (wlv.n_extra > 0) {
                len += wlv.n_extra - tab_len;
              }
              c = wp->w_p_lcs_chars.tab1;
              p = xmalloc((size_t)len + 1);
              memset(p, ' ', (size_t)len);
              p[len] = NUL;
              xfree(wlv.p_extra_free);
              wlv.p_extra_free = p;
              for (int i = 0; i < tab_len; i++) {
                if (*p == NUL) {
                  tab_len = i;
                  break;
                }
                int lcs = wp->w_p_lcs_chars.tab2;

                // if tab3 is given, use it for the last char
                if (wp->w_p_lcs_chars.tab3 && i == tab_len - 1) {
                  lcs = wp->w_p_lcs_chars.tab3;
                }
                p += utf_char2bytes(lcs, p);
                wlv.n_extra += utf_char2len(lcs) - (saved_nextra > 0 ? 1 : 0);
              }
              wlv.p_extra = wlv.p_extra_free;

              // n_extra will be increased by FIX_FOX_BOGUSCOLS
              // macro below, so need to adjust for that here
              if (wlv.vcol_off > 0) {
                wlv.n_extra -= wlv.vcol_off;
              }
            }
          }

          {
            int vc_saved = wlv.vcol_off;

            // Tab alignment should be identical regardless of
            // 'conceallevel' value. So tab compensates of all
            // previous concealed characters, and thus resets
            // vcol_off and boguscols accumulated so far in the
            // line. Note that the tab can be longer than
            // 'tabstop' when there are concealed characters.
            FIX_FOR_BOGUSCOLS;

            // Make sure, the highlighting for the tab char will be
            // correctly set further below (effectively reverts the
            // FIX_FOR_BOGSUCOLS macro).
            if (wlv.n_extra == tab_len + vc_saved && wp->w_p_list
                && wp->w_p_lcs_chars.tab1) {
              tab_len += vc_saved;
            }
          }

          mb_utf8 = false;  // don't draw as UTF-8
          if (wp->w_p_list) {
            c = (wlv.n_extra == 0 && wp->w_p_lcs_chars.tab3)
                 ? wp->w_p_lcs_chars.tab3
                 : wp->w_p_lcs_chars.tab1;
            if (wp->w_p_lbr && wlv.p_extra != NULL && *wlv.p_extra != NUL) {
              wlv.c_extra = NUL;  // using p_extra from above
            } else {
              wlv.c_extra = wp->w_p_lcs_chars.tab2;
            }
            wlv.c_final = wp->w_p_lcs_chars.tab3;
            n_attr = tab_len + 1;
            wlv.extra_attr = win_hl_attr(wp, HLF_0);
            saved_attr2 = wlv.char_attr;  // save current attr
            mb_c = c;
            mb_utf8 = check_mb_utf8(&c, u8cc);
          } else {
            wlv.c_final = NUL;
            wlv.c_extra = ' ';
            c = ' ';
          }
        } else if (c == NUL
                   && (wp->w_p_list
                       || ((wlv.fromcol >= 0 || fromcol_prev >= 0)
                           && wlv.tocol > wlv.vcol
                           && VIsual_mode != Ctrl_V
                           && (wp->w_p_rl ? (wlv.col >= 0) : (wlv.col < grid->cols))
                           && !(noinvcur
                                && lnum == wp->w_cursor.lnum
                                && wlv.vcol == wp->w_virtcol)))
                   && lcs_eol_one > 0) {
          // Display a '$' after the line or highlight an extra
          // character if the line break is included.
          // For a diff line the highlighting continues after the "$".
          if (wlv.diff_hlf == (hlf_T)0
              && wlv.line_attr == 0
              && wlv.line_attr_lowprio == 0) {
            // In virtualedit, visual selections may extend beyond end of line
            if (area_highlighting && virtual_active()
                && wlv.tocol != MAXCOL && wlv.vcol < wlv.tocol) {
              wlv.n_extra = 0;
            } else {
              wlv.p_extra = at_end_str;
              wlv.n_extra = 1;
              wlv.c_extra = NUL;
              wlv.c_final = NUL;
            }
          }
          if (wp->w_p_list && wp->w_p_lcs_chars.eol > 0) {
            c = wp->w_p_lcs_chars.eol;
          } else {
            c = ' ';
          }
          lcs_eol_one = -1;
          ptr--;  // put it back at the NUL
          wlv.extra_attr = win_hl_attr(wp, HLF_AT);
          n_attr = 1;
          mb_c = c;
          mb_utf8 = check_mb_utf8(&c, u8cc);
        } else if (c != NUL) {
          wlv.p_extra = transchar_buf(wp->w_buffer, c);
          if (wlv.n_extra == 0) {
            wlv.n_extra = byte2cells(c) - 1;
          }
          if ((dy_flags & DY_UHEX) && wp->w_p_rl) {
            rl_mirror_ascii(wlv.p_extra);                 // reverse "<12>"
          }
          wlv.c_extra = NUL;
          wlv.c_final = NUL;
          if (wp->w_p_lbr) {
            char *p;

            c = (uint8_t)(*wlv.p_extra);
            p = xmalloc((size_t)wlv.n_extra + 1);
            memset(p, ' ', (size_t)wlv.n_extra);
            strncpy(p,  // NOLINT(runtime/printf)
                    wlv.p_extra + 1,
                    (size_t)strlen(wlv.p_extra) - 1);
            p[wlv.n_extra] = NUL;
            xfree(wlv.p_extra_free);
            wlv.p_extra_free = wlv.p_extra = p;
          } else {
            wlv.n_extra = byte2cells(c) - 1;
            c = (uint8_t)(*wlv.p_extra++);
          }
          n_attr = wlv.n_extra + 1;
          wlv.extra_attr = win_hl_attr(wp, HLF_8);
          saved_attr2 = wlv.char_attr;  // save current attr
          mb_utf8 = false;   // don't draw as UTF-8
        } else if (VIsual_active
                   && (VIsual_mode == Ctrl_V || VIsual_mode == 'v')
                   && virtual_active()
                   && wlv.tocol != MAXCOL
                   && wlv.vcol < wlv.tocol
                   && (wp->w_p_rl ? (wlv.col >= 0) : (wlv.col < grid->cols))) {
          c = ' ';
          ptr--;  // put it back at the NUL
        }
      }

      if (wp->w_p_cole > 0
          && (wp != curwin || lnum != wp->w_cursor.lnum || conceal_cursor_line(wp))
          && ((syntax_flags & HL_CONCEAL) != 0 || has_match_conc > 0 || decor_conceal > 0)
          && !(lnum_in_visual_area && vim_strchr(wp->w_p_cocu, 'v') == NULL)) {
        wlv.char_attr = conceal_attr;
        if (((prev_syntax_id != syntax_seqnr && (syntax_flags & HL_CONCEAL) != 0)
             || has_match_conc > 1 || decor_conceal > 1)
            && (syn_get_sub_char() != NUL
                || (has_match_conc && match_conc)
                || (decor_conceal && decor_state.conceal_char)
                || wp->w_p_cole == 1)
            && wp->w_p_cole != 3) {
          // First time at this concealed item: display one
          // character.
          if (has_match_conc && match_conc) {
            c = match_conc;
          } else if (decor_conceal && decor_state.conceal_char) {
            c = decor_state.conceal_char;
            if (decor_state.conceal_attr) {
              wlv.char_attr = decor_state.conceal_attr;
            }
          } else if (syn_get_sub_char() != NUL) {
            c = syn_get_sub_char();
          } else if (wp->w_p_lcs_chars.conceal != NUL) {
            c = wp->w_p_lcs_chars.conceal;
          } else {
            c = ' ';
          }

          prev_syntax_id = syntax_seqnr;

          if (wlv.n_extra > 0) {
            wlv.vcol_off += wlv.n_extra;
          }
          wlv.vcol += wlv.n_extra;
          if (wp->w_p_wrap && wlv.n_extra > 0) {
            if (wp->w_p_rl) {
              wlv.col -= wlv.n_extra;
              wlv.boguscols -= wlv.n_extra;
            } else {
              wlv.boguscols += wlv.n_extra;
              wlv.col += wlv.n_extra;
            }
          }
          wlv.n_extra = 0;
          n_attr = 0;
        } else if (n_skip == 0) {
          is_concealing = true;
          n_skip = 1;
        }
        mb_c = c;
        mb_utf8 = check_mb_utf8(&c, u8cc);
      } else {
        prev_syntax_id = 0;
        is_concealing = false;
      }

      if (n_skip > 0 && did_decrement_ptr) {
        // not showing the '>', put pointer back to avoid getting stuck
        ptr++;
      }
    }  // end of printing from buffer content

    // In the cursor line and we may be concealing characters: correct
    // the cursor column when we reach its position.
    if (!did_wcol && wlv.draw_state == WL_LINE
        && wp == curwin && lnum == wp->w_cursor.lnum
        && conceal_cursor_line(wp)
        && (int)wp->w_virtcol <= wlv.vcol + n_skip) {
      if (wp->w_p_rl) {
        wp->w_wcol = grid->cols - wlv.col + wlv.boguscols - 1;
      } else {
        wp->w_wcol = wlv.col - wlv.boguscols;
      }
      wp->w_wrow = wlv.row;
      did_wcol = true;
      wp->w_valid |= VALID_WCOL|VALID_WROW|VALID_VIRTCOL;
    }

    // Don't override visual selection highlighting.
    if (n_attr > 0 && wlv.draw_state == WL_LINE && !search_attr_from_match) {
      wlv.char_attr = hl_combine_attr(wlv.char_attr, wlv.extra_attr);
    }

    // Handle the case where we are in column 0 but not on the first
    // character of the line and the user wants us to show us a
    // special character (via 'listchars' option "precedes:<char>".
    if (lcs_prec_todo != NUL
        && wp->w_p_list
        && (wp->w_p_wrap ? (wp->w_skipcol > 0 && wlv.row == 0) : wp->w_leftcol > 0)
        && wlv.filler_todo <= 0
        && wlv.draw_state > WL_STC
        && c != NUL) {
      c = wp->w_p_lcs_chars.prec;
      lcs_prec_todo = NUL;
      if (utf_char2cells(mb_c) > 1) {
        // Double-width character being overwritten by the "precedes"
        // character, need to fill up half the character.
        wlv.c_extra = MB_FILLER_CHAR;
        wlv.c_final = NUL;
        wlv.n_extra = 1;
        n_attr = 2;
        wlv.extra_attr = win_hl_attr(wp, HLF_AT);
      }
      mb_c = c;
      mb_utf8 = check_mb_utf8(&c, u8cc);
      saved_attr3 = wlv.char_attr;  // save current attr
      wlv.char_attr = win_hl_attr(wp, HLF_AT);  // overwriting char_attr
      n_attr3 = 1;
    }

    // At end of the text line or just after the last character.
    if (c == NUL && eol_hl_off == 0) {
      // flag to indicate whether prevcol equals startcol of search_hl or
      // one of the matches
      bool prevcol_hl_flag = get_prevcol_hl_flag(wp, &screen_search_hl,
                                                 (long)(ptr - line) - 1);  // NOLINT(google-readability-casting)

      // Invert at least one char, used for Visual and empty line or
      // highlight match at end of line. If it's beyond the last
      // char on the screen, just overwrite that one (tricky!)  Not
      // needed when a '$' was displayed for 'list'.
      if (wp->w_p_lcs_chars.eol == lcs_eol_one
          && ((area_attr != 0 && wlv.vcol == wlv.fromcol
               && (VIsual_mode != Ctrl_V
                   || lnum == VIsual.lnum
                   || lnum == curwin->w_cursor.lnum))
              // highlight 'hlsearch' match at end of line
              || prevcol_hl_flag)) {
        int n = 0;

        if (wp->w_p_rl) {
          if (wlv.col < 0) {
            n = 1;
          }
        } else {
          if (wlv.col >= grid->cols) {
            n = -1;
          }
        }
        if (n != 0) {
          // At the window boundary, highlight the last character
          // instead (better than nothing).
          wlv.off += n;
          wlv.col += n;
        } else {
          // Add a blank character to highlight.
          schar_from_ascii(linebuf_char[wlv.off], ' ');
        }
        if (area_attr == 0 && !has_fold) {
          // Use attributes from match with highest priority among
          // 'search_hl' and the match list.
          get_search_match_hl(wp,
                              &screen_search_hl,
                              (long)(ptr - line),  // NOLINT(google-readability-casting)
                              &wlv.char_attr);
        }

        int eol_attr = wlv.char_attr;
        if (wlv.cul_attr) {
          eol_attr = hl_combine_attr(wlv.cul_attr, eol_attr);
        }
        linebuf_attr[wlv.off] = eol_attr;
        if (wp->w_p_rl) {
          wlv.col--;
          wlv.off--;
        } else {
          wlv.col++;
          wlv.off++;
        }
        wlv.vcol++;
        eol_hl_off = 1;
      }
    }

    // At end of the text line.
    if (c == NUL) {
      // Highlight 'cursorcolumn' & 'colorcolumn' past end of the line.
      if (wp->w_p_wrap) {
        v = wp->w_skipcol;
      } else {
        v = wp->w_leftcol;
      }

      // check if line ends before left margin
      if (wlv.vcol < v + wlv.col - win_col_off(wp)) {
        wlv.vcol = (colnr_T)v + wlv.col - win_col_off(wp);
      }
      // Get rid of the boguscols now, we want to draw until the right
      // edge for 'cursorcolumn'.
      wlv.col -= wlv.boguscols;
      wlv.boguscols = 0;

      if (draw_color_col) {
        draw_color_col = advance_color_col(VCOL_HLC, &color_cols);
      }

      bool has_virttext = false;
      // Make sure alignment is the same regardless
      // if listchars=eol:X is used or not.
      int eol_skip = (wp->w_p_lcs_chars.eol == lcs_eol_one && eol_hl_off == 0
                      ? 1 : 0);

      if (has_decor) {
        has_virttext = decor_redraw_eol(wp, &decor_state, &wlv.line_attr,
                                        wlv.col + eol_skip);
      }

      if (((wp->w_p_cuc
            && (int)wp->w_virtcol >= VCOL_HLC - eol_hl_off
            && (int)wp->w_virtcol <
            (long)grid->cols * (wlv.row - startrow + 1) + v
            && lnum != wp->w_cursor.lnum)
           || draw_color_col || wlv.line_attr_lowprio || wlv.line_attr
           || wlv.diff_hlf != (hlf_T)0 || has_virttext)) {
        int rightmost_vcol = 0;

        if (wp->w_p_cuc) {
          rightmost_vcol = wp->w_virtcol;
        }

        if (draw_color_col) {
          // determine rightmost colorcolumn to possibly draw
          for (int i = 0; color_cols[i] >= 0; i++) {
            if (rightmost_vcol < color_cols[i]) {
              rightmost_vcol = color_cols[i];
            }
          }
        }

        int cuc_attr = win_hl_attr(wp, HLF_CUC);
        int mc_attr = win_hl_attr(wp, HLF_MC);

        int diff_attr = 0;
        if (wlv.diff_hlf == HLF_TXD) {
          wlv.diff_hlf = HLF_CHD;
        }
        if (wlv.diff_hlf != 0) {
          diff_attr = win_hl_attr(wp, (int)wlv.diff_hlf);
        }

        int base_attr = hl_combine_attr(wlv.line_attr_lowprio, diff_attr);
        if (base_attr || wlv.line_attr || has_virttext) {
          rightmost_vcol = INT_MAX;
        }

        int col_stride = wp->w_p_rl ? -1 : 1;

        while (wp->w_p_rl ? wlv.col >= 0 : wlv.col < grid->cols) {
          schar_from_ascii(linebuf_char[wlv.off], ' ');
          wlv.col += col_stride;
          if (draw_color_col) {
            draw_color_col = advance_color_col(VCOL_HLC, &color_cols);
          }

          int col_attr = base_attr;

          if (wp->w_p_cuc && VCOL_HLC == (long)wp->w_virtcol) {
            col_attr = cuc_attr;
          } else if (draw_color_col && VCOL_HLC == *color_cols) {
            col_attr = hl_combine_attr(wlv.line_attr_lowprio, mc_attr);
          }

          col_attr = hl_combine_attr(col_attr, wlv.line_attr);

          linebuf_attr[wlv.off] = col_attr;
          wlv.off += col_stride;

          if (VCOL_HLC >= rightmost_vcol) {
            break;
          }

          wlv.vcol += 1;
        }
      }

      // TODO(bfredl): integrate with the common beyond-the-end-loop
      if (wp->w_buffer->terminal) {
        // terminal buffers may need to highlight beyond the end of the
        // logical line
        int n = wp->w_p_rl ? -1 : 1;
        while (wlv.col >= 0 && wlv.col < grid->cols) {
          schar_from_ascii(linebuf_char[wlv.off], ' ');
          linebuf_attr[wlv.off] = wlv.vcol >= TERM_ATTRS_MAX ? 0 : term_attrs[wlv.vcol];
          wlv.off += n;
          wlv.vcol += n;
          wlv.col += n;
        }
      }

      draw_virt_text(wp, buf, win_col_offset, &wlv.col, grid->cols, wlv.row);
      grid_put_linebuf(grid, wlv.row, 0, wlv.col, grid->cols, wp->w_p_rl, wp, bg_attr, false);
      wlv.row++;

      // Update w_cline_height and w_cline_folded if the cursor line was
      // updated (saves a call to plines_win() later).
      if (wp == curwin && lnum == curwin->w_cursor.lnum) {
        curwin->w_cline_row = startrow;
        curwin->w_cline_height = wlv.row - startrow;
        curwin->w_cline_folded = foldinfo.fi_lines > 0;
        curwin->w_valid |= (VALID_CHEIGHT|VALID_CROW);
        conceal_cursor_used = conceal_cursor_line(curwin);
      }
      break;
    }

    // Show "extends" character from 'listchars' if beyond the line end and
    // 'list' is set.
    if (wp->w_p_lcs_chars.ext != NUL
        && wlv.draw_state == WL_LINE
        && wp->w_p_list
        && !wp->w_p_wrap
        && wlv.filler_todo <= 0
        && (wp->w_p_rl ? wlv.col == 0 : wlv.col == grid->cols - 1)
        && !has_fold
        && (*ptr != NUL
            || lcs_eol_one > 0
            || (wlv.n_extra && (wlv.c_extra != NUL || *wlv.p_extra != NUL)))) {
      c = wp->w_p_lcs_chars.ext;
      wlv.char_attr = win_hl_attr(wp, HLF_AT);
      mb_c = c;
      mb_utf8 = check_mb_utf8(&c, u8cc);
    }

    // advance to the next 'colorcolumn'
    if (draw_color_col) {
      draw_color_col = advance_color_col(VCOL_HLC, &color_cols);
    }

    // Highlight the cursor column if 'cursorcolumn' is set.  But don't
    // highlight the cursor position itself.
    // Also highlight the 'colorcolumn' if it is different than
    // 'cursorcolumn'
    // Also highlight the 'colorcolumn' if 'breakindent' and/or 'showbreak'
    // options are set
    vcol_save_attr = -1;
    if ((wlv.draw_state == WL_LINE
         || wlv.draw_state == WL_BRI
         || wlv.draw_state == WL_SBR)
        && !lnum_in_visual_area
        && search_attr == 0
        && area_attr == 0
        && wlv.filler_todo <= 0) {
      if (wp->w_p_cuc && VCOL_HLC == (long)wp->w_virtcol
          && lnum != wp->w_cursor.lnum) {
        vcol_save_attr = wlv.char_attr;
        wlv.char_attr = hl_combine_attr(win_hl_attr(wp, HLF_CUC), wlv.char_attr);
      } else if (draw_color_col && VCOL_HLC == *color_cols) {
        vcol_save_attr = wlv.char_attr;
        wlv.char_attr = hl_combine_attr(win_hl_attr(wp, HLF_MC), wlv.char_attr);
      }
    }

    // Apply lowest-priority line attr now, so everything can override it.
    if (wlv.draw_state == WL_LINE) {
      wlv.char_attr = hl_combine_attr(wlv.line_attr_lowprio, wlv.char_attr);
    }

    // Store character to be displayed.
    // Skip characters that are left of the screen for 'nowrap'.
    vcol_prev = wlv.vcol;
    if (wlv.draw_state < WL_LINE || n_skip <= 0) {
      //
      // Store the character.
      //
      if (wp->w_p_rl && utf_char2cells(mb_c) > 1) {
        // A double-wide character is: put first half in left cell.
        wlv.off--;
        wlv.col--;
      }
      if (mb_utf8) {
        schar_from_cc(linebuf_char[wlv.off], mb_c, u8cc);
      } else {
        schar_from_ascii(linebuf_char[wlv.off], (char)c);
      }
      if (multi_attr) {
        linebuf_attr[wlv.off] = multi_attr;
        multi_attr = 0;
      } else {
        linebuf_attr[wlv.off] = wlv.char_attr;
      }

      if (utf_char2cells(mb_c) > 1) {
        // Need to fill two screen columns.
        wlv.off++;
        wlv.col++;
        // UTF-8: Put a 0 in the second screen char.
        linebuf_char[wlv.off][0] = 0;
        linebuf_attr[wlv.off] = linebuf_attr[wlv.off - 1];
        if (wlv.draw_state > WL_STC && wlv.filler_todo <= 0) {
          wlv.vcol++;
        }
        // When "wlv.tocol" is halfway through a character, set it to the end
        // of the character, otherwise highlighting won't stop.
        if (wlv.tocol == wlv.vcol) {
          wlv.tocol++;
        }
        if (wp->w_p_rl) {
          // now it's time to backup one cell
          wlv.off--;
          wlv.col--;
        }
      }
      if (wp->w_p_rl) {
        wlv.off--;
        wlv.col--;
      } else {
        wlv.off++;
        wlv.col++;
      }
    } else if (wp->w_p_cole > 0 && is_concealing) {
      n_skip--;
      wlv.vcol_off++;
      if (wlv.n_extra > 0) {
        wlv.vcol_off += wlv.n_extra;
      }
      if (wp->w_p_wrap) {
        // Special voodoo required if 'wrap' is on.
        //
        // Advance the column indicator to force the line
        // drawing to wrap early. This will make the line
        // take up the same screen space when parts are concealed,
        // so that cursor line computations aren't messed up.
        //
        // To avoid the fictitious advance of 'wlv.col' causing
        // trailing junk to be written out of the screen line
        // we are building, 'boguscols' keeps track of the number
        // of bad columns we have advanced.
        if (wlv.n_extra > 0) {
          wlv.vcol += wlv.n_extra;
          if (wp->w_p_rl) {
            wlv.col -= wlv.n_extra;
            wlv.boguscols -= wlv.n_extra;
          } else {
            wlv.col += wlv.n_extra;
            wlv.boguscols += wlv.n_extra;
          }
          wlv.n_extra = 0;
          n_attr = 0;
        }

        if (utf_char2cells(mb_c) > 1) {
          // Need to fill two screen columns.
          if (wp->w_p_rl) {
            wlv.boguscols--;
            wlv.col--;
          } else {
            wlv.boguscols++;
            wlv.col++;
          }
        }

        if (wp->w_p_rl) {
          wlv.boguscols--;
          wlv.col--;
        } else {
          wlv.boguscols++;
          wlv.col++;
        }
      } else {
        if (wlv.n_extra > 0) {
          wlv.vcol += wlv.n_extra;
          wlv.n_extra = 0;
          n_attr = 0;
        }
      }
    } else {
      n_skip--;
    }

    // Only advance the "wlv.vcol" when after the 'number' or
    // 'relativenumber' column.
    if (wlv.draw_state > WL_STC
        && wlv.filler_todo <= 0) {
      wlv.vcol++;
    }

    if (vcol_save_attr >= 0) {
      wlv.char_attr = vcol_save_attr;
    }

    // restore attributes after "predeces" in 'listchars'
    if (wlv.draw_state > WL_STC && n_attr3 > 0 && --n_attr3 == 0) {
      wlv.char_attr = saved_attr3;
    }

    // restore attributes after last 'listchars' or 'number' char
    if (n_attr > 0 && wlv.draw_state == WL_LINE && --n_attr == 0) {
      wlv.char_attr = saved_attr2;
    }

    // At end of screen line and there is more to come: Display the line
    // so far.  If there is no more to display it is caught above.
    if ((wp->w_p_rl ? (wlv.col < 0) : (wlv.col >= grid->cols))
        && (!has_fold || virt_line_offset >= 0)
        && (wlv.draw_state != WL_LINE
            || *ptr != NUL
            || wlv.filler_todo > 0
            || (wp->w_p_list && wp->w_p_lcs_chars.eol != NUL
                && wlv.p_extra != at_end_str)
            || (wlv.n_extra != 0
                && (wlv.c_extra != NUL || *wlv.p_extra != NUL)))) {
      bool wrap = wp->w_p_wrap       // Wrapping enabled.
                  && wlv.filler_todo <= 0          // Not drawing diff filler lines.
                  && lcs_eol_one != -1         // Haven't printed the lcs_eol character.
                  && wlv.row != endrow - 1     // Not the last line being displayed.
                  && (grid->cols == Columns  // Window spans the width of the screen,
                      || ui_has(kUIMultigrid))  // or has dedicated grid.
                  && !wp->w_p_rl;              // Not right-to-left.

      int draw_col = wlv.col - wlv.boguscols;
      if (virt_line_offset >= 0) {
        draw_virt_text_item(buf, virt_line_offset, kv_A(virt_lines, virt_line_index).line,
                            kHlModeReplace, grid->cols, 0);
      } else {
        draw_virt_text(wp, buf, win_col_offset, &draw_col, grid->cols, wlv.row);
      }

      grid_put_linebuf(grid, wlv.row, 0, draw_col, grid->cols, wp->w_p_rl, wp, bg_attr, wrap);
      if (wrap) {
        ScreenGrid *current_grid = grid;
        int current_row = wlv.row, dummy_col = 0;  // dummy_col unused
        grid_adjust(&current_grid, &current_row, &dummy_col);

        // Force a redraw of the first column of the next line.
        current_grid->attrs[current_grid->line_offset[current_row + 1]] = -1;

        // Remember that the line wraps, used for modeless copy.
        current_grid->line_wraps[current_row] = true;
      }

      wlv.boguscols = 0;
      wlv.row++;

      // When not wrapping and finished diff lines, or when displayed
      // '$' and highlighting until last column, break here.
      if ((!wp->w_p_wrap && wlv.filler_todo <= 0) || lcs_eol_one == -1) {
        break;
      }

      // When the window is too narrow draw all "@" lines.
      if (wlv.draw_state != WL_LINE && wlv.filler_todo <= 0) {
        win_draw_end(wp, '@', ' ', true, wlv.row, wp->w_grid.rows, HLF_AT);
        set_empty_rows(wp, wlv.row);
        wlv.row = endrow;
      }

      // When line got too long for screen break here.
      if (wlv.row == endrow) {
        wlv.row++;
        break;
      }

      win_line_start(wp, &wlv, true);

      lcs_prec_todo = wp->w_p_lcs_chars.prec;
      if (wlv.filler_todo <= 0) {
        wlv.need_showbreak = true;
      }
      if (statuscol.draw) {
        if (wlv.row == startrow + wlv.filler_lines) {
          statuscol.textp = NULL;  // re-evaluate for first non-filler line
        } else if (vim_strchr(p_cpo, CPO_NUMCOL) && wlv.row > startrow + wlv.filler_lines) {
          statuscol.draw = false;  // don't draw status column if "n" is in 'cpo'
        } else if (wlv.row == startrow + wlv.filler_lines + 1) {
          statuscol.textp = NULL;  // re-evaluate for first wrapped line
        } else {
          // Draw the already built 'statuscolumn' on the next wrapped or filler line
          statuscol.textp = statuscol.text;
          statuscol.hlrecp = statuscol.hlrec;
        }
      }
      wlv.filler_todo--;
      virt_line_offset = -1;
      // When the filler lines are actually below the last line of the
      // file, don't draw the line itself, break here.
      if (wlv.filler_todo == 0 && (wp->w_botfill || end_fill)) {
        break;
      }
    }
  }     // for every character in the line

  // After an empty line check first word for capital.
  if (*skipwhite(line) == NUL) {
    capcol_lnum = lnum + 1;
    cap_col = 0;
  }

  kv_destroy(virt_lines);
  xfree(wlv.p_extra_free);
  return wlv.row;
}

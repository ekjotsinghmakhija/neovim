// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

// fileio.c: read from and write to a file

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <uv.h>

#include "auto/config.h"
#include "nvim/ascii.h"
#include "nvim/autocmd.h"
#include "nvim/buffer.h"
#include "nvim/buffer_defs.h"
#include "nvim/buffer_updates.h"
#include "nvim/change.h"
#include "nvim/cursor.h"
#include "nvim/diff.h"
#include "nvim/drawscreen.h"
#include "nvim/edit.h"
#include "nvim/eval.h"
#include "nvim/ex_cmds.h"
#include "nvim/ex_eval.h"
#include "nvim/fileio.h"
#include "nvim/fold.h"
#include "nvim/garray.h"
#include "nvim/getchar.h"
#include "nvim/gettext.h"
#include "nvim/globals.h"
#include "nvim/highlight_defs.h"
#include "nvim/iconv.h"
#include "nvim/input.h"
#include "nvim/log.h"
#include "nvim/macros.h"
#include "nvim/mbyte.h"
#include "nvim/memfile.h"
#include "nvim/memline.h"
#include "nvim/memory.h"
#include "nvim/message.h"
#include "nvim/move.h"
#include "nvim/option.h"
#include "nvim/optionstr.h"
#include "nvim/os/fs_defs.h"
#include "nvim/os/input.h"
#include "nvim/os/os.h"
#include "nvim/os/time.h"
#include "nvim/path.h"
#include "nvim/pos.h"
#include "nvim/regexp.h"
#include "nvim/sha256.h"
#include "nvim/shada.h"
#include "nvim/strings.h"
#include "nvim/types.h"
#include "nvim/ui.h"
#include "nvim/undo.h"
#include "nvim/vim.h"

#ifdef BACKSLASH_IN_FILENAME
# include "nvim/charset.h"
#endif

#ifdef HAVE_DIRFD_AND_FLOCK
# include <dirent.h>
# include <sys/file.h>
#endif

#ifdef OPEN_CHR_FILES
# include "nvim/charset.h"
#endif

#define BUFSIZE         8192    // size of normal write buffer
#define SMBUFSIZE       256     // size of emergency write buffer

// For compatibility with libuv < 1.20.0 (tested on 1.18.0)
#ifndef UV_FS_COPYFILE_FICLONE
# define UV_FS_COPYFILE_FICLONE 0
#endif

enum {
  FIO_LATIN1 = 0x01,       // convert Latin1
  FIO_UTF8 = 0x02,         // convert UTF-8
  FIO_UCS2 = 0x04,         // convert UCS-2
  FIO_UCS4 = 0x08,         // convert UCS-4
  FIO_UTF16 = 0x10,        // convert UTF-16
  FIO_ENDIAN_L = 0x80,     // little endian
  FIO_NOCONVERT = 0x2000,  // skip encoding conversion
  FIO_UCSBOM = 0x4000,     // check for BOM at start of file
  FIO_ALL = -1,            // allow all formats
};

// When converting, a read() or write() may leave some bytes to be converted
// for the next call.  The value is guessed...
#define CONV_RESTLEN 30

// We have to guess how much a sequence of bytes may expand when converting
// with iconv() to be able to allocate a buffer.
#define ICONV_MULT 8

// Structure to pass arguments from buf_write() to buf_write_bytes().
struct bw_info {
  int bw_fd;                      // file descriptor
  char *bw_buf;                   // buffer with data to be written
  int bw_len;                     // length of data
  int bw_flags;                   // FIO_ flags
  uint8_t bw_rest[CONV_RESTLEN];  // not converted bytes
  int bw_restlen;                 // nr of bytes in bw_rest[]
  int bw_first;                   // first write call
  char *bw_conv_buf;              // buffer for writing converted chars
  size_t bw_conv_buflen;          // size of bw_conv_buf
  int bw_conv_error;              // set for conversion error
  linenr_T bw_conv_error_lnum;    // first line with error or zero
  linenr_T bw_start_lnum;         // line number at start of buffer
  iconv_t bw_iconv_fd;            // descriptor for iconv() or -1
};

typedef struct {
  const char *num;
  char *msg;
  int arg;
  bool alloc;
} Error_T;

static char *err_readonly = "is read-only (cannot override: \"W\" in 'cpoptions')";

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "fileio.c.generated.h"
#endif

static const char *e_auchangedbuf = N_("E812: Autocommands changed buffer or buffer name");
static const char e_no_matching_autocommands_for_buftype_str_buffer[]
  = N_("E676: No matching autocommands for buftype=%s buffer");

void filemess(buf_T *buf, char *name, char *s, int attr)
{
  int msg_scroll_save;

  if (msg_silent != 0) {
    return;
  }
  add_quoted_fname(IObuff, IOSIZE - 100, buf, (const char *)name);
  // Avoid an over-long translation to cause trouble.
  xstrlcat(IObuff, s, IOSIZE);
  // For the first message may have to start a new line.
  // For further ones overwrite the previous one, reset msg_scroll before
  // calling filemess().
  msg_scroll_save = msg_scroll;
  if (shortmess(SHM_OVERALL) && !exiting && p_verbose == 0) {
    msg_scroll = false;
  }
  if (!msg_scroll) {    // wait a bit when overwriting an error msg
    msg_check_for_delay(false);
  }
  msg_start();
  msg_scroll = msg_scroll_save;
  msg_scrolled_ign = true;
  // may truncate the message to avoid a hit-return prompt
  msg_outtrans_attr(msg_may_trunc(false, IObuff), attr);
  msg_clr_eos();
  ui_flush();
  msg_scrolled_ign = false;
}

/// Read lines from file "fname" into the buffer after line "from".
///
/// 1. We allocate blocks with try_malloc, as big as possible.
/// 2. Each block is filled with characters from the file with a single read().
/// 3. The lines are inserted in the buffer with ml_append().
///
/// (caller must check that fname != NULL, unless READ_STDIN is used)
///
/// "lines_to_skip" is the number of lines that must be skipped
/// "lines_to_read" is the number of lines that are appended
/// When not recovering lines_to_skip is 0 and lines_to_read MAXLNUM.
///
/// flags:
/// READ_NEW     starting to edit a new buffer
/// READ_FILTER  reading filter output
/// READ_STDIN   read from stdin instead of a file
/// READ_BUFFER  read from curbuf instead of a file (converting after reading
///              stdin)
/// READ_NOFILE  do not read a file, only trigger BufReadCmd
/// READ_DUMMY   read into a dummy buffer (to check if file contents changed)
/// READ_KEEP_UNDO  don't clear undo info or read it from a file
/// READ_FIFO    read from fifo/socket instead of a file
///
/// @param eap  can be NULL!
///
/// @return     FAIL for failure, NOTDONE for directory (failure), or OK
int readfile(char *fname, char *sfname, linenr_T from, linenr_T lines_to_skip,
             linenr_T lines_to_read, exarg_T *eap, int flags, bool silent)
{
  int fd = stdin_fd >= 0 ? stdin_fd : 0;
  int newfile = (flags & READ_NEW);
  int check_readonly;
  int filtering = (flags & READ_FILTER);
  int read_stdin = (flags & READ_STDIN);
  int read_buffer = (flags & READ_BUFFER);
  int read_fifo = (flags & READ_FIFO);
  int set_options = newfile || read_buffer
                    || (eap != NULL && eap->read_edit);
  linenr_T read_buf_lnum = 1;           // next line to read from curbuf
  colnr_T read_buf_col = 0;             // next char to read from this line
  char c;
  linenr_T lnum = from;
  char *ptr = NULL;              // pointer into read buffer
  char *buffer = NULL;           // read buffer
  char *new_buffer = NULL;       // init to shut up gcc
  char *line_start = NULL;       // init to shut up gcc
  int wasempty;                         // buffer was empty before reading
  colnr_T len;
  ptrdiff_t size = 0;
  uint8_t *p = NULL;
  off_T filesize = 0;
  bool skip_read = false;
  context_sha256_T sha_ctx;
  int read_undo_file = false;
  int split = 0;  // number of split lines
  linenr_T linecnt;
  bool error = false;                   // errors encountered
  int ff_error = EOL_UNKNOWN;           // file format with errors
  ptrdiff_t linerest = 0;               // remaining chars in line
  int perm = 0;
#ifdef UNIX
  int swap_mode = -1;                   // protection bits for swap file
#endif
  int fileformat = 0;                   // end-of-line format
  bool keep_fileformat = false;
  FileInfo file_info;
  linenr_T skip_count = 0;
  linenr_T read_count = 0;
  int msg_save = msg_scroll;
  linenr_T read_no_eol_lnum = 0;        // non-zero lnum when last line of
                                        // last read was missing the eol
  bool file_rewind = false;
  int can_retry;
  linenr_T conv_error = 0;              // line nr with conversion error
  linenr_T illegal_byte = 0;            // line nr with illegal byte
  bool keep_dest_enc = false;           // don't retry when char doesn't fit
                                        // in destination encoding
  int bad_char_behavior = BAD_REPLACE;
  // BAD_KEEP, BAD_DROP or character to
  // replace with
  char *tmpname = NULL;          // name of 'charconvert' output file
  int fio_flags = 0;
  char *fenc;                    // fileencoding to use
  bool fenc_alloced;                    // fenc_next is in allocated memory
  char *fenc_next = NULL;        // next item in 'fencs' or NULL
  bool advance_fenc = false;
  long real_size = 0;
  iconv_t iconv_fd = (iconv_t)-1;       // descriptor for iconv() or -1
  bool did_iconv = false;               // true when iconv() failed and trying
                                        // 'charconvert' next
  bool converted = false;                // true if conversion done
  bool notconverted = false;             // true if conversion wanted but it wasn't possible
  char conv_rest[CONV_RESTLEN];
  int conv_restlen = 0;                 // nr of bytes in conv_rest[]
  pos_T orig_start;
  buf_T *old_curbuf;
  char *old_b_ffname;
  char *old_b_fname;
  int using_b_ffname;
  int using_b_fname;
  static char *msg_is_a_directory = N_("is a directory");

  au_did_filetype = false;  // reset before triggering any autocommands

  curbuf->b_no_eol_lnum = 0;    // in case it was set by the previous read

  // If there is no file name yet, use the one for the read file.
  // BF_NOTEDITED is set to reflect this.
  // Don't do this for a read from a filter.
  // Only do this when 'cpoptions' contains the 'f' flag.
  if (curbuf->b_ffname == NULL
      && !filtering
      && fname != NULL
      && vim_strchr(p_cpo, CPO_FNAMER) != NULL
      && !(flags & READ_DUMMY)) {
    if (set_rw_fname(fname, sfname) == FAIL) {
      return FAIL;
    }
  }

  // Remember the initial values of curbuf, curbuf->b_ffname and
  // curbuf->b_fname to detect whether they are altered as a result of
  // executing nasty autocommands.  Also check if "fname" and "sfname"
  // point to one of these values.
  old_curbuf = curbuf;
  old_b_ffname = curbuf->b_ffname;
  old_b_fname = curbuf->b_fname;
  using_b_ffname = (fname == curbuf->b_ffname) || (sfname == curbuf->b_ffname);
  using_b_fname = (fname == curbuf->b_fname) || (sfname == curbuf->b_fname);

  // After reading a file the cursor line changes but we don't want to
  // display the line.
  ex_no_reprint = true;

  // don't display the file info for another buffer now
  need_fileinfo = false;

  // For Unix: Use the short file name whenever possible.
  // Avoids problems with networks and when directory names are changed.
  // Don't do this for Windows, a "cd" in a sub-shell may have moved us to
  // another directory, which we don't detect.
  if (sfname == NULL) {
    sfname = fname;
  }
#if defined(UNIX)
  fname = sfname;
#endif

  // The BufReadCmd and FileReadCmd events intercept the reading process by
  // executing the associated commands instead.
  if (!filtering && !read_stdin && !read_buffer) {
    orig_start = curbuf->b_op_start;

    // Set '[ mark to the line above where the lines go (line 1 if zero).
    curbuf->b_op_start.lnum = ((from == 0) ? 1 : from);
    curbuf->b_op_start.col = 0;

    if (newfile) {
      if (apply_autocmds_exarg(EVENT_BUFREADCMD, NULL, sfname,
                               false, curbuf, eap)) {
        int status = OK;

        if (aborting()) {
          status = FAIL;
        }

        // The BufReadCmd code usually uses ":read" to get the text and
        // perhaps ":file" to change the buffer name. But we should
        // consider this to work like ":edit", thus reset the
        // BF_NOTEDITED flag.  Then ":write" will work to overwrite the
        // same file.
        if (status == OK) {
          curbuf->b_flags &= ~BF_NOTEDITED;
        }
        return status;
      }
    } else if (apply_autocmds_exarg(EVENT_FILEREADCMD, sfname, sfname,
                                    false, NULL, eap)) {
      return aborting() ? FAIL : OK;
    }

    curbuf->b_op_start = orig_start;

    if (flags & READ_NOFILE) {
      // Return NOTDONE instead of FAIL so that BufEnter can be triggered
      // and other operations don't fail.
      return NOTDONE;
    }
  }

  if ((shortmess(SHM_OVER) || curbuf->b_help) && p_verbose == 0) {
    msg_scroll = false;         // overwrite previous file message
  } else {
    msg_scroll = true;          // don't overwrite previous file message
  }
  // If the name is too long we might crash further on, quit here.
  if (fname != NULL && *fname != NUL) {
    size_t namelen = strlen(fname);

    // If the name is too long we might crash further on, quit here.
    if (namelen >= MAXPATHL) {
      filemess(curbuf, fname, _("Illegal file name"), 0);
      msg_end();
      msg_scroll = msg_save;
      return FAIL;
    }

    // If the name ends in a path separator, we can't open it.  Check here,
    // because reading the file may actually work, but then creating the
    // swap file may destroy it!  Reported on MS-DOS and Win 95.
    if (after_pathsep(fname, fname + namelen)) {
      if (!silent) {
        filemess(curbuf, fname, _(msg_is_a_directory), 0);
      }
      msg_end();
      msg_scroll = msg_save;
      return NOTDONE;
    }
  }

  if (!read_buffer && !read_stdin && !read_fifo) {
    perm = os_getperm(fname);
    // On Unix it is possible to read a directory, so we have to
    // check for it before os_open().

#ifdef OPEN_CHR_FILES
# define IS_CHR_DEV(perm, fname) S_ISCHR(perm) && is_dev_fd_file(fname)
#else
# define IS_CHR_DEV(perm, fname) false
#endif

    if (perm >= 0 && !S_ISREG(perm)                 // not a regular file ...
        && !S_ISFIFO(perm)                          // ... or fifo
        && !S_ISSOCK(perm)                          // ... or socket
        && !(IS_CHR_DEV(perm, fname))
        // ... or a character special file named /dev/fd/<n>
        ) {
      if (S_ISDIR(perm)) {
        if (!silent) {
          filemess(curbuf, fname, _(msg_is_a_directory), 0);
        }
      } else {
        filemess(curbuf, fname, _("is not a file"), 0);
      }
      msg_end();
      msg_scroll = msg_save;
      return S_ISDIR(perm) ? NOTDONE : FAIL;
    }
  }

  // Set default or forced 'fileformat' and 'binary'.
  set_file_options(set_options, eap);

  // When opening a new file we take the readonly flag from the file.
  // Default is r/w, can be set to r/o below.
  // Don't reset it when in readonly mode
  // Only set/reset b_p_ro when BF_CHECK_RO is set.
  check_readonly = (newfile && (curbuf->b_flags & BF_CHECK_RO));
  if (check_readonly && !readonlymode) {
    curbuf->b_p_ro = false;
  }

  if (newfile && !read_stdin && !read_buffer && !read_fifo) {
    // Remember time of file.
    if (os_fileinfo(fname, &file_info)) {
      buf_store_file_info(curbuf, &file_info);
      curbuf->b_mtime_read = curbuf->b_mtime;
      curbuf->b_mtime_read_ns = curbuf->b_mtime_ns;
#ifdef UNIX
      // Use the protection bits of the original file for the swap file.
      // This makes it possible for others to read the name of the
      // edited file from the swapfile, but only if they can read the
      // edited file.
      // Remove the "write" and "execute" bits for group and others
      // (they must not write the swapfile).
      // Add the "read" and "write" bits for the user, otherwise we may
      // not be able to write to the file ourselves.
      // Setting the bits is done below, after creating the swap file.
      swap_mode = ((int)file_info.stat.st_mode & 0644) | 0600;
#endif
    } else {
      curbuf->b_mtime = 0;
      curbuf->b_mtime_ns = 0;
      curbuf->b_mtime_read = 0;
      curbuf->b_mtime_read_ns = 0;
      curbuf->b_orig_size = 0;
      curbuf->b_orig_mode = 0;
    }

    // Reset the "new file" flag.  It will be set again below when the
    // file doesn't exist.
    curbuf->b_flags &= ~(BF_NEW | BF_NEW_W);
  }

  // Check readonly.
  bool file_readonly = false;
  if (!read_buffer && !read_stdin) {
    if (!newfile || readonlymode || !(perm & 0222)
        || !os_file_is_writable(fname)) {
      file_readonly = true;
    }
    fd = os_open(fname, O_RDONLY, 0);
  }

  if (fd < 0) {                     // cannot open at all
    msg_scroll = msg_save;
    if (!newfile) {
      return FAIL;
    }
    if (perm == UV_ENOENT) {  // check if the file exists
      // Set the 'new-file' flag, so that when the file has
      // been created by someone else, a ":w" will complain.
      curbuf->b_flags |= BF_NEW;

      // Create a swap file now, so that other Vims are warned
      // that we are editing this file.  Don't do this for a
      // "nofile" or "nowrite" buffer type.
      if (!bt_dontwrite(curbuf)) {
        check_need_swap(newfile);
        // SwapExists autocommand may mess things up
        if (curbuf != old_curbuf
            || (using_b_ffname
                && (old_b_ffname != curbuf->b_ffname))
            || (using_b_fname
                && (old_b_fname != curbuf->b_fname))) {
          emsg(_(e_auchangedbuf));
          return FAIL;
        }
      }
      if (!silent) {
        if (dir_of_file_exists(fname)) {
          filemess(curbuf, sfname, new_file_message(), 0);
        } else {
          filemess(curbuf, sfname, _("[New DIRECTORY]"), 0);
        }
      }
      // Even though this is a new file, it might have been
      // edited before and deleted.  Get the old marks.
      check_marks_read();
      // Set forced 'fileencoding'.
      if (eap != NULL) {
        set_forced_fenc(eap);
      }
      apply_autocmds_exarg(EVENT_BUFNEWFILE, sfname, sfname,
                           false, curbuf, eap);
      // remember the current fileformat
      save_file_ff(curbuf);

      if (aborting()) {             // autocmds may abort script processing
        return FAIL;
      }
      return OK;                  // a new file is not an error
    }
#if defined(UNIX) && defined(EOVERFLOW)
    filemess(curbuf, sfname, ((fd == UV_EFBIG) ? _("[File too big]") :
                              // libuv only returns -errno
                              // in Unix and in Windows
                              // open() does not set
                              // EOVERFLOW
                              (fd == -EOVERFLOW) ? _("[File too big]") :
                              _("[Permission Denied]")), 0);
#else
    filemess(curbuf, sfname, ((fd == UV_EFBIG) ? _("[File too big]") :
                              _("[Permission Denied]")), 0);
#endif
    curbuf->b_p_ro = true;                  // must use "w!" now

    return FAIL;
  }

  // Only set the 'ro' flag for readonly files the first time they are
  // loaded.    Help files always get readonly mode
  if ((check_readonly && file_readonly) || curbuf->b_help) {
    curbuf->b_p_ro = true;
  }

  if (set_options) {
    // Don't change 'eol' if reading from buffer as it will already be
    // correctly set when reading stdin.
    if (!read_buffer) {
      curbuf->b_p_eof = false;
      curbuf->b_start_eof = false;
      curbuf->b_p_eol = true;
      curbuf->b_start_eol = true;
    }
    curbuf->b_p_bomb = false;
    curbuf->b_start_bomb = false;
  }

  // Create a swap file now, so that other Vims are warned that we are
  // editing this file.
  // Don't do this for a "nofile" or "nowrite" buffer type.
  if (!bt_dontwrite(curbuf)) {
    check_need_swap(newfile);
    if (!read_stdin
        && (curbuf != old_curbuf
            || (using_b_ffname && (old_b_ffname != curbuf->b_ffname))
            || (using_b_fname && (old_b_fname != curbuf->b_fname)))) {
      emsg(_(e_auchangedbuf));
      if (!read_buffer) {
        close(fd);
      }
      return FAIL;
    }
#ifdef UNIX
    // Set swap file protection bits after creating it.
    if (swap_mode > 0 && curbuf->b_ml.ml_mfp != NULL
        && curbuf->b_ml.ml_mfp->mf_fname != NULL) {
      const char *swap_fname = (const char *)curbuf->b_ml.ml_mfp->mf_fname;

      // If the group-read bit is set but not the world-read bit, then
      // the group must be equal to the group of the original file.  If
      // we can't make that happen then reset the group-read bit.  This
      // avoids making the swap file readable to more users when the
      // primary group of the user is too permissive.
      if ((swap_mode & 044) == 040) {
        FileInfo swap_info;

        if (os_fileinfo(swap_fname, &swap_info)
            && file_info.stat.st_gid != swap_info.stat.st_gid
            && os_fchown(curbuf->b_ml.ml_mfp->mf_fd, (uv_uid_t)(-1),
                         (uv_gid_t)file_info.stat.st_gid)
            == -1) {
          swap_mode &= 0600;
        }
      }

      (void)os_setperm(swap_fname, swap_mode);
    }
#endif
  }

  // If "Quit" selected at ATTENTION dialog, don't load the file.
  if (swap_exists_action == SEA_QUIT) {
    if (!read_buffer && !read_stdin) {
      close(fd);
    }
    return FAIL;
  }

  no_wait_return++;         // don't wait for return yet

  // Set '[ mark to the line above where the lines go (line 1 if zero).
  orig_start = curbuf->b_op_start;
  curbuf->b_op_start.lnum = ((from == 0) ? 1 : from);
  curbuf->b_op_start.col = 0;

  int try_mac = (vim_strchr(p_ffs, 'm') != NULL);
  int try_dos = (vim_strchr(p_ffs, 'd') != NULL);
  int try_unix = (vim_strchr(p_ffs, 'x') != NULL);

  if (!read_buffer) {
    int m = msg_scroll;
    int n = msg_scrolled;

    // The file must be closed again, the autocommands may want to change
    // the file before reading it.
    if (!read_stdin) {
      close(fd);                // ignore errors
    }

    // The output from the autocommands should not overwrite anything and
    // should not be overwritten: Set msg_scroll, restore its value if no
    // output was done.
    msg_scroll = true;
    if (filtering) {
      apply_autocmds_exarg(EVENT_FILTERREADPRE, NULL, sfname,
                           false, curbuf, eap);
    } else if (read_stdin) {
      apply_autocmds_exarg(EVENT_STDINREADPRE, NULL, sfname,
                           false, curbuf, eap);
    } else if (newfile) {
      apply_autocmds_exarg(EVENT_BUFREADPRE, NULL, sfname,
                           false, curbuf, eap);
    } else {
      apply_autocmds_exarg(EVENT_FILEREADPRE, sfname, sfname,
                           false, NULL, eap);
    }

    // autocommands may have changed it
    try_mac = (vim_strchr(p_ffs, 'm') != NULL);
    try_dos = (vim_strchr(p_ffs, 'd') != NULL);
    try_unix = (vim_strchr(p_ffs, 'x') != NULL);
    curbuf->b_op_start = orig_start;

    if (msg_scrolled == n) {
      msg_scroll = m;
    }

    if (aborting()) {       // autocmds may abort script processing
      no_wait_return--;
      msg_scroll = msg_save;
      curbuf->b_p_ro = true;            // must use "w!" now
      return FAIL;
    }
    // Don't allow the autocommands to change the current buffer.
    // Try to re-open the file.
    //
    // Don't allow the autocommands to change the buffer name either
    // (cd for example) if it invalidates fname or sfname.
    if (!read_stdin && (curbuf != old_curbuf
                        || (using_b_ffname && (old_b_ffname != curbuf->b_ffname))
                        || (using_b_fname && (old_b_fname != curbuf->b_fname))
                        || (fd = os_open(fname, O_RDONLY, 0)) < 0)) {
      no_wait_return--;
      msg_scroll = msg_save;
      if (fd < 0) {
        emsg(_("E200: *ReadPre autocommands made the file unreadable"));
      } else {
        emsg(_("E201: *ReadPre autocommands must not change current buffer"));
      }
      curbuf->b_p_ro = true;            // must use "w!" now
      return FAIL;
    }
  }

  // Autocommands may add lines to the file, need to check if it is empty
  wasempty = (curbuf->b_ml.ml_flags & ML_EMPTY);

  if (!recoverymode && !filtering && !(flags & READ_DUMMY) && !silent) {
    if (!read_stdin && !read_buffer) {
      filemess(curbuf, sfname, "", 0);
    }
  }

  msg_scroll = false;                   // overwrite the file message

  // Set linecnt now, before the "retry" caused by a wrong guess for
  // fileformat, and after the autocommands, which may change them.
  linecnt = curbuf->b_ml.ml_line_count;

  // "++bad=" argument.
  if (eap != NULL && eap->bad_char != 0) {
    bad_char_behavior = eap->bad_char;
    if (set_options) {
      curbuf->b_bad_char = eap->bad_char;
    }
  } else {
    curbuf->b_bad_char = 0;
  }

  // Decide which 'encoding' to use or use first.
  if (eap != NULL && eap->force_enc != 0) {
    fenc = enc_canonize(eap->cmd + eap->force_enc);
    fenc_alloced = true;
    keep_dest_enc = true;
  } else if (curbuf->b_p_bin) {
    fenc = "";                // binary: don't convert
    fenc_alloced = false;
  } else if (curbuf->b_help) {
    // Help files are either utf-8 or latin1.  Try utf-8 first, if this
    // fails it must be latin1.
    // It is needed when the first line contains non-ASCII characters.
    // That is only in *.??x files.
    fenc_next = "latin1";
    fenc = "utf-8";

    fenc_alloced = false;
  } else if (*p_fencs == NUL) {
    fenc = curbuf->b_p_fenc;            // use format from buffer
    fenc_alloced = false;
  } else {
    fenc_next = p_fencs;                // try items in 'fileencodings'
    fenc = next_fenc(&fenc_next, &fenc_alloced);
  }

  // Jump back here to retry reading the file in different ways.
  // Reasons to retry:
  // - encoding conversion failed: try another one from "fenc_next"
  // - BOM detected and fenc was set, need to setup conversion
  // - "fileformat" check failed: try another
  //
  // Variables set for special retry actions:
  // "file_rewind"      Rewind the file to start reading it again.
  // "advance_fenc"     Advance "fenc" using "fenc_next".
  // "skip_read"        Re-use already read bytes (BOM detected).
  // "did_iconv"        iconv() conversion failed, try 'charconvert'.
  // "keep_fileformat" Don't reset "fileformat".
  //
  // Other status indicators:
  // "tmpname"  When != NULL did conversion with 'charconvert'.
  //                    Output file has to be deleted afterwards.
  // "iconv_fd" When != -1 did conversion with iconv().
retry:

  if (file_rewind) {
    if (read_buffer) {
      read_buf_lnum = 1;
      read_buf_col = 0;
    } else if (read_stdin || vim_lseek(fd, (off_T)0L, SEEK_SET) != 0) {
      // Can't rewind the file, give up.
      error = true;
      goto failed;
    }
    // Delete the previously read lines.
    while (lnum > from) {
      ml_delete(lnum--, false);
    }
    file_rewind = false;
    if (set_options) {
      curbuf->b_p_bomb = false;
      curbuf->b_start_bomb = false;
    }
    conv_error = 0;
  }

  // When retrying with another "fenc" and the first time "fileformat"
  // will be reset.
  if (keep_fileformat) {
    keep_fileformat = false;
  } else {
    if (eap != NULL && eap->force_ff != 0) {
      fileformat = get_fileformat_force(curbuf, eap);
      try_unix = try_dos = try_mac = false;
    } else if (curbuf->b_p_bin) {
      fileformat = EOL_UNIX;                    // binary: use Unix format
    } else if (*p_ffs ==
               NUL) {
      fileformat = get_fileformat(curbuf);      // use format from buffer
    } else {
      fileformat = EOL_UNKNOWN;                 // detect from file
    }
  }

  if (iconv_fd != (iconv_t)-1) {
    // aborted conversion with iconv(), close the descriptor
    iconv_close(iconv_fd);
    iconv_fd = (iconv_t)-1;
  }

  if (advance_fenc) {
    // Try the next entry in 'fileencodings'.
    advance_fenc = false;

    if (eap != NULL && eap->force_enc != 0) {
      // Conversion given with "++cc=" wasn't possible, read
      // without conversion.
      notconverted = true;
      conv_error = 0;
      if (fenc_alloced) {
        xfree(fenc);
      }
      fenc = "";
      fenc_alloced = false;
    } else {
      if (fenc_alloced) {
        xfree(fenc);
      }
      if (fenc_next != NULL) {
        fenc = next_fenc(&fenc_next, &fenc_alloced);
      } else {
        fenc = "";
        fenc_alloced = false;
      }
    }
    if (tmpname != NULL) {
      os_remove(tmpname);  // delete converted file
      XFREE_CLEAR(tmpname);
    }
  }

  // Conversion may be required when the encoding of the file is different
  // from 'encoding' or 'encoding' is UTF-16, UCS-2 or UCS-4.
  fio_flags = 0;
  converted = need_conversion(fenc);
  if (converted) {
    // "ucs-bom" means we need to check the first bytes of the file
    // for a BOM.
    if (strcmp(fenc, ENC_UCSBOM) == 0) {
      fio_flags = FIO_UCSBOM;
    } else {
      // Check if UCS-2/4 or Latin1 to UTF-8 conversion needs to be
      // done.  This is handled below after read().  Prepare the
      // fio_flags to avoid having to parse the string each time.
      // Also check for Unicode to Latin1 conversion, because iconv()
      // appears not to handle this correctly.  This works just like
      // conversion to UTF-8 except how the resulting character is put in
      // the buffer.
      fio_flags = get_fio_flags(fenc);
    }

    // Try using iconv() if we can't convert internally.
    if (fio_flags == 0
        && !did_iconv) {
      iconv_fd = (iconv_t)my_iconv_open("utf-8", fenc);
    }

    // Use the 'charconvert' expression when conversion is required
    // and we can't do it internally or with iconv().
    if (fio_flags == 0 && !read_stdin && !read_buffer && *p_ccv != NUL
        && !read_fifo && iconv_fd == (iconv_t)-1) {
      did_iconv = false;
      // Skip conversion when it's already done (retry for wrong
      // "fileformat").
      if (tmpname == NULL) {
        tmpname = readfile_charconvert(fname, fenc, &fd);
        if (tmpname == NULL) {
          // Conversion failed.  Try another one.
          advance_fenc = true;
          if (fd < 0) {
            // Re-opening the original file failed!
            emsg(_("E202: Conversion made file unreadable!"));
            error = true;
            goto failed;
          }
          goto retry;
        }
      }
    } else {
      if (fio_flags == 0 && iconv_fd == (iconv_t)-1) {
        // Conversion wanted but we can't.
        // Try the next conversion in 'fileencodings'
        advance_fenc = true;
        goto retry;
      }
    }
  }

  // Set "can_retry" when it's possible to rewind the file and try with
  // another "fenc" value.  It's false when no other "fenc" to try, reading
  // stdin or fixed at a specific encoding.
  can_retry = (*fenc != NUL && !read_stdin && !keep_dest_enc && !read_fifo);

  if (!skip_read) {
    linerest = 0;
    filesize = 0;
    skip_count = lines_to_skip;
    read_count = lines_to_read;
    conv_restlen = 0;
    read_undo_file = (newfile && (flags & READ_KEEP_UNDO) == 0
                      && curbuf->b_ffname != NULL
                      && curbuf->b_p_udf
                      && !filtering
                      && !read_fifo
                      && !read_stdin
                      && !read_buffer);
    if (read_undo_file) {
      sha256_start(&sha_ctx);
    }
  }

  while (!error && !got_int) {
    // We allocate as much space for the file as we can get, plus
    // space for the old line plus room for one terminating NUL.
    // The amount is limited by the fact that read() only can read
    // up to max_unsigned characters (and other things).
    {
      if (!skip_read) {
        // Use buffer >= 64K.  Add linerest to double the size if the
        // line gets very long, to avoid a lot of copying. But don't
        // read more than 1 Mbyte at a time, so we can be interrupted.
        size = 0x10000L + linerest;
        if (size > 0x100000L) {
          size = 0x100000L;
        }
      }

      // Protect against the argument of lalloc() going negative.
      if (size < 0 || size + linerest + 1 < 0 || linerest >= MAXCOL) {
        split++;
        *ptr = NL;  // split line by inserting a NL
        size = 1;
      } else if (!skip_read) {
        for (; size >= 10; size /= 2) {
          new_buffer = verbose_try_malloc((size_t)size + (size_t)linerest + 1);
          if (new_buffer) {
            break;
          }
        }
        if (new_buffer == NULL) {
          error = true;
          break;
        }
        if (linerest) {         // copy characters from the previous buffer
          memmove(new_buffer, ptr - linerest, (size_t)linerest);
        }
        xfree(buffer);
        buffer = new_buffer;
        ptr = buffer + linerest;
        line_start = buffer;

        // May need room to translate into.
        // For iconv() we don't really know the required space, use a
        // factor ICONV_MULT.
        // latin1 to utf-8: 1 byte becomes up to 2 bytes
        // utf-16 to utf-8: 2 bytes become up to 3 bytes, 4 bytes
        // become up to 4 bytes, size must be multiple of 2
        // ucs-2 to utf-8: 2 bytes become up to 3 bytes, size must be
        // multiple of 2
        // ucs-4 to utf-8: 4 bytes become up to 6 bytes, size must be
        // multiple of 4
        real_size = (int)size;
        if (iconv_fd != (iconv_t)-1) {
          size = size / ICONV_MULT;
        } else if (fio_flags & FIO_LATIN1) {
          size = size / 2;
        } else if (fio_flags & (FIO_UCS2 | FIO_UTF16)) {
          size = (size * 2 / 3) & ~1;
        } else if (fio_flags & FIO_UCS4) {
          size = (size * 2 / 3) & ~3;
        } else if (fio_flags == FIO_UCSBOM) {
          size = size / ICONV_MULT;  // worst case
        }

        if (conv_restlen > 0) {
          // Insert unconverted bytes from previous line.
          memmove(ptr, conv_rest, (size_t)conv_restlen);  // -V614
          ptr += conv_restlen;
          size -= conv_restlen;
        }

        if (read_buffer) {
          // Read bytes from curbuf.  Used for converting text read
          // from stdin.
          if (read_buf_lnum > from) {
            size = 0;
          } else {
            int ni;
            long tlen = 0;
            for (;;) {
              p = (uint8_t *)ml_get(read_buf_lnum) + read_buf_col;
              int n = (int)strlen((char *)p);
              if ((int)tlen + n + 1 > size) {
                // Filled up to "size", append partial line.
                // Change NL to NUL to reverse the effect done
                // below.
                n = (int)(size - tlen);
                for (ni = 0; ni < n; ni++) {
                  if (p[ni] == NL) {
                    ptr[tlen++] = NUL;
                  } else {
                    ptr[tlen++] = (char)p[ni];
                  }
                }
                read_buf_col += n;
                break;
              }

              // Append whole line and new-line.  Change NL
              // to NUL to reverse the effect done below.
              for (ni = 0; ni < n; ni++) {
                if (p[ni] == NL) {
                  ptr[tlen++] = NUL;
                } else {
                  ptr[tlen++] = (char)p[ni];
                }
              }
              ptr[tlen++] = NL;
              read_buf_col = 0;
              if (++read_buf_lnum > from) {
                // When the last line didn't have an
                // end-of-line don't add it now either.
                if (!curbuf->b_p_eol) {
                  tlen--;
                }
                size = tlen;
                break;
              }
            }
          }
        } else {
          // Read bytes from the file.
          size = read_eintr(fd, ptr, (size_t)size);
        }

        if (size <= 0) {
          if (size < 0) {                           // read error
            error = true;
          } else if (conv_restlen > 0) {
            // Reached end-of-file but some trailing bytes could
            // not be converted.  Truncated file?

            // When we did a conversion report an error.
            if (fio_flags != 0 || iconv_fd != (iconv_t)-1) {
              if (can_retry) {
                goto rewind_retry;
              }
              if (conv_error == 0) {
                conv_error = curbuf->b_ml.ml_line_count
                             - linecnt + 1;
              }
            } else if (illegal_byte == 0) {
              // Remember the first linenr with an illegal byte
              illegal_byte = curbuf->b_ml.ml_line_count
                             - linecnt + 1;
            }
            if (bad_char_behavior == BAD_DROP) {
              *(ptr - conv_restlen) = NUL;
              conv_restlen = 0;
            } else {
              // Replace the trailing bytes with the replacement
              // character if we were converting; if we weren't,
              // leave the UTF8 checking code to do it, as it
              // works slightly differently.
              if (bad_char_behavior != BAD_KEEP && (fio_flags != 0 || iconv_fd != (iconv_t)-1)) {
                while (conv_restlen > 0) {
                  *(--ptr) = (char)bad_char_behavior;
                  conv_restlen--;
                }
              }
              fio_flags = 0;  // don't convert this
              if (iconv_fd != (iconv_t)-1) {
                iconv_close(iconv_fd);
                iconv_fd = (iconv_t)-1;
              }
            }
          }
        }
      }

      skip_read = false;

      // At start of file: Check for BOM.
      // Also check for a BOM for other Unicode encodings, but not after
      // converting with 'charconvert' or when a BOM has already been
      // found.
      if ((filesize == 0)
          && (fio_flags == FIO_UCSBOM
              || (!curbuf->b_p_bomb
                  && tmpname == NULL
                  && (*fenc == 'u' || *fenc == NUL)))) {
        char *ccname;
        int blen = 0;

        // no BOM detection in a short file or in binary mode
        if (size < 2 || curbuf->b_p_bin) {
          ccname = NULL;
        } else {
          ccname = check_for_bom(ptr, (int)size, &blen,
                                 fio_flags == FIO_UCSBOM ? FIO_ALL : get_fio_flags(fenc));
        }
        if (ccname != NULL) {
          // Remove BOM from the text
          filesize += blen;
          size -= blen;
          memmove(ptr, ptr + blen, (size_t)size);
          if (set_options) {
            curbuf->b_p_bomb = true;
            curbuf->b_start_bomb = true;
          }
        }

        if (fio_flags == FIO_UCSBOM) {
          if (ccname == NULL) {
            // No BOM detected: retry with next encoding.
            advance_fenc = true;
          } else {
            // BOM detected: set "fenc" and jump back
            if (fenc_alloced) {
              xfree(fenc);
            }
            fenc = ccname;
            fenc_alloced = false;
          }
          // retry reading without getting new bytes or rewinding
          skip_read = true;
          goto retry;
        }
      }

      // Include not converted bytes.
      ptr -= conv_restlen;
      size += conv_restlen;
      conv_restlen = 0;
      // Break here for a read error or end-of-file.
      if (size <= 0) {
        break;
      }

      if (iconv_fd != (iconv_t)-1) {
        // Attempt conversion of the read bytes to 'encoding' using iconv().
        const char *fromp = ptr;
        size_t from_size = (size_t)size;
        ptr += size;
        char *top = ptr;
        size_t to_size = (size_t)(real_size - size);

        // If there is conversion error or not enough room try using
        // another conversion.  Except for when there is no
        // alternative (help files).
        while ((iconv(iconv_fd, (void *)&fromp, &from_size,
                      &top, &to_size)
                == (size_t)-1 && ICONV_ERRNO != ICONV_EINVAL)
               || from_size > CONV_RESTLEN) {
          if (can_retry) {
            goto rewind_retry;
          }
          if (conv_error == 0) {
            conv_error = readfile_linenr(linecnt, ptr, top);
          }

          // Deal with a bad byte and continue with the next.
          fromp++;
          from_size--;
          if (bad_char_behavior == BAD_KEEP) {
            *top++ = *(fromp - 1);
            to_size--;
          } else if (bad_char_behavior != BAD_DROP) {
            *top++ = (char)bad_char_behavior;
            to_size--;
          }
        }

        if (from_size > 0) {
          // Some remaining characters, keep them for the next
          // round.
          memmove(conv_rest, fromp, from_size);
          conv_restlen = (int)from_size;
        }

        // move the linerest to before the converted characters
        line_start = ptr - linerest;
        memmove(line_start, buffer, (size_t)linerest);
        size = (top - ptr);
      }

      if (fio_flags != 0) {
        unsigned int u8c;
        char *dest;
        char *tail = NULL;

        // Convert Unicode or Latin1 to UTF-8.
        // Go from end to start through the buffer, because the number
        // of bytes may increase.
        // "dest" points to after where the UTF-8 bytes go, "p" points
        // to after the next character to convert.
        dest = ptr + real_size;
        if (fio_flags == FIO_LATIN1 || fio_flags == FIO_UTF8) {
          p = (uint8_t *)ptr + size;
          if (fio_flags == FIO_UTF8) {
            // Check for a trailing incomplete UTF-8 sequence
            tail = ptr + size - 1;
            while (tail > ptr && (*tail & 0xc0) == 0x80) {
              tail--;
            }
            if (tail + utf_byte2len(*tail) <= ptr + size) {
              tail = NULL;
            } else {
              p = (uint8_t *)tail;
            }
          }
        } else if (fio_flags & (FIO_UCS2 | FIO_UTF16)) {
          // Check for a trailing byte
          p = (uint8_t *)ptr + (size & ~1);
          if (size & 1) {
            tail = (char *)p;
          }
          if ((fio_flags & FIO_UTF16) && p > (uint8_t *)ptr) {
            // Check for a trailing leading word
            if (fio_flags & FIO_ENDIAN_L) {
              u8c = (unsigned)(*--p) << 8;
              u8c += *--p;
            } else {
              u8c = *--p;
              u8c += (unsigned)(*--p) << 8;
            }
            if (u8c >= 0xd800 && u8c <= 0xdbff) {
              tail = (char *)p;
            } else {
              p += 2;
            }
          }
        } else {   //  FIO_UCS4
                   // Check for trailing 1, 2 or 3 bytes
          p = (uint8_t *)ptr + (size & ~3);
          if (size & 3) {
            tail = (char *)p;
          }
        }

        // If there is a trailing incomplete sequence move it to
        // conv_rest[].
        if (tail != NULL) {
          conv_restlen = (int)((ptr + size) - tail);
          memmove(conv_rest, tail, (size_t)conv_restlen);
          size -= conv_restlen;
        }

        while (p > (uint8_t *)ptr) {
          if (fio_flags & FIO_LATIN1) {
            u8c = *--p;
          } else if (fio_flags & (FIO_UCS2 | FIO_UTF16)) {
            if (fio_flags & FIO_ENDIAN_L) {
              u8c = (unsigned)(*--p) << 8;
              u8c += *--p;
            } else {
              u8c = *--p;
              u8c += (unsigned)(*--p) << 8;
            }
            if ((fio_flags & FIO_UTF16)
                && u8c >= 0xdc00 && u8c <= 0xdfff) {
              int u16c;

              if (p == (uint8_t *)ptr) {
                // Missing leading word.
                if (can_retry) {
                  goto rewind_retry;
                }
                if (conv_error == 0) {
                  conv_error = readfile_linenr(linecnt, ptr, (char *)p);
                }
                if (bad_char_behavior == BAD_DROP) {
                  continue;
                }
                if (bad_char_behavior != BAD_KEEP) {
                  u8c = (unsigned)bad_char_behavior;
                }
              }

              // found second word of double-word, get the first
              // word and compute the resulting character
              if (fio_flags & FIO_ENDIAN_L) {
                u16c = (*--p << 8);
                u16c += *--p;
              } else {
                u16c = *--p;
                u16c += (*--p << 8);
              }
              u8c = 0x10000 + (((unsigned)u16c & 0x3ff) << 10)
                    + (u8c & 0x3ff);

              // Check if the word is indeed a leading word.
              if (u16c < 0xd800 || u16c > 0xdbff) {
                if (can_retry) {
                  goto rewind_retry;
                }
                if (conv_error == 0) {
                  conv_error = readfile_linenr(linecnt, ptr, (char *)p);
                }
                if (bad_char_behavior == BAD_DROP) {
                  continue;
                }
                if (bad_char_behavior != BAD_KEEP) {
                  u8c = (unsigned)bad_char_behavior;
                }
              }
            }
          } else if (fio_flags & FIO_UCS4) {
            if (fio_flags & FIO_ENDIAN_L) {
              u8c = (unsigned)(*--p) << 24;
              u8c += (unsigned)(*--p) << 16;
              u8c += (unsigned)(*--p) << 8;
              u8c += *--p;
            } else {          // big endian
              u8c = *--p;
              u8c += (unsigned)(*--p) << 8;
              u8c += (unsigned)(*--p) << 16;
              u8c += (unsigned)(*--p) << 24;
            }
            // Replace characters over INT_MAX with Unicode replacement character
            if (u8c > INT_MAX) {
              u8c = 0xfffd;
            }
          } else {        // UTF-8
            if (*--p < 0x80) {
              u8c = *p;
            } else {
              len = utf_head_off(ptr, (char *)p);
              p -= len;
              u8c = (unsigned)utf_ptr2char((char *)p);
              if (len == 0) {
                // Not a valid UTF-8 character, retry with
                // another fenc when possible, otherwise just
                // report the error.
                if (can_retry) {
                  goto rewind_retry;
                }
                if (conv_error == 0) {
                  conv_error = readfile_linenr(linecnt, ptr, (char *)p);
                }
                if (bad_char_behavior == BAD_DROP) {
                  continue;
                }
                if (bad_char_behavior != BAD_KEEP) {
                  u8c = (unsigned)bad_char_behavior;
                }
              }
            }
          }
          assert(u8c <= INT_MAX);
          // produce UTF-8
          dest -= utf_char2len((int)u8c);
          (void)utf_char2bytes((int)u8c, dest);
        }

        // move the linerest to before the converted characters
        line_start = dest - linerest;
        memmove(line_start, buffer, (size_t)linerest);
        size = ((ptr + real_size) - dest);
        ptr = dest;
      } else if (!curbuf->b_p_bin) {
        bool incomplete_tail = false;

        // Reading UTF-8: Check if the bytes are valid UTF-8.
        for (p = (uint8_t *)ptr;; p++) {
          int todo = (int)(((uint8_t *)ptr + size) - p);
          int l;

          if (todo <= 0) {
            break;
          }
          if (*p >= 0x80) {
            // A length of 1 means it's an illegal byte.  Accept
            // an incomplete character at the end though, the next
            // read() will get the next bytes, we'll check it
            // then.
            l = utf_ptr2len_len((char *)p, todo);
            if (l > todo && !incomplete_tail) {
              // Avoid retrying with a different encoding when
              // a truncated file is more likely, or attempting
              // to read the rest of an incomplete sequence when
              // we have already done so.
              if (p > (uint8_t *)ptr || filesize > 0) {
                incomplete_tail = true;
              }
              // Incomplete byte sequence, move it to conv_rest[]
              // and try to read the rest of it, unless we've
              // already done so.
              if (p > (uint8_t *)ptr) {
                conv_restlen = todo;
                memmove(conv_rest, p, (size_t)conv_restlen);
                size -= conv_restlen;
                break;
              }
            }
            if (l == 1 || l > todo) {
              // Illegal byte.  If we can try another encoding
              // do that, unless at EOF where a truncated
              // file is more likely than a conversion error.
              if (can_retry && !incomplete_tail) {
                break;
              }

              // When we did a conversion report an error.
              if (iconv_fd != (iconv_t)-1 && conv_error == 0) {
                conv_error = readfile_linenr(linecnt, ptr, (char *)p);
              }

              // Remember the first linenr with an illegal byte
              if (conv_error == 0 && illegal_byte == 0) {
                illegal_byte = readfile_linenr(linecnt, ptr, (char *)p);
              }

              // Drop, keep or replace the bad byte.
              if (bad_char_behavior == BAD_DROP) {
                memmove(p, p + 1, (size_t)(todo - 1));
                p--;
                size--;
              } else if (bad_char_behavior != BAD_KEEP) {
                *p = (uint8_t)bad_char_behavior;
              }
            } else {
              p += l - 1;
            }
          }
        }
        if (p < (uint8_t *)ptr + size && !incomplete_tail) {
          // Detected a UTF-8 error.
rewind_retry:
          // Retry reading with another conversion.
          if (*p_ccv != NUL && iconv_fd != (iconv_t)-1) {
            // iconv() failed, try 'charconvert'
            did_iconv = true;
          } else {
            // use next item from 'fileencodings'
            advance_fenc = true;
          }
          file_rewind = true;
          goto retry;
        }
      }

      // count the number of characters (after conversion!)
      filesize += size;

      // when reading the first part of a file: guess EOL type
      if (fileformat == EOL_UNKNOWN) {
        // First try finding a NL, for Dos and Unix
        if (try_dos || try_unix) {
          // Reset the carriage return counter.
          if (try_mac) {
            try_mac = 1;
          }

          for (p = (uint8_t *)ptr; p < (uint8_t *)ptr + size; p++) {
            if (*p == NL) {
              if (!try_unix
                  || (try_dos && p > (uint8_t *)ptr && p[-1] == CAR)) {
                fileformat = EOL_DOS;
              } else {
                fileformat = EOL_UNIX;
              }
              break;
            } else if (*p == CAR && try_mac) {
              try_mac++;
            }
          }

          // Don't give in to EOL_UNIX if EOL_MAC is more likely
          if (fileformat == EOL_UNIX && try_mac) {
            // Need to reset the counters when retrying fenc.
            try_mac = 1;
            try_unix = 1;
            for (; p >= (uint8_t *)ptr && *p != CAR; p--) {}
            if (p >= (uint8_t *)ptr) {
              for (p = (uint8_t *)ptr; p < (uint8_t *)ptr + size; p++) {
                if (*p == NL) {
                  try_unix++;
                } else if (*p == CAR) {
                  try_mac++;
                }
              }
              if (try_mac > try_unix) {
                fileformat = EOL_MAC;
              }
            }
          } else if (fileformat == EOL_UNKNOWN && try_mac == 1) {
            // Looking for CR but found no end-of-line markers at all:
            // use the default format.
            fileformat = default_fileformat();
          }
        }

        // No NL found: may use Mac format
        if (fileformat == EOL_UNKNOWN && try_mac) {
          fileformat = EOL_MAC;
        }

        // Still nothing found?  Use first format in 'ffs'
        if (fileformat == EOL_UNKNOWN) {
          fileformat = default_fileformat();
        }

        // May set 'p_ff' if editing a new file.
        if (set_options) {
          set_fileformat(fileformat, OPT_LOCAL);
        }
      }
    }

    // This loop is executed once for every character read.
    // Keep it fast!
    if (fileformat == EOL_MAC) {
      ptr--;
      while (++ptr, --size >= 0) {
        // catch most common case first
        if ((c = *ptr) != NUL && c != CAR && c != NL) {
          continue;
        }
        if (c == NUL) {
          *ptr = NL;            // NULs are replaced by newlines!
        } else if (c == NL) {
          *ptr = CAR;           // NLs are replaced by CRs!
        } else {
          if (skip_count == 0) {
            *ptr = NUL;                     // end of line
            len = (colnr_T)(ptr - line_start + 1);
            if (ml_append(lnum, line_start, len, newfile) == FAIL) {
              error = true;
              break;
            }
            if (read_undo_file) {
              sha256_update(&sha_ctx, (uint8_t *)line_start, (size_t)len);
            }
            lnum++;
            if (--read_count == 0) {
              error = true;                     // break loop
              line_start = ptr;                 // nothing left to write
              break;
            }
          } else {
            skip_count--;
          }
          line_start = ptr + 1;
        }
      }
    } else {
      ptr--;
      while (++ptr, --size >= 0) {
        if ((c = *ptr) != NUL && c != NL) {        // catch most common case
          continue;
        }
        if (c == NUL) {
          *ptr = NL;            // NULs are replaced by newlines!
        } else {
          if (skip_count == 0) {
            *ptr = NUL;                         // end of line
            len = (colnr_T)(ptr - line_start + 1);
            if (fileformat == EOL_DOS) {
              if (ptr > line_start && ptr[-1] == CAR) {
                // remove CR before NL
                ptr[-1] = NUL;
                len--;
              } else if (ff_error != EOL_DOS) {
                // Reading in Dos format, but no CR-LF found!
                // When 'fileformats' includes "unix", delete all
                // the lines read so far and start all over again.
                // Otherwise give an error message later.
                if (try_unix
                    && !read_stdin
                    && (read_buffer
                        || vim_lseek(fd, (off_T)0L, SEEK_SET) == 0)) {
                  fileformat = EOL_UNIX;
                  if (set_options) {
                    set_fileformat(EOL_UNIX, OPT_LOCAL);
                  }
                  file_rewind = true;
                  keep_fileformat = true;
                  goto retry;
                }
                ff_error = EOL_DOS;
              }
            }
            if (ml_append(lnum, line_start, len, newfile) == FAIL) {
              error = true;
              break;
            }
            if (read_undo_file) {
              sha256_update(&sha_ctx, (uint8_t *)line_start, (size_t)len);
            }
            lnum++;
            if (--read_count == 0) {
              error = true;                         // break loop
              line_start = ptr;                 // nothing left to write
              break;
            }
          } else {
            skip_count--;
          }
          line_start = ptr + 1;
        }
      }
    }
    linerest = (ptr - line_start);
    os_breakcheck();
  }

failed:
  // not an error, max. number of lines reached
  if (error && read_count == 0) {
    error = false;
  }

  // In Dos format ignore a trailing CTRL-Z, unless 'binary' is set.
  // In old days the file length was in sector count and the CTRL-Z the
  // marker where the file really ended.  Assuming we write it to a file
  // system that keeps file length properly the CTRL-Z should be dropped.
  // Set the 'endoffile' option so the user can decide what to write later.
  // In Unix format the CTRL-Z is just another character.
  if (linerest != 0
      && !curbuf->b_p_bin
      && fileformat == EOL_DOS
      && ptr[-1] == Ctrl_Z) {
    ptr--;
    linerest--;
    if (set_options) {
      curbuf->b_p_eof = true;
    }
  }

  // If we get EOF in the middle of a line, note the fact and
  // complete the line ourselves.
  if (!error
      && !got_int
      && linerest != 0) {
    // remember for when writing
    if (set_options) {
      curbuf->b_p_eol = false;
    }
    *ptr = NUL;
    len = (colnr_T)(ptr - line_start + 1);
    if (ml_append(lnum, line_start, len, newfile) == FAIL) {
      error = true;
    } else {
      if (read_undo_file) {
        sha256_update(&sha_ctx, (uint8_t *)line_start, (size_t)len);
      }
      read_no_eol_lnum = ++lnum;
    }
  }

  if (set_options) {
    // Remember the current file format.
    save_file_ff(curbuf);
    // If editing a new file: set 'fenc' for the current buffer.
    // Also for ":read ++edit file".
    set_string_option_direct("fenc", -1, fenc, OPT_FREE | OPT_LOCAL, 0);
  }
  if (fenc_alloced) {
    xfree(fenc);
  }
  if (iconv_fd != (iconv_t)-1) {
    iconv_close(iconv_fd);
  }

  if (!read_buffer && !read_stdin) {
    close(fd);  // errors are ignored
  } else {
    (void)os_set_cloexec(fd);
  }
  xfree(buffer);

  if (read_stdin) {
    close(fd);
    if (stdin_fd < 0) {
#ifndef MSWIN
      // On Unix, use stderr for stdin, makes shell commands work.
      vim_ignored = dup(2);
#else
      // On Windows, use the console input handle for stdin.
      HANDLE conin = CreateFile("CONIN$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, (LPSECURITY_ATTRIBUTES)NULL,
                                OPEN_EXISTING, 0, (HANDLE)NULL);
      vim_ignored = _open_osfhandle((intptr_t)conin, _O_RDONLY);
#endif
    }
  }

  if (tmpname != NULL) {
    os_remove(tmpname);  // delete converted file
    xfree(tmpname);
  }
  no_wait_return--;                     // may wait for return now

  // In recovery mode everything but autocommands is skipped.
  if (!recoverymode) {
    // need to delete the last line, which comes from the empty buffer
    if (newfile && wasempty && !(curbuf->b_ml.ml_flags & ML_EMPTY)) {
      ml_delete(curbuf->b_ml.ml_line_count, false);
      linecnt--;
    }
    curbuf->deleted_bytes = 0;
    curbuf->deleted_bytes2 = 0;
    curbuf->deleted_codepoints = 0;
    curbuf->deleted_codeunits = 0;
    linecnt = curbuf->b_ml.ml_line_count - linecnt;
    if (filesize == 0) {
      linecnt = 0;
    }
    if (newfile || read_buffer) {
      redraw_curbuf_later(UPD_NOT_VALID);
      // After reading the text into the buffer the diff info needs to
      // be updated.
      diff_invalidate(curbuf);
      // All folds in the window are invalid now.  Mark them for update
      // before triggering autocommands.
      foldUpdateAll(curwin);
    } else if (linecnt) {               // appended at least one line
      appended_lines_mark(from, linecnt);
    }

    if (got_int) {
      if (!(flags & READ_DUMMY)) {
        filemess(curbuf, sfname, _(e_interr), 0);
        if (newfile) {
          curbuf->b_p_ro = true;                // must use "w!" now
        }
      }
      msg_scroll = msg_save;
      check_marks_read();
      return OK;                // an interrupt isn't really an error
    }

    if (!filtering && !(flags & READ_DUMMY) && !silent) {
      add_quoted_fname(IObuff, IOSIZE, curbuf, (const char *)sfname);
      c = false;

#ifdef UNIX
      if (S_ISFIFO(perm)) {             // fifo
        STRCAT(IObuff, _("[fifo]"));
        c = true;
      }
      if (S_ISSOCK(perm)) {            // or socket
        STRCAT(IObuff, _("[socket]"));
        c = true;
      }
# ifdef OPEN_CHR_FILES
      if (S_ISCHR(perm)) {                          // or character special
        STRCAT(IObuff, _("[character special]"));
        c = true;
      }
# endif
#endif
      if (curbuf->b_p_ro) {
        STRCAT(IObuff, shortmess(SHM_RO) ? _("[RO]") : _("[readonly]"));
        c = true;
      }
      if (read_no_eol_lnum) {
        msg_add_eol();
        c = true;
      }
      if (ff_error == EOL_DOS) {
        STRCAT(IObuff, _("[CR missing]"));
        c = true;
      }
      if (split) {
        STRCAT(IObuff, _("[long lines split]"));
        c = true;
      }
      if (notconverted) {
        STRCAT(IObuff, _("[NOT converted]"));
        c = true;
      } else if (converted) {
        STRCAT(IObuff, _("[converted]"));
        c = true;
      }
      if (conv_error != 0) {
        snprintf(IObuff + strlen(IObuff), IOSIZE - strlen(IObuff),
                 _("[CONVERSION ERROR in line %" PRId64 "]"), (int64_t)conv_error);
        c = true;
      } else if (illegal_byte > 0) {
        snprintf(IObuff + strlen(IObuff), IOSIZE - strlen(IObuff),
                 _("[ILLEGAL BYTE in line %" PRId64 "]"), (int64_t)illegal_byte);
        c = true;
      } else if (error) {
        STRCAT(IObuff, _("[READ ERRORS]"));
        c = true;
      }
      if (msg_add_fileformat(fileformat)) {
        c = true;
      }

      msg_add_lines(c, (long)linecnt, filesize);

      XFREE_CLEAR(keep_msg);
      p = NULL;
      msg_scrolled_ign = true;

      if (!read_stdin && !read_buffer) {
        p = (uint8_t *)msg_trunc_attr(IObuff, false, 0);
      }

      if (read_stdin || read_buffer || restart_edit != 0
          || (msg_scrolled != 0 && !need_wait_return)) {
        // Need to repeat the message after redrawing when:
        // - When reading from stdin (the screen will be cleared next).
        // - When restart_edit is set (otherwise there will be a delay before
        //   redrawing).
        // - When the screen was scrolled but there is no wait-return prompt.
        set_keep_msg((char *)p, 0);
      }
      msg_scrolled_ign = false;
    }

    // with errors writing the file requires ":w!"
    if (newfile && (error
                    || conv_error != 0
                    || (illegal_byte > 0 && bad_char_behavior != BAD_KEEP))) {
      curbuf->b_p_ro = true;
    }

    u_clearline();          // cannot use "U" command after adding lines

    // In Ex mode: cursor at last new line.
    // Otherwise: cursor at first new line.
    if (exmode_active) {
      curwin->w_cursor.lnum = from + linecnt;
    } else {
      curwin->w_cursor.lnum = from + 1;
    }
    check_cursor_lnum();
    beginline(BL_WHITE | BL_FIX);           // on first non-blank

    if ((cmdmod.cmod_flags & CMOD_LOCKMARKS) == 0) {
      // Set '[ and '] marks to the newly read lines.
      curbuf->b_op_start.lnum = from + 1;
      curbuf->b_op_start.col = 0;
      curbuf->b_op_end.lnum = from + linecnt;
      curbuf->b_op_end.col = 0;
    }
  }
  msg_scroll = msg_save;

  // Get the marks before executing autocommands, so they can be used there.
  check_marks_read();

  // We remember if the last line of the read didn't have
  // an eol even when 'binary' is off, to support turning 'fixeol' off,
  // or writing the read again with 'binary' on.  The latter is required
  // for ":autocmd FileReadPost *.gz set bin|'[,']!gunzip" to work.
  curbuf->b_no_eol_lnum = read_no_eol_lnum;

  // When reloading a buffer put the cursor at the first line that is
  // different.
  if (flags & READ_KEEP_UNDO) {
    u_find_first_changed();
  }

  // When opening a new file locate undo info and read it.
  if (read_undo_file) {
    uint8_t hash[UNDO_HASH_SIZE];

    sha256_finish(&sha_ctx, hash);
    u_read_undo(NULL, hash, fname);
  }

  if (!read_stdin && !read_fifo && (!read_buffer || sfname != NULL)) {
    int m = msg_scroll;
    int n = msg_scrolled;

    // Save the fileformat now, otherwise the buffer will be considered
    // modified if the format/encoding was automatically detected.
    if (set_options) {
      save_file_ff(curbuf);
    }

    // The output from the autocommands should not overwrite anything and
    // should not be overwritten: Set msg_scroll, restore its value if no
    // output was done.
    msg_scroll = true;
    if (filtering) {
      apply_autocmds_exarg(EVENT_FILTERREADPOST, NULL, sfname,
                           false, curbuf, eap);
    } else if (newfile || (read_buffer && sfname != NULL)) {
      apply_autocmds_exarg(EVENT_BUFREADPOST, NULL, sfname,
                           false, curbuf, eap);
      if (!au_did_filetype && *curbuf->b_p_ft != NUL) {
        // EVENT_FILETYPE was not triggered but the buffer already has a
        // filetype.  Trigger EVENT_FILETYPE using the existing filetype.
        apply_autocmds(EVENT_FILETYPE, curbuf->b_p_ft, curbuf->b_fname, true, curbuf);
      }
    } else {
      apply_autocmds_exarg(EVENT_FILEREADPOST, sfname, sfname,
                           false, NULL, eap);
    }
    if (msg_scrolled == n) {
      msg_scroll = m;
    }
    if (aborting()) {       // autocmds may abort script processing
      return FAIL;
    }
  }

  if (recoverymode && error) {
    return FAIL;
  }
  return OK;
}

#ifdef OPEN_CHR_FILES
/// Returns true if the file name argument is of the form "/dev/fd/\d\+",
/// which is the name of files used for process substitution output by
/// some shells on some operating systems, e.g., bash on SunOS.
/// Do not accept "/dev/fd/[012]", opening these may hang Vim.
///
/// @param fname file name to check
bool is_dev_fd_file(char *fname)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT
{
  return strncmp(fname, "/dev/fd/", 8) == 0
         && ascii_isdigit((uint8_t)fname[8])
         && *skipdigits(fname + 9) == NUL
         && (fname[9] != NUL
             || (fname[8] != '0' && fname[8] != '1' && fname[8] != '2'));
}
#endif

/// From the current line count and characters read after that, estimate the
/// line number where we are now.
/// Used for error messages that include a line number.
///
/// @param linecnt  line count before reading more bytes
/// @param p        start of more bytes read
/// @param endp     end of more bytes read
static linenr_T readfile_linenr(linenr_T linecnt, char *p, const char *endp)
{
  char *s;
  linenr_T lnum;

  lnum = curbuf->b_ml.ml_line_count - linecnt + 1;
  for (s = p; s < endp; s++) {
    if (*s == '\n') {
      lnum++;
    }
  }
  return lnum;
}

/// Fill "*eap" to force the 'fileencoding', 'fileformat' and 'binary' to be
/// equal to the buffer "buf".  Used for calling readfile().
void prep_exarg(exarg_T *eap, const buf_T *buf)
  FUNC_ATTR_NONNULL_ALL
{
  const size_t cmd_len = 15 + strlen(buf->b_p_fenc);
  eap->cmd = xmalloc(cmd_len);

  snprintf(eap->cmd, cmd_len, "e ++enc=%s", buf->b_p_fenc);
  eap->force_enc = 8;
  eap->bad_char = buf->b_bad_char;
  eap->force_ff = (unsigned char)(*buf->b_p_ff);

  eap->force_bin = buf->b_p_bin ? FORCE_BIN : FORCE_NOBIN;
  eap->read_edit = false;
  eap->forceit = false;
}

/// Set default or forced 'fileformat' and 'binary'.
void set_file_options(int set_options, exarg_T *eap)
{
  // set default 'fileformat'
  if (set_options) {
    if (eap != NULL && eap->force_ff != 0) {
      set_fileformat(get_fileformat_force(curbuf, eap), OPT_LOCAL);
    } else if (*p_ffs != NUL) {
      set_fileformat(default_fileformat(), OPT_LOCAL);
    }
  }

  // set or reset 'binary'
  if (eap != NULL && eap->force_bin != 0) {
    int oldval = curbuf->b_p_bin;

    curbuf->b_p_bin = (eap->force_bin == FORCE_BIN);
    set_options_bin(oldval, curbuf->b_p_bin, OPT_LOCAL);
  }
}

/// Set forced 'fileencoding'.
void set_forced_fenc(exarg_T *eap)
{
  if (eap->force_enc == 0) {
    return;
  }

  char *fenc = enc_canonize(eap->cmd + eap->force_enc);
  set_string_option_direct("fenc", -1, fenc, OPT_FREE|OPT_LOCAL, 0);
  xfree(fenc);
}

/// Find next fileencoding to use from 'fileencodings'.
/// "pp" points to fenc_next.  It's advanced to the next item.
/// When there are no more items, an empty string is returned and *pp is set to
/// NULL.
/// When *pp is not set to NULL, the result is in allocated memory and "alloced"
/// is set to true.
static char *next_fenc(char **pp, bool *alloced)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_NONNULL_RET
{
  char *p;
  char *r;

  *alloced = false;
  if (**pp == NUL) {
    *pp = NULL;
    return "";
  }
  p = vim_strchr(*pp, ',');
  if (p == NULL) {
    r = enc_canonize(*pp);
    *pp += strlen(*pp);
  } else {
    r = xstrnsave(*pp, (size_t)(p - *pp));
    *pp = p + 1;
    p = enc_canonize(r);
    xfree(r);
    r = p;
  }
  *alloced = true;
  return r;
}

/// Convert a file with the 'charconvert' expression.
/// This closes the file which is to be read, converts it and opens the
/// resulting file for reading.
///
/// @param fname  name of input file
/// @param fenc   converted from
/// @param fdp    in/out: file descriptor of file
///
/// @return       name of the resulting converted file (the caller should delete it after reading it).
///               Returns NULL if the conversion failed ("*fdp" is not set) .
static char *readfile_charconvert(char *fname, char *fenc, int *fdp)
{
  char *tmpname;
  char *errmsg = NULL;

  tmpname = vim_tempname();
  if (tmpname == NULL) {
    errmsg = _("Can't find temp file for conversion");
  } else {
    close(*fdp);                // close the input file, ignore errors
    *fdp = -1;
    if (eval_charconvert(fenc, "utf-8",
                         fname, tmpname) == FAIL) {
      errmsg = _("Conversion with 'charconvert' failed");
    }
    if (errmsg == NULL && (*fdp = os_open(tmpname, O_RDONLY, 0)) < 0) {
      errmsg = _("can't read output of 'charconvert'");
    }
  }

  if (errmsg != NULL) {
    // Don't use emsg(), it breaks mappings, the retry with
    // another type of conversion might still work.
    msg(errmsg);
    if (tmpname != NULL) {
      os_remove(tmpname);  // delete converted file
      XFREE_CLEAR(tmpname);
    }
  }

  // If the input file is closed, open it (caller should check for error).
  if (*fdp < 0) {
    *fdp = os_open(fname, O_RDONLY, 0);
  }

  return tmpname;
}

/// Read marks for the current buffer from the ShaDa file, when we support
/// buffer marks and the buffer has a name.
static void check_marks_read(void)
{
  if (!curbuf->b_marks_read && get_shada_parameter('\'') > 0
      && curbuf->b_ffname != NULL) {
    shada_read_marks();
  }

  // Always set b_marks_read; needed when 'shada' is changed to include
  // the ' parameter after opening a buffer.
  curbuf->b_marks_read = true;
}

char *new_file_message(void)
{
  return shortmess(SHM_NEW) ? _("[New]") : _("[New File]");
}

static int buf_write_do_autocmds(buf_T *buf, char **fnamep, char **sfnamep, char **ffnamep,
                                 linenr_T start, linenr_T *endp, exarg_T *eap, bool append,
                                 bool filtering, bool reset_changed, bool overwriting, bool whole,
                                 const pos_T orig_start, const pos_T orig_end)
{
  linenr_T old_line_count = buf->b_ml.ml_line_count;
  int msg_save = msg_scroll;

  aco_save_T aco;
  bool did_cmd = false;
  bool nofile_err = false;
  bool empty_memline = buf->b_ml.ml_mfp == NULL;
  bufref_T bufref;

  char *sfname = *sfnamep;

  // Apply PRE autocommands.
  // Set curbuf to the buffer to be written.
  // Careful: The autocommands may call buf_write() recursively!
  bool buf_ffname = *ffnamep == buf->b_ffname;
  bool buf_sfname = sfname == buf->b_sfname;
  bool buf_fname_f = *fnamep == buf->b_ffname;
  bool buf_fname_s = *fnamep == buf->b_sfname;

  // Set curwin/curbuf to buf and save a few things.
  aucmd_prepbuf(&aco, buf);
  set_bufref(&bufref, buf);

  if (append) {
    did_cmd = apply_autocmds_exarg(EVENT_FILEAPPENDCMD, sfname, sfname, false, curbuf, eap);
    if (!did_cmd) {
      if (overwriting && bt_nofilename(curbuf)) {
        nofile_err = true;
      } else {
        apply_autocmds_exarg(EVENT_FILEAPPENDPRE,
                             sfname, sfname, false, curbuf, eap);
      }
    }
  } else if (filtering) {
    apply_autocmds_exarg(EVENT_FILTERWRITEPRE,
                         NULL, sfname, false, curbuf, eap);
  } else if (reset_changed && whole) {
    bool was_changed = curbufIsChanged();

    did_cmd = apply_autocmds_exarg(EVENT_BUFWRITECMD, sfname, sfname, false, curbuf, eap);
    if (did_cmd) {
      if (was_changed && !curbufIsChanged()) {
        // Written everything correctly and BufWriteCmd has reset
        // 'modified': Correct the undo information so that an
        // undo now sets 'modified'.
        u_unchanged(curbuf);
        u_update_save_nr(curbuf);
      }
    } else {
      if (overwriting && bt_nofilename(curbuf)) {
        nofile_err = true;
      } else {
        apply_autocmds_exarg(EVENT_BUFWRITEPRE,
                             sfname, sfname, false, curbuf, eap);
      }
    }
  } else {
    did_cmd = apply_autocmds_exarg(EVENT_FILEWRITECMD, sfname, sfname, false, curbuf, eap);
    if (!did_cmd) {
      if (overwriting && bt_nofilename(curbuf)) {
        nofile_err = true;
      } else {
        apply_autocmds_exarg(EVENT_FILEWRITEPRE,
                             sfname, sfname, false, curbuf, eap);
      }
    }
  }

  // restore curwin/curbuf and a few other things
  aucmd_restbuf(&aco);

  // In three situations we return here and don't write the file:
  // 1. the autocommands deleted or unloaded the buffer.
  // 2. The autocommands abort script processing.
  // 3. If one of the "Cmd" autocommands was executed.
  if (!bufref_valid(&bufref)) {
    buf = NULL;
  }
  if (buf == NULL || (buf->b_ml.ml_mfp == NULL && !empty_memline)
      || did_cmd || nofile_err
      || aborting()) {
    if (buf != NULL && (cmdmod.cmod_flags & CMOD_LOCKMARKS)) {
      // restore the original '[ and '] positions
      buf->b_op_start = orig_start;
      buf->b_op_end = orig_end;
    }

    no_wait_return--;
    msg_scroll = msg_save;
    if (nofile_err) {
      semsg(_(e_no_matching_autocommands_for_buftype_str_buffer), curbuf->b_p_bt);
    }

    if (nofile_err
        || aborting()) {
      // An aborting error, interrupt or exception in the
      // autocommands.
      return FAIL;
    }
    if (did_cmd) {
      if (buf == NULL) {
        // The buffer was deleted.  We assume it was written
        // (can't retry anyway).
        return OK;
      }
      if (overwriting) {
        // Assume the buffer was written, update the timestamp.
        ml_timestamp(buf);
        if (append) {
          buf->b_flags &= ~BF_NEW;
        } else {
          buf->b_flags &= ~BF_WRITE_MASK;
        }
      }
      if (reset_changed && buf->b_changed && !append
          && (overwriting || vim_strchr(p_cpo, CPO_PLUS) != NULL)) {
        // Buffer still changed, the autocommands didn't work properly.
        return FAIL;
      }
      return OK;
    }
    if (!aborting()) {
      emsg(_("E203: Autocommands deleted or unloaded buffer to be written"));
    }
    return FAIL;
  }

  // The autocommands may have changed the number of lines in the file.
  // When writing the whole file, adjust the end.
  // When writing part of the file, assume that the autocommands only
  // changed the number of lines that are to be written (tricky!).
  if (buf->b_ml.ml_line_count != old_line_count) {
    if (whole) {                                              // write all
      *endp = buf->b_ml.ml_line_count;
    } else if (buf->b_ml.ml_line_count > old_line_count) {           // more lines
      *endp += buf->b_ml.ml_line_count - old_line_count;
    } else {                                                    // less lines
      *endp -= old_line_count - buf->b_ml.ml_line_count;
      if (*endp < start) {
        no_wait_return--;
        msg_scroll = msg_save;
        emsg(_("E204: Autocommand changed number of lines in unexpected way"));
        return FAIL;
      }
    }
  }

  // The autocommands may have changed the name of the buffer, which may
  // be kept in fname, ffname and sfname.
  if (buf_ffname) {
    *ffnamep = buf->b_ffname;
  }
  if (buf_sfname) {
    *sfnamep = buf->b_sfname;
  }
  if (buf_fname_f) {
    *fnamep = buf->b_ffname;
  }
  if (buf_fname_s) {
    *fnamep = buf->b_sfname;
  }
  return NOTDONE;
}

static void buf_write_do_post_autocmds(buf_T *buf, char *fname, exarg_T *eap, bool append,
                                       bool filtering, bool reset_changed, bool whole)
{
  aco_save_T aco;

  curbuf->b_no_eol_lnum = 0;      // in case it was set by the previous read

  // Apply POST autocommands.
  // Careful: The autocommands may call buf_write() recursively!
  aucmd_prepbuf(&aco, buf);

  if (append) {
    apply_autocmds_exarg(EVENT_FILEAPPENDPOST, fname, fname,
                         false, curbuf, eap);
  } else if (filtering) {
    apply_autocmds_exarg(EVENT_FILTERWRITEPOST, NULL, fname,
                         false, curbuf, eap);
  } else if (reset_changed && whole) {
    apply_autocmds_exarg(EVENT_BUFWRITEPOST, fname, fname,
                         false, curbuf, eap);
  } else {
    apply_autocmds_exarg(EVENT_FILEWRITEPOST, fname, fname,
                         false, curbuf, eap);
  }

  // restore curwin/curbuf and a few other things
  aucmd_restbuf(&aco);
}

static inline Error_T set_err_num(const char *num, const char *msg)
{
  return (Error_T){ .num = num, .msg = (char *)msg, .arg = 0 };
}

static inline Error_T set_err_arg(const char *msg, int arg)
{
  return (Error_T){ .num = NULL, .msg = (char *)msg, .arg = arg };
}

static inline Error_T set_err(const char *msg)
{
  return (Error_T){ .num = NULL, .msg = (char *)msg, .arg = 0 };
}

static void emit_err(Error_T *e)
{
  if (e->num != NULL) {
    if (e->arg != 0) {
      semsg("%s: %s%s: %s", e->num, IObuff, e->msg, os_strerror(e->arg));
    } else {
      semsg("%s: %s%s", e->num, IObuff, e->msg);
    }
  } else if (e->arg != 0) {
    semsg(e->msg, os_strerror(e->arg));
  } else {
    emsg(e->msg);
  }
  if (e->alloc) {
    xfree(e->msg);
  }
}

#if defined(UNIX)

static int get_fileinfo_os(char *fname, FileInfo *file_info_old, bool overwriting, long *perm,
                           bool *device, bool *newfile, Error_T *err)
{
  *perm = -1;
  if (!os_fileinfo(fname, file_info_old)) {
    *newfile = true;
  } else {
    *perm = (long)file_info_old->stat.st_mode;
    if (!S_ISREG(file_info_old->stat.st_mode)) {             // not a file
      if (S_ISDIR(file_info_old->stat.st_mode)) {
        *err = set_err_num("E502", _("is a directory"));
        return FAIL;
      }
      if (os_nodetype(fname) != NODE_WRITABLE) {
        *err = set_err_num("E503", _("is not a file or writable device"));
        return FAIL;
      }
      // It's a device of some kind (or a fifo) which we can write to
      // but for which we can't make a backup.
      *device = true;
      *newfile = true;
      *perm = -1;
    }
  }
  return OK;
}

#else

static int get_fileinfo_os(char *fname, FileInfo *file_info_old, bool overwriting, long *perm,
                           bool *device, bool *newfile, Error_T *err)
{
  // Check for a writable device name.
  char nodetype = fname == NULL ? NODE_OTHER : (char)os_nodetype(fname);
  if (nodetype == NODE_OTHER) {
    *err = set_err_num("E503", _("is not a file or writable device"));
    return FAIL;
  }
  if (nodetype == NODE_WRITABLE) {
    *device = true;
    *newfile = true;
    *perm = -1;
  } else {
    *perm = os_getperm((const char *)fname);
    if (*perm < 0) {
      *newfile = true;
    } else if (os_isdir(fname)) {
      *err = set_err_num("E502", _("is a directory"));
      return FAIL;
    }
    if (overwriting) {
      os_fileinfo(fname, file_info_old);
    }
  }
  return OK;
}

#endif

/// @param buf
/// @param fname          File name
/// @param overwriting
/// @param forceit
/// @param[out] file_info_old
/// @param[out] perm
/// @param[out] device
/// @param[out] newfile
/// @param[out] readonly
static int get_fileinfo(buf_T *buf, char *fname, bool overwriting, bool forceit,
                        FileInfo *file_info_old, long *perm, bool *device, bool *newfile,
                        bool *readonly, Error_T *err)
{
  if (get_fileinfo_os(fname, file_info_old, overwriting, perm, device, newfile, err) == FAIL) {
    return FAIL;
  }

  *readonly = false;  // overwritten file is read-only

  if (!*device && !*newfile) {
    // Check if the file is really writable (when renaming the file to
    // make a backup we won't discover it later).
    *readonly = !os_file_is_writable(fname);

    if (!forceit && *readonly) {
      if (vim_strchr(p_cpo, CPO_FWRITE) != NULL) {
        *err = set_err_num("E504", _(err_readonly));
      } else {
        *err = set_err_num("E505", _("is read-only (add ! to override)"));
      }
      return FAIL;
    }

    // If 'forceit' is false, check if the timestamp hasn't changed since reading the file.
    if (overwriting && !forceit) {
      int retval = check_mtime(buf, file_info_old);
      if (retval == FAIL) {
        return FAIL;
      }
    }
  }
  return OK;
}

static int buf_write_make_backup(char *fname, bool append, FileInfo *file_info_old, vim_acl_T acl,
                                 long perm, unsigned int bkc, bool file_readonly, bool forceit,
                                 int *backup_copyp, char **backupp, Error_T *err)
{
  FileInfo file_info;
  const bool no_prepend_dot = false;

  if ((bkc & BKC_YES) || append) {       // "yes"
    *backup_copyp = true;
  } else if ((bkc & BKC_AUTO)) {          // "auto"
    // Don't rename the file when:
    // - it's a hard link
    // - it's a symbolic link
    // - we don't have write permission in the directory
    if (os_fileinfo_hardlinks(file_info_old) > 1
        || !os_fileinfo_link(fname, &file_info)
        || !os_fileinfo_id_equal(&file_info, file_info_old)) {
      *backup_copyp = true;
    } else {
      // Check if we can create a file and set the owner/group to
      // the ones from the original file.
      // First find a file name that doesn't exist yet (use some
      // arbitrary numbers).
      STRCPY(IObuff, fname);
      for (int i = 4913;; i += 123) {
        char *tail = path_tail(IObuff);
        size_t size = (size_t)(tail - IObuff);
        snprintf(tail, IOSIZE - size, "%d", i);
        if (!os_fileinfo_link(IObuff, &file_info)) {
          break;
        }
      }
      int fd = os_open(IObuff,
                       O_CREAT|O_WRONLY|O_EXCL|O_NOFOLLOW, (int)perm);
      if (fd < 0) {           // can't write in directory
        *backup_copyp = true;
      } else {
#ifdef UNIX
        os_fchown(fd, (uv_uid_t)file_info_old->stat.st_uid, (uv_gid_t)file_info_old->stat.st_gid);
        if (!os_fileinfo(IObuff, &file_info)
            || file_info.stat.st_uid != file_info_old->stat.st_uid
            || file_info.stat.st_gid != file_info_old->stat.st_gid
            || (long)file_info.stat.st_mode != perm) {
          *backup_copyp = true;
        }
#endif
        // Close the file before removing it, on MS-Windows we
        // can't delete an open file.
        close(fd);
        os_remove(IObuff);
      }
    }
  }

  // Break symlinks and/or hardlinks if we've been asked to.
  if ((bkc & BKC_BREAKSYMLINK) || (bkc & BKC_BREAKHARDLINK)) {
#ifdef UNIX
    bool file_info_link_ok = os_fileinfo_link(fname, &file_info);

    // Symlinks.
    if ((bkc & BKC_BREAKSYMLINK)
        && file_info_link_ok
        && !os_fileinfo_id_equal(&file_info, file_info_old)) {
      *backup_copyp = false;
    }

    // Hardlinks.
    if ((bkc & BKC_BREAKHARDLINK)
        && os_fileinfo_hardlinks(file_info_old) > 1
        && (!file_info_link_ok
            || os_fileinfo_id_equal(&file_info, file_info_old))) {
      *backup_copyp = false;
    }
#endif
  }

  // make sure we have a valid backup extension to use
  char *backup_ext = *p_bex == NUL ? ".bak" : p_bex;

  if (*backup_copyp) {
    int some_error = false;

    // Try to make the backup in each directory in the 'bdir' option.
    //
    // Unix semantics has it, that we may have a writable file,
    // that cannot be recreated with a simple open(..., O_CREAT, ) e.g:
    //  - the directory is not writable,
    //  - the file may be a symbolic link,
    //  - the file may belong to another user/group, etc.
    //
    // For these reasons, the existing writable file must be truncated
    // and reused. Creation of a backup COPY will be attempted.
    char *dirp = p_bdir;
    while (*dirp) {
      // Isolate one directory name, using an entry in 'bdir'.
      size_t dir_len = copy_option_part(&dirp, IObuff, IOSIZE, ",");
      char *p  = IObuff + dir_len;
      bool trailing_pathseps = after_pathsep(IObuff, p) && p[-1] == p[-2];
      if (trailing_pathseps) {
        IObuff[dir_len - 2] = NUL;
      }
      if (*dirp == NUL && !os_isdir(IObuff)) {
        int ret;
        char *failed_dir;
        if ((ret = os_mkdir_recurse(IObuff, 0755, &failed_dir)) != 0) {
          semsg(_("E303: Unable to create directory \"%s\" for backup file: %s"),
                failed_dir, os_strerror(ret));
          xfree(failed_dir);
        }
      }
      if (trailing_pathseps) {
        // Ends with '//', Use Full path
        if ((p = make_percent_swname(IObuff, fname))
            != NULL) {
          *backupp = modname(p, backup_ext, no_prepend_dot);
          xfree(p);
        }
      }

      char *rootname = get_file_in_dir(fname, IObuff);
      if (rootname == NULL) {
        some_error = true;                // out of memory
        goto nobackup;
      }

      FileInfo file_info_new;
      {
        //
        // Make the backup file name.
        //
        if (*backupp == NULL) {
          *backupp = modname(rootname, backup_ext, no_prepend_dot);
        }

        if (*backupp == NULL) {
          xfree(rootname);
          some_error = true;                          // out of memory
          goto nobackup;
        }

        // Check if backup file already exists.
        if (os_fileinfo(*backupp, &file_info_new)) {
          if (os_fileinfo_id_equal(&file_info_new, file_info_old)) {
            //
            // Backup file is same as original file.
            // May happen when modname() gave the same file back (e.g. silly
            // link). If we don't check here, we either ruin the file when
            // copying or erase it after writing.
            //
            XFREE_CLEAR(*backupp);              // no backup file to delete
          } else if (!p_bk) {
            // We are not going to keep the backup file, so don't
            // delete an existing one, and try to use another name instead.
            // Change one character, just before the extension.
            //
            char *wp = *backupp + strlen(*backupp) - 1 - strlen(backup_ext);
            if (wp < *backupp) {                // empty file name ???
              wp = *backupp;
            }
            *wp = 'z';
            while (*wp > 'a' && os_fileinfo(*backupp, &file_info_new)) {
              (*wp)--;
            }
            // They all exist??? Must be something wrong.
            if (*wp == 'a') {
              XFREE_CLEAR(*backupp);
            }
          }
        }
      }
      xfree(rootname);

      // Try to create the backup file
      if (*backupp != NULL) {
        // remove old backup, if present
        os_remove(*backupp);

        // set file protection same as original file, but
        // strip s-bit.
        (void)os_setperm((const char *)(*backupp), perm & 0777);

#ifdef UNIX
        //
        // Try to set the group of the backup same as the original file. If
        // this fails, set the protection bits for the group same as the
        // protection bits for others.
        //
        if (file_info_new.stat.st_gid != file_info_old->stat.st_gid
            && os_chown(*backupp, (uv_uid_t)-1, (uv_gid_t)file_info_old->stat.st_gid) != 0) {
          os_setperm((const char *)(*backupp),
                     ((int)perm & 0707) | (((int)perm & 07) << 3));
        }
#endif

        // copy the file
        if (os_copy(fname, *backupp, UV_FS_COPYFILE_FICLONE) != 0) {
          *err = set_err(_("E509: Cannot create backup file (add ! to override)"));
          XFREE_CLEAR(*backupp);
          *backupp = NULL;
          continue;
        }

#ifdef UNIX
        os_file_settime(*backupp,
                        (double)file_info_old->stat.st_atim.tv_sec,
                        (double)file_info_old->stat.st_mtim.tv_sec);
#endif
        os_set_acl(*backupp, acl);
        *err = set_err(NULL);
        break;
      }
    }

nobackup:
    if (*backupp == NULL && err->msg == NULL) {
      *err = set_err(_("E509: Cannot create backup file (add ! to override)"));
    }
    // Ignore errors when forceit is true.
    if ((some_error || err->msg != NULL) && !forceit) {
      return FAIL;
    }
    *err = set_err(NULL);
  } else {
    // Make a backup by renaming the original file.

    // If 'cpoptions' includes the "W" flag, we don't want to
    // overwrite a read-only file.  But rename may be possible
    // anyway, thus we need an extra check here.
    if (file_readonly && vim_strchr(p_cpo, CPO_FWRITE) != NULL) {
      *err = set_err_num("E504", _(err_readonly));
      return FAIL;
    }

    // Form the backup file name - change path/fo.o.h to
    // path/fo.o.h.bak Try all directories in 'backupdir', first one
    // that works is used.
    char *dirp = p_bdir;
    while (*dirp) {
      // Isolate one directory name and make the backup file name.
      size_t dir_len = copy_option_part(&dirp, IObuff, IOSIZE, ",");
      char *p = IObuff + dir_len;
      bool trailing_pathseps = after_pathsep(IObuff, p) && p[-1] == p[-2];
      if (trailing_pathseps) {
        IObuff[dir_len - 2] = NUL;
      }
      if (*dirp == NUL && !os_isdir(IObuff)) {
        int ret;
        char *failed_dir;
        if ((ret = os_mkdir_recurse(IObuff, 0755, &failed_dir)) != 0) {
          semsg(_("E303: Unable to create directory \"%s\" for backup file: %s"),
                failed_dir, os_strerror(ret));
          xfree(failed_dir);
        }
      }
      if (trailing_pathseps) {
        // path ends with '//', use full path
        if ((p = make_percent_swname(IObuff, fname))
            != NULL) {
          *backupp = modname(p, backup_ext, no_prepend_dot);
          xfree(p);
        }
      }

      if (*backupp == NULL) {
        char *rootname = get_file_in_dir(fname, IObuff);
        if (rootname == NULL) {
          *backupp = NULL;
        } else {
          *backupp = modname(rootname, backup_ext, no_prepend_dot);
          xfree(rootname);
        }
      }

      if (*backupp != NULL) {
        // If we are not going to keep the backup file, don't
        // delete an existing one, try to use another name.
        // Change one character, just before the extension.
        if (!p_bk && os_path_exists(*backupp)) {
          p = *backupp + strlen(*backupp) - 1 - strlen(backup_ext);
          if (p < *backupp) {           // empty file name ???
            p = *backupp;
          }
          *p = 'z';
          while (*p > 'a' && os_path_exists(*backupp)) {
            (*p)--;
          }
          // They all exist??? Must be something wrong!
          if (*p == 'a') {
            XFREE_CLEAR(*backupp);
          }
        }
      }
      if (*backupp != NULL) {
        // Delete any existing backup and move the current version
        // to the backup. For safety, we don't remove the backup
        // until the write has finished successfully. And if the
        // 'backup' option is set, leave it around.

        // If the renaming of the original file to the backup file
        // works, quit here.
        ///
        if (vim_rename(fname, *backupp) == 0) {
          break;
        }

        XFREE_CLEAR(*backupp);             // don't do the rename below
      }
    }
    if (*backupp == NULL && !forceit) {
      *err = set_err(_("E510: Can't make backup file (add ! to override)"));
      return FAIL;
    }
  }
  return OK;
}

/// buf_write() - write to file "fname" lines "start" through "end"
///
/// We do our own buffering here because fwrite() is so slow.
///
/// If "forceit" is true, we don't care for errors when attempting backups.
/// In case of an error everything possible is done to restore the original
/// file.  But when "forceit" is true, we risk losing it.
///
/// When "reset_changed" is true and "append" == false and "start" == 1 and
/// "end" == curbuf->b_ml.ml_line_count, reset curbuf->b_changed.
///
/// This function must NOT use NameBuff (because it's called by autowrite()).
///
///
/// @param eap     for forced 'ff' and 'fenc', can be NULL!
/// @param append  append to the file
///
/// @return        FAIL for failure, OK otherwise
int buf_write(buf_T *buf, char *fname, char *sfname, linenr_T start, linenr_T end, exarg_T *eap,
              int append, int forceit, int reset_changed, int filtering)
{
  int retval = OK;
  int msg_save = msg_scroll;
  int prev_got_int = got_int;
  // writing everything
  int whole = (start == 1 && end == buf->b_ml.ml_line_count);
  int write_undo_file = false;
  context_sha256_T sha_ctx;
  unsigned int bkc = get_bkc_value(buf);

  if (fname == NULL || *fname == NUL) {  // safety check
    return FAIL;
  }
  if (buf->b_ml.ml_mfp == NULL) {
    // This can happen during startup when there is a stray "w" in the
    // vimrc file.
    emsg(_(e_emptybuf));
    return FAIL;
  }

  // Disallow writing in secure mode.
  if (check_secure()) {
    return FAIL;
  }

  // Avoid a crash for a long name.
  if (strlen(fname) >= MAXPATHL) {
    emsg(_(e_longname));
    return FAIL;
  }

  // must init bw_conv_buf and bw_iconv_fd before jumping to "fail"
  struct bw_info write_info;            // info for buf_write_bytes()
  write_info.bw_conv_buf = NULL;
  write_info.bw_conv_error = false;
  write_info.bw_conv_error_lnum = 0;
  write_info.bw_restlen = 0;
  write_info.bw_iconv_fd = (iconv_t)-1;

  // After writing a file changedtick changes but we don't want to display
  // the line.
  ex_no_reprint = true;

  // If there is no file name yet, use the one for the written file.
  // BF_NOTEDITED is set to reflect this (in case the write fails).
  // Don't do this when the write is for a filter command.
  // Don't do this when appending.
  // Only do this when 'cpoptions' contains the 'F' flag.
  if (buf->b_ffname == NULL
      && reset_changed
      && whole
      && buf == curbuf
      && !bt_nofilename(buf)
      && !filtering
      && (!append || vim_strchr(p_cpo, CPO_FNAMEAPP) != NULL)
      && vim_strchr(p_cpo, CPO_FNAMEW) != NULL) {
    if (set_rw_fname(fname, sfname) == FAIL) {
      return FAIL;
    }
    buf = curbuf;           // just in case autocmds made "buf" invalid
  }

  if (sfname == NULL) {
    sfname = fname;
  }

  // For Unix: Use the short file name whenever possible.
  // Avoids problems with networks and when directory names are changed.
  // Don't do this for Windows, a "cd" in a sub-shell may have moved us to
  // another directory, which we don't detect.
  char *ffname = fname;                           // remember full fname
#ifdef UNIX
  fname = sfname;
#endif

// true if writing over original
  int overwriting = buf->b_ffname != NULL && path_fnamecmp(ffname, buf->b_ffname) == 0;

  no_wait_return++;                 // don't wait for return yet

  const pos_T orig_start = buf->b_op_start;
  const pos_T orig_end = buf->b_op_end;

  // Set '[ and '] marks to the lines to be written.
  buf->b_op_start.lnum = start;
  buf->b_op_start.col = 0;
  buf->b_op_end.lnum = end;
  buf->b_op_end.col = 0;

  int res = buf_write_do_autocmds(buf, &fname, &sfname, &ffname, start, &end, eap, append,
                                  filtering, reset_changed, overwriting, whole, orig_start,
                                  orig_end);
  if (res != NOTDONE) {
    return res;
  }

  if (cmdmod.cmod_flags & CMOD_LOCKMARKS) {
    // restore the original '[ and '] positions
    buf->b_op_start = orig_start;
    buf->b_op_end = orig_end;
  }

  if (shortmess(SHM_OVER) && !exiting) {
    msg_scroll = false;             // overwrite previous file message
  } else {
    msg_scroll = true;              // don't overwrite previous file message
  }
  if (!filtering) {
    // show that we are busy
#ifndef UNIX
    filemess(buf, sfname, "", 0);
#else
    filemess(buf, fname, "", 0);
#endif
  }
  msg_scroll = false;               // always overwrite the file message now

  char *buffer = verbose_try_malloc(BUFSIZE);
  int bufsize;
  char smallbuf[SMBUFSIZE];
  // can't allocate big buffer, use small one (to be able to write when out of
  // memory)
  if (buffer == NULL) {
    buffer = smallbuf;
    bufsize = SMBUFSIZE;
  } else {
    bufsize = BUFSIZE;
  }

  Error_T err = { 0 };
  long perm;             // file permissions
  bool newfile = false;  // true if file doesn't exist yet
  bool device = false;   // writing to a device
  bool file_readonly = false;  // overwritten file is read-only
  char *backup = NULL;
  char *fenc_tofree = NULL;   // allocated "fenc"

  // Get information about original file (if there is one).
  FileInfo file_info_old;

  vim_acl_T acl = NULL;                 // ACL copied from original file to
                                        // backup or new file

  if (get_fileinfo(buf, fname, overwriting, forceit, &file_info_old, &perm, &device, &newfile,
                   &file_readonly, &err) == FAIL) {
    goto fail;
  }

  // For systems that support ACL: get the ACL from the original file.
  if (!newfile) {
    acl = os_get_acl(fname);
  }

  // If 'backupskip' is not empty, don't make a backup for some files.
  bool dobackup = (p_wb || p_bk || *p_pm != NUL);
  if (dobackup && *p_bsk != NUL && match_file_list(p_bsk, sfname, ffname)) {
    dobackup = false;
  }

  int backup_copy = false;  // copy the original file?

  // Save the value of got_int and reset it.  We don't want a previous
  // interruption cancel writing, only hitting CTRL-C while writing should
  // abort it.
  prev_got_int = got_int;
  got_int = false;

  // Mark the buffer as 'being saved' to prevent changed buffer warnings
  buf->b_saving = true;

  // If we are not appending or filtering, the file exists, and the
  // 'writebackup', 'backup' or 'patchmode' option is set, need a backup.
  // When 'patchmode' is set also make a backup when appending.
  //
  // Do not make any backup, if 'writebackup' and 'backup' are both switched
  // off.  This helps when editing large files on almost-full disks.
  if (!(append && *p_pm == NUL) && !filtering && perm >= 0 && dobackup) {
    if (buf_write_make_backup(fname, append, &file_info_old, acl, perm, bkc, file_readonly, forceit,
                              &backup_copy, &backup, &err) == FAIL) {
      retval = FAIL;
      goto fail;
    }
  }

#if defined(UNIX)
  int made_writable = false;  // 'w' bit has been set

  // When using ":w!" and the file was read-only: make it writable
  if (forceit && perm >= 0 && !(perm & 0200)
      && file_info_old.stat.st_uid == getuid()
      && vim_strchr(p_cpo, CPO_FWRITE) == NULL) {
    perm |= 0200;
    (void)os_setperm((const char *)fname, (int)perm);
    made_writable = true;
  }
#endif

  // When using ":w!" and writing to the current file, 'readonly' makes no
  // sense, reset it, unless 'Z' appears in 'cpoptions'.
  if (forceit && overwriting && vim_strchr(p_cpo, CPO_KEEPRO) == NULL) {
    buf->b_p_ro = false;
    need_maketitle = true;          // set window title later
    status_redraw_all();            // redraw status lines later
  }

  if (end > buf->b_ml.ml_line_count) {
    end = buf->b_ml.ml_line_count;
  }
  if (buf->b_ml.ml_flags & ML_EMPTY) {
    start = end + 1;
  }

  char *wfname = NULL;       // name of file to write to

  // If the original file is being overwritten, there is a small chance that
  // we crash in the middle of writing. Therefore the file is preserved now.
  // This makes all block numbers positive so that recovery does not need
  // the original file.
  // Don't do this if there is a backup file and we are exiting.
  if (reset_changed && !newfile && overwriting
      && !(exiting && backup != NULL)) {
    ml_preserve(buf, false, !!p_fs);
    if (got_int) {
      err = set_err(_(e_interr));
      goto restore_backup;
    }
  }

  // Default: write the file directly.  May write to a temp file for
  // multi-byte conversion.
  wfname = fname;

  char *fenc;  // effective 'fileencoding'

  // Check for forced 'fileencoding' from "++opt=val" argument.
  if (eap != NULL && eap->force_enc != 0) {
    fenc = eap->cmd + eap->force_enc;
    fenc = enc_canonize(fenc);
    fenc_tofree = fenc;
  } else {
    fenc = buf->b_p_fenc;
  }

  // Check if the file needs to be converted.
  int converted = need_conversion(fenc);
  int wb_flags = 0;

  // Check if UTF-8 to UCS-2/4 or Latin1 conversion needs to be done.  Or
  // Latin1 to Unicode conversion.  This is handled in buf_write_bytes().
  // Prepare the flags for it and allocate bw_conv_buf when needed.
  if (converted) {
    wb_flags = get_fio_flags(fenc);
    if (wb_flags & (FIO_UCS2 | FIO_UCS4 | FIO_UTF16 | FIO_UTF8)) {
      // Need to allocate a buffer to translate into.
      if (wb_flags & (FIO_UCS2 | FIO_UTF16 | FIO_UTF8)) {
        write_info.bw_conv_buflen = (size_t)bufsize * 2;
      } else {       // FIO_UCS4
        write_info.bw_conv_buflen = (size_t)bufsize * 4;
      }
      write_info.bw_conv_buf = verbose_try_malloc(write_info.bw_conv_buflen);
      if (!write_info.bw_conv_buf) {
        end = 0;
      }
    }
  }

  if (converted && wb_flags == 0) {
    // Use iconv() conversion when conversion is needed and it's not done
    // internally.
    write_info.bw_iconv_fd = (iconv_t)my_iconv_open(fenc, "utf-8");
    if (write_info.bw_iconv_fd != (iconv_t)-1) {
      // We're going to use iconv(), allocate a buffer to convert in.
      write_info.bw_conv_buflen = (size_t)bufsize * ICONV_MULT;
      write_info.bw_conv_buf = verbose_try_malloc(write_info.bw_conv_buflen);
      if (!write_info.bw_conv_buf) {
        end = 0;
      }
      write_info.bw_first = true;
    } else {
      // When the file needs to be converted with 'charconvert' after
      // writing, write to a temp file instead and let the conversion
      // overwrite the original file.
      if (*p_ccv != NUL) {
        wfname = vim_tempname();
        if (wfname == NULL) {  // Can't write without a tempfile!
          err = set_err(_("E214: Can't find temp file for writing"));
          goto restore_backup;
        }
      }
    }
  }

  int notconverted = false;

  if (converted && wb_flags == 0
      && write_info.bw_iconv_fd == (iconv_t)-1
      && wfname == fname) {
    if (!forceit) {
      err = set_err(_("E213: Cannot convert (add ! to write without conversion)"));
      goto restore_backup;
    }
    notconverted = true;
  }

  int no_eol = false;  // no end-of-line written
  long nchars;
  linenr_T lnum;
  int fileformat;
  int checking_conversion;

  int fd;

  // If conversion is taking place, we may first pretend to write and check
  // for conversion errors.  Then loop again to write for real.
  // When not doing conversion this writes for real right away.
  for (checking_conversion = true;; checking_conversion = false) {
    // There is no need to check conversion when:
    // - there is no conversion
    // - we make a backup file, that can be restored in case of conversion
    // failure.
    if (!converted || dobackup) {
      checking_conversion = false;
    }

    if (checking_conversion) {
      // Make sure we don't write anything.
      fd = -1;
      write_info.bw_fd = fd;
    } else {
      // Open the file "wfname" for writing.
      // We may try to open the file twice: If we can't write to the file
      // and forceit is true we delete the existing file and try to
      // create a new one. If this still fails we may have lost the
      // original file!  (this may happen when the user reached his
      // quotum for number of files).
      // Appending will fail if the file does not exist and forceit is
      // false.
      const int fflags = O_WRONLY | (append
                                     ? (forceit ? (O_APPEND | O_CREAT) : O_APPEND)
                                     : (O_CREAT | O_TRUNC));
      const int mode = perm < 0 ? 0666 : (perm & 0777);

      while ((fd = os_open(wfname, fflags, mode)) < 0) {
        // A forced write will try to create a new file if the old one
        // is still readonly. This may also happen when the directory
        // is read-only. In that case the mch_remove() will fail.
        if (err.msg == NULL) {
#ifdef UNIX
          FileInfo file_info;

          // Don't delete the file when it's a hard or symbolic link.
          if ((!newfile && os_fileinfo_hardlinks(&file_info_old) > 1)
              || (os_fileinfo_link(fname, &file_info)
                  && !os_fileinfo_id_equal(&file_info, &file_info_old))) {
            err = set_err(_("E166: Can't open linked file for writing"));
          } else {
            err = set_err_arg(_("E212: Can't open file for writing: %s"), fd);
            if (forceit && vim_strchr(p_cpo, CPO_FWRITE) == NULL && perm >= 0) {
              // we write to the file, thus it should be marked
              // writable after all
              if (!(perm & 0200)) {
                made_writable = true;
              }
              perm |= 0200;
              if (file_info_old.stat.st_uid != getuid()
                  || file_info_old.stat.st_gid != getgid()) {
                perm &= 0777;
              }
              if (!append) {                    // don't remove when appending
                os_remove(wfname);
              }
              continue;
            }
          }
#else
          err = set_err_arg(_("E212: Can't open file for writing: %s"), fd);
          if (forceit && vim_strchr(p_cpo, CPO_FWRITE) == NULL && perm >= 0) {
            if (!append) {                    // don't remove when appending
              os_remove(wfname);
            }
            continue;
          }
#endif
        }

restore_backup:
        {
          // If we failed to open the file, we don't need a backup. Throw it
          // away.  If we moved or removed the original file try to put the
          // backup in its place.
          if (backup != NULL && wfname == fname) {
            if (backup_copy) {
              // There is a small chance that we removed the original,
              // try to move the copy in its place.
              // This may not work if the vim_rename() fails.
              // In that case we leave the copy around.
              // If file does not exist, put the copy in its place
              if (!os_path_exists(fname)) {
                vim_rename(backup, fname);
              }
              // if original file does exist throw away the copy
              if (os_path_exists(fname)) {
                os_remove(backup);
              }
            } else {
              // try to put the original file back
              vim_rename(backup, fname);
            }
          }

          // if original file no longer exists give an extra warning
          if (!newfile && !os_path_exists(fname)) {
            end = 0;
          }
        }

        if (wfname != fname) {
          xfree(wfname);
        }
        goto fail;
      }
      write_info.bw_fd = fd;
    }
    err = set_err(NULL);

    write_info.bw_buf = buffer;
    nchars = 0;

    // use "++bin", "++nobin" or 'binary'
    int write_bin;
    if (eap != NULL && eap->force_bin != 0) {
      write_bin = (eap->force_bin == FORCE_BIN);
    } else {
      write_bin = buf->b_p_bin;
    }

    // Skip the BOM when appending and the file already existed, the BOM
    // only makes sense at the start of the file.
    if (buf->b_p_bomb && !write_bin && (!append || perm < 0)) {
      write_info.bw_len = make_bom(buffer, fenc);
      if (write_info.bw_len > 0) {
        // don't convert
        write_info.bw_flags = FIO_NOCONVERT | wb_flags;
        if (buf_write_bytes(&write_info) == FAIL) {
          end = 0;
        } else {
          nchars += write_info.bw_len;
        }
      }
    }
    write_info.bw_start_lnum = start;

    write_undo_file = (buf->b_p_udf && overwriting && !append
                       && !filtering && reset_changed && !checking_conversion);
    if (write_undo_file) {
      // Prepare for computing the hash value of the text.
      sha256_start(&sha_ctx);
    }

    write_info.bw_len = bufsize;
    write_info.bw_flags = wb_flags;
    fileformat = get_fileformat_force(buf, eap);
    char *s = buffer;
    int len = 0;
    for (lnum = start; lnum <= end; lnum++) {
      // The next while loop is done once for each character written.
      // Keep it fast!
      char *ptr = ml_get_buf(buf, lnum, false) - 1;
      if (write_undo_file) {
        sha256_update(&sha_ctx, (uint8_t *)ptr + 1, (uint32_t)(strlen(ptr + 1) + 1));
      }
      char c;
      while ((c = *++ptr) != NUL) {
        if (c == NL) {
          *s = NUL;                       // replace newlines with NULs
        } else if (c == CAR && fileformat == EOL_MAC) {
          *s = NL;                        // Mac: replace CRs with NLs
        } else {
          *s = c;
        }
        s++;
        if (++len != bufsize) {
          continue;
        }
        if (buf_write_bytes(&write_info) == FAIL) {
          end = 0;                        // write error: break loop
          break;
        }
        nchars += bufsize;
        s = buffer;
        len = 0;
        write_info.bw_start_lnum = lnum;
      }
      // write failed or last line has no EOL: stop here
      if (end == 0
          || (lnum == end
              && (write_bin || !buf->b_p_fixeol)
              && ((write_bin && lnum == buf->b_no_eol_lnum)
                  || (lnum == buf->b_ml.ml_line_count && !buf->b_p_eol)))) {
        lnum++;                           // written the line, count it
        no_eol = true;
        break;
      }
      if (fileformat == EOL_UNIX) {
        *s++ = NL;
      } else {
        *s++ = CAR;                       // EOL_MAC or EOL_DOS: write CR
        if (fileformat == EOL_DOS) {      // write CR-NL
          if (++len == bufsize) {
            if (buf_write_bytes(&write_info) == FAIL) {
              end = 0;                    // write error: break loop
              break;
            }
            nchars += bufsize;
            s = buffer;
            len = 0;
          }
          *s++ = NL;
        }
      }
      if (++len == bufsize) {
        if (buf_write_bytes(&write_info) == FAIL) {
          end = 0;  // Write error: break loop.
          break;
        }
        nchars += bufsize;
        s = buffer;
        len = 0;

        os_breakcheck();
        if (got_int) {
          end = 0;  // Interrupted, break loop.
          break;
        }
      }
    }
    if (len > 0 && end > 0) {
      write_info.bw_len = len;
      if (buf_write_bytes(&write_info) == FAIL) {
        end = 0;                      // write error
      }
      nchars += len;
    }

    if (!buf->b_p_fixeol && buf->b_p_eof) {
      // write trailing CTRL-Z
      (void)write_eintr(write_info.bw_fd, "\x1a", 1);
    }

    // Stop when writing done or an error was encountered.
    if (!checking_conversion || end == 0) {
      break;
    }

    // If no error happened until now, writing should be ok, so loop to
    // really write the buffer.
  }

  // If we started writing, finish writing. Also when an error was
  // encountered.
  if (!checking_conversion) {
    // On many journalling file systems there is a bug that causes both the
    // original and the backup file to be lost when halting the system right
    // after writing the file.  That's because only the meta-data is
    // journalled.  Syncing the file slows down the system, but assures it has
    // been written to disk and we don't lose it.
    // For a device do try the fsync() but don't complain if it does not work
    // (could be a pipe).
    // If the 'fsync' option is false, don't fsync().  Useful for laptops.
    int error;
    if (p_fs && (error = os_fsync(fd)) != 0 && !device
        // fsync not supported on this storage.
        && error != UV_ENOTSUP) {
      err = set_err_arg(e_fsync, error);
      end = 0;
    }

#ifdef UNIX
    // When creating a new file, set its owner/group to that of the original
    // file.  Get the new device and inode number.
    if (backup != NULL && !backup_copy) {
      // don't change the owner when it's already OK, some systems remove
      // permission or ACL stuff
      FileInfo file_info;
      if (!os_fileinfo(wfname, &file_info)
          || file_info.stat.st_uid != file_info_old.stat.st_uid
          || file_info.stat.st_gid != file_info_old.stat.st_gid) {
        os_fchown(fd, (uv_uid_t)file_info_old.stat.st_uid, (uv_gid_t)file_info_old.stat.st_gid);
        if (perm >= 0) {  // Set permission again, may have changed.
          (void)os_setperm(wfname, (int)perm);
        }
      }
      buf_set_file_id(buf);
    } else if (!buf->file_id_valid) {
      // Set the file_id when creating a new file.
      buf_set_file_id(buf);
    }
#endif

    if ((error = os_close(fd)) != 0) {
      err = set_err_arg(_("E512: Close failed: %s"), error);
      end = 0;
    }

#ifdef UNIX
    if (made_writable) {
      perm &= ~0200;              // reset 'w' bit for security reasons
    }
#endif
    if (perm >= 0) {  // Set perm. of new file same as old file.
      (void)os_setperm((const char *)wfname, (int)perm);
    }
    // Probably need to set the ACL before changing the user (can't set the
    // ACL on a file the user doesn't own).
    if (!backup_copy) {
      os_set_acl(wfname, acl);
    }

    if (wfname != fname) {
      // The file was written to a temp file, now it needs to be converted
      // with 'charconvert' to (overwrite) the output file.
      if (end != 0) {
        if (eval_charconvert("utf-8", fenc, wfname, fname) == FAIL) {
          write_info.bw_conv_error = true;
          end = 0;
        }
      }
      os_remove(wfname);
      xfree(wfname);
    }
  }

  if (end == 0) {
    // Error encountered.
    if (err.msg == NULL) {
      if (write_info.bw_conv_error) {
        if (write_info.bw_conv_error_lnum == 0) {
          err = set_err(_("E513: write error, conversion failed "
                          "(make 'fenc' empty to override)"));
        } else {
          err = set_err(xmalloc(300));
          err.alloc = true;
          vim_snprintf(err.msg, 300,  // NOLINT(runtime/printf)
                       _("E513: write error, conversion failed in line %" PRIdLINENR
                         " (make 'fenc' empty to override)"),
                       write_info.bw_conv_error_lnum);
        }
      } else if (got_int) {
        err = set_err(_(e_interr));
      } else {
        err = set_err(_("E514: write error (file system full?)"));
      }
    }

    // If we have a backup file, try to put it in place of the new file,
    // because the new file is probably corrupt.  This avoids losing the
    // original file when trying to make a backup when writing the file a
    // second time.
    // When "backup_copy" is set we need to copy the backup over the new
    // file.  Otherwise rename the backup file.
    // If this is OK, don't give the extra warning message.
    if (backup != NULL) {
      if (backup_copy) {
        // This may take a while, if we were interrupted let the user
        // know we got the message.
        if (got_int) {
          msg(_(e_interr));
          ui_flush();
        }

        // copy the file.
        if (os_copy(backup, fname, UV_FS_COPYFILE_FICLONE)
            == 0) {
          end = 1;  // success
        }
      } else {
        if (vim_rename(backup, fname) == 0) {
          end = 1;
        }
      }
    }
    goto fail;
  }

  lnum -= start;            // compute number of written lines
  no_wait_return--;         // may wait for return now

#if !defined(UNIX)
  fname = sfname;           // use shortname now, for the messages
#endif
  if (!filtering) {
    add_quoted_fname(IObuff, IOSIZE, buf, (const char *)fname);
    bool insert_space = false;
    if (write_info.bw_conv_error) {
      STRCAT(IObuff, _(" CONVERSION ERROR"));
      insert_space = true;
      if (write_info.bw_conv_error_lnum != 0) {
        vim_snprintf_add(IObuff, IOSIZE, _(" in line %" PRId64 ";"),
                         (int64_t)write_info.bw_conv_error_lnum);
      }
    } else if (notconverted) {
      STRCAT(IObuff, _("[NOT converted]"));
      insert_space = true;
    } else if (converted) {
      STRCAT(IObuff, _("[converted]"));
      insert_space = true;
    }
    if (device) {
      STRCAT(IObuff, _("[Device]"));
      insert_space = true;
    } else if (newfile) {
      STRCAT(IObuff, new_file_message());
      insert_space = true;
    }
    if (no_eol) {
      msg_add_eol();
      insert_space = true;
    }
    // may add [unix/dos/mac]
    if (msg_add_fileformat(fileformat)) {
      insert_space = true;
    }
    msg_add_lines(insert_space, (long)lnum, nchars);       // add line/char count
    if (!shortmess(SHM_WRITE)) {
      if (append) {
        STRCAT(IObuff, shortmess(SHM_WRI) ? _(" [a]") : _(" appended"));
      } else {
        STRCAT(IObuff, shortmess(SHM_WRI) ? _(" [w]") : _(" written"));
      }
    }

    set_keep_msg(msg_trunc_attr(IObuff, false, 0), 0);
  }

  // When written everything correctly: reset 'modified'.  Unless not
  // writing to the original file and '+' is not in 'cpoptions'.
  if (reset_changed && whole && !append
      && !write_info.bw_conv_error
      && (overwriting || vim_strchr(p_cpo, CPO_PLUS) != NULL)) {
    unchanged(buf, true, false);
    const varnumber_T changedtick = buf_get_changedtick(buf);
    if (buf->b_last_changedtick + 1 == changedtick) {
      // b:changedtick may be incremented in unchanged() but that
      // should not trigger a TextChanged event.
      buf->b_last_changedtick = changedtick;
    }
    u_unchanged(buf);
    u_update_save_nr(buf);
  }

  // If written to the current file, update the timestamp of the swap file
  // and reset the BF_WRITE_MASK flags. Also sets buf->b_mtime.
  if (overwriting) {
    ml_timestamp(buf);
    if (append) {
      buf->b_flags &= ~BF_NEW;
    } else {
      buf->b_flags &= ~BF_WRITE_MASK;
    }
  }

  // If we kept a backup until now, and we are in patch mode, then we make
  // the backup file our 'original' file.
  if (*p_pm && dobackup) {
    char *const org = modname(fname, p_pm, false);

    if (backup != NULL) {
      // If the original file does not exist yet
      // the current backup file becomes the original file
      if (org == NULL) {
        emsg(_("E205: Patchmode: can't save original file"));
      } else if (!os_path_exists(org)) {
        vim_rename(backup, org);
        XFREE_CLEAR(backup);                   // don't delete the file
#ifdef UNIX
        os_file_settime(org,
                        (double)file_info_old.stat.st_atim.tv_sec,
                        (double)file_info_old.stat.st_mtim.tv_sec);
#endif
      }
    } else {
      // If there is no backup file, remember that a (new) file was
      // created.
      int empty_fd;

      if (org == NULL
          || (empty_fd = os_open(org,
                                 O_CREAT | O_EXCL | O_NOFOLLOW,
                                 perm < 0 ? 0666 : (perm & 0777))) < 0) {
        emsg(_("E206: patchmode: can't touch empty original file"));
      } else {
        close(empty_fd);
      }
    }
    if (org != NULL) {
      os_setperm(org, os_getperm((const char *)fname) & 0777);
      xfree(org);
    }
  }

  // Remove the backup unless 'backup' option is set
  if (!p_bk && backup != NULL
      && !write_info.bw_conv_error
      && os_remove(backup) != 0) {
    emsg(_("E207: Can't delete backup file"));
  }

  goto nofail;

  // Finish up.  We get here either after failure or success.
fail:
  no_wait_return--;             // may wait for return now
nofail:

  // Done saving, we accept changed buffer warnings again
  buf->b_saving = false;

  xfree(backup);
  if (buffer != smallbuf) {
    xfree(buffer);
  }
  xfree(fenc_tofree);
  xfree(write_info.bw_conv_buf);
  if (write_info.bw_iconv_fd != (iconv_t)-1) {
    iconv_close(write_info.bw_iconv_fd);
    write_info.bw_iconv_fd = (iconv_t)-1;
  }
  os_free_acl(acl);

  if (err.msg != NULL) {
    // - 100 to save some space for further error message
#ifndef UNIX
    add_quoted_fname(IObuff, IOSIZE - 100, buf, (const char *)sfname);
#else
    add_quoted_fname(IObuff, IOSIZE - 100, buf, (const char *)fname);
#endif
    emit_err(&err);

    retval = FAIL;
    if (end == 0) {
      const int attr = HL_ATTR(HLF_E);  // Set highlight for error messages.
      msg_puts_attr(_("\nWARNING: Original file may be lost or damaged\n"),
                    attr | MSG_HIST);
      msg_puts_attr(_("don't quit the editor until the file is successfully written!"),
                    attr | MSG_HIST);

      // Update the timestamp to avoid an "overwrite changed file"
      // prompt when writing again.
      if (os_fileinfo(fname, &file_info_old)) {
        buf_store_file_info(buf, &file_info_old);
        buf->b_mtime_read = buf->b_mtime;
        buf->b_mtime_read_ns = buf->b_mtime_ns;
      }
    }
  }
  msg_scroll = msg_save;

  // When writing the whole file and 'undofile' is set, also write the undo
  // file.
  if (retval == OK && write_undo_file) {
    uint8_t hash[UNDO_HASH_SIZE];

    sha256_finish(&sha_ctx, hash);
    u_write_undo(NULL, false, buf, hash);
  }

  if (!should_abort(retval)) {
    buf_write_do_post_autocmds(buf, fname, eap, append, filtering, reset_changed, whole);
    if (aborting()) {       // autocmds may abort script processing
      retval = false;
    }
  }

  got_int |= prev_got_int;

  return retval;
}

/// Set the name of the current buffer.  Use when the buffer doesn't have a
/// name and a ":r" or ":w" command with a file name is used.
static int set_rw_fname(char *fname, char *sfname)
{
  buf_T *buf = curbuf;

  // It's like the unnamed buffer is deleted....
  if (curbuf->b_p_bl) {
    apply_autocmds(EVENT_BUFDELETE, NULL, NULL, false, curbuf);
  }
  apply_autocmds(EVENT_BUFWIPEOUT, NULL, NULL, false, curbuf);
  if (aborting()) {         // autocmds may abort script processing
    return FAIL;
  }
  if (curbuf != buf) {
    // We are in another buffer now, don't do the renaming.
    emsg(_(e_auchangedbuf));
    return FAIL;
  }

  if (setfname(curbuf, fname, sfname, false) == OK) {
    curbuf->b_flags |= BF_NOTEDITED;
  }

  // ....and a new named one is created
  apply_autocmds(EVENT_BUFNEW, NULL, NULL, false, curbuf);
  if (curbuf->b_p_bl) {
    apply_autocmds(EVENT_BUFADD, NULL, NULL, false, curbuf);
  }
  if (aborting()) {         // autocmds may abort script processing
    return FAIL;
  }

  // Do filetype detection now if 'filetype' is empty.
  if (*curbuf->b_p_ft == NUL) {
    if (augroup_exists("filetypedetect")) {
      (void)do_doautocmd("filetypedetect BufRead", false, NULL);
    }
    do_modelines(0);
  }

  return OK;
}

/// Put file name into the specified buffer with quotes
///
/// Replaces home directory at the start with `~`.
///
/// @param[out]  ret_buf  Buffer to save results to.
/// @param[in]  buf_len  ret_buf length.
/// @param[in]  buf  buf_T file name is coming from.
/// @param[in]  fname  File name to write.
static void add_quoted_fname(char *const ret_buf, const size_t buf_len, const buf_T *const buf,
                             const char *fname)
  FUNC_ATTR_NONNULL_ARG(1)
{
  if (fname == NULL) {
    fname = "-stdin-";
  }
  ret_buf[0] = '"';
  home_replace(buf, fname, ret_buf + 1, buf_len - 4, true);
  xstrlcat(ret_buf, "\" ", buf_len);
}

/// Append message for text mode to IObuff.
///
/// @param eol_type line ending type
///
/// @return true if something was appended.
static bool msg_add_fileformat(int eol_type)
{
#ifndef USE_CRNL
  if (eol_type == EOL_DOS) {
    STRCAT(IObuff, shortmess(SHM_TEXT) ? _("[dos]") : _("[dos format]"));
    return true;
  }
#endif
  if (eol_type == EOL_MAC) {
    STRCAT(IObuff, shortmess(SHM_TEXT) ? _("[mac]") : _("[mac format]"));
    return true;
  }
#ifdef USE_CRNL
  if (eol_type == EOL_UNIX) {
    STRCAT(IObuff, shortmess(SHM_TEXT) ? _("[unix]") : _("[unix format]"));
    return true;
  }
#endif
  return false;
}

/// Append line and character count to IObuff.
void msg_add_lines(int insert_space, long lnum, off_T nchars)
{
  char *p = IObuff + strlen(IObuff);

  if (insert_space) {
    *p++ = ' ';
  }
  if (shortmess(SHM_LINES)) {
    vim_snprintf(p, (size_t)(IOSIZE - (p - IObuff)), "%" PRId64 "L, %" PRId64 "B",
                 (int64_t)lnum, (int64_t)nchars);
  } else {
    vim_snprintf(p, (size_t)(IOSIZE - (p - IObuff)),
                 NGETTEXT("%" PRId64 " line, ", "%" PRId64 " lines, ", lnum),
                 (int64_t)lnum);
    p += strlen(p);
    vim_snprintf(p, (size_t)(IOSIZE - (p - IObuff)),
                 NGETTEXT("%" PRId64 " byte", "%" PRId64 " bytes", nchars),
                 (int64_t)nchars);
  }
}

/// Append message for missing line separator to IObuff.
static void msg_add_eol(void)
{
  STRCAT(IObuff,
         shortmess(SHM_LAST) ? _("[noeol]") : _("[Incomplete last line]"));
}

/// Check modification time of file, before writing to it.
/// The size isn't checked, because using a tool like "gzip" takes care of
/// using the same timestamp but can't set the size.
static int check_mtime(buf_T *buf, FileInfo *file_info)
{
  if (buf->b_mtime_read != 0
      && time_differs(file_info, buf->b_mtime_read, buf->b_mtime_read_ns)) {
    msg_scroll = true;  // Don't overwrite messages here.
    msg_silent = 0;     // Must give this prompt.
    // Don't use emsg() here, don't want to flush the buffers.
    msg_attr(_("WARNING: The file has been changed since reading it!!!"),
             HL_ATTR(HLF_E));
    if (ask_yesno(_("Do you really want to write to it"), true) == 'n') {
      return FAIL;
    }
    msg_scroll = false;  // Always overwrite the file message now.
  }
  return OK;
}

static bool time_differs(const FileInfo *file_info, long mtime, long mtime_ns) FUNC_ATTR_CONST
{
#if defined(__linux__) || defined(MSWIN)
  return file_info->stat.st_mtim.tv_nsec != mtime_ns
         // On a FAT filesystem, esp. under Linux, there are only 5 bits to store
         // the seconds.  Since the roundoff is done when flushing the inode, the
         // time may change unexpectedly by one second!!!
         || file_info->stat.st_mtim.tv_sec - mtime > 1
         || mtime - file_info->stat.st_mtim.tv_sec > 1;
#else
  return file_info->stat.st_mtim.tv_nsec != mtime_ns
         || file_info->stat.st_mtim.tv_sec != mtime;
#endif
}

static int buf_write_convert_with_iconv(struct bw_info *ip, char **bufp, int *lenp)
{
  const char *from;
  size_t fromlen;
  size_t tolen;

  int len = *lenp;

  // Convert with iconv().
  if (ip->bw_restlen > 0) {
    // Need to concatenate the remainder of the previous call and
    // the bytes of the current call.  Use the end of the
    // conversion buffer for this.
    fromlen = (size_t)len + (size_t)ip->bw_restlen;
    char *fp = ip->bw_conv_buf + ip->bw_conv_buflen - fromlen;
    memmove(fp, ip->bw_rest, (size_t)ip->bw_restlen);
    memmove(fp + ip->bw_restlen, *bufp, (size_t)len);
    from = fp;
    tolen = ip->bw_conv_buflen - fromlen;
  } else {
    from = *bufp;
    fromlen = (size_t)len;
    tolen = ip->bw_conv_buflen;
  }
  char *to = ip->bw_conv_buf;

  if (ip->bw_first) {
    size_t save_len = tolen;

    // output the initial shift state sequence
    (void)iconv(ip->bw_iconv_fd, NULL, NULL, &to, &tolen);

    // There is a bug in iconv() on Linux (which appears to be
    // wide-spread) which sets "to" to NULL and messes up "tolen".
    if (to == NULL) {
      to = ip->bw_conv_buf;
      tolen = save_len;
    }
    ip->bw_first = false;
  }

  // If iconv() has an error or there is not enough room, fail.
  if ((iconv(ip->bw_iconv_fd, (void *)&from, &fromlen, &to, &tolen)
       == (size_t)-1 && ICONV_ERRNO != ICONV_EINVAL)
      || fromlen > CONV_RESTLEN) {
    ip->bw_conv_error = true;
    return FAIL;
  }

  // copy remainder to ip->bw_rest[] to be used for the next call.
  if (fromlen > 0) {
    memmove(ip->bw_rest, (void *)from, fromlen);
  }
  ip->bw_restlen = (int)fromlen;

  *bufp = ip->bw_conv_buf;
  *lenp = (int)(to - ip->bw_conv_buf);

  return OK;
}

static int buf_write_convert(struct bw_info *ip, char **bufp, int *lenp)
{
  int flags = ip->bw_flags;  // extra flags

  if (flags & FIO_UTF8) {
    // Convert latin1 in the buffer to UTF-8 in the file.
    char *p = ip->bw_conv_buf;              // translate to buffer
    for (int wlen = 0; wlen < *lenp; wlen++) {
      p += utf_char2bytes((uint8_t)(*bufp)[wlen], p);
    }
    *bufp = ip->bw_conv_buf;
    *lenp = (int)(p - ip->bw_conv_buf);
  } else if (flags & (FIO_UCS4 | FIO_UTF16 | FIO_UCS2 | FIO_LATIN1)) {
    unsigned c;
    int n = 0;
    // Convert UTF-8 bytes in the buffer to UCS-2, UCS-4, UTF-16 or
    // Latin1 chars in the file.
    // translate in-place (can only get shorter) or to buffer
    char *p = flags & FIO_LATIN1 ? *bufp : ip->bw_conv_buf;
    for (int wlen = 0; wlen < *lenp; wlen += n) {
      if (wlen == 0 && ip->bw_restlen != 0) {
        // Use remainder of previous call.  Append the start of
        // buf[] to get a full sequence.  Might still be too
        // short!
        int l = MIN(*lenp, CONV_RESTLEN - ip->bw_restlen);
        memmove(ip->bw_rest + ip->bw_restlen, *bufp, (size_t)l);
        n = utf_ptr2len_len((char *)ip->bw_rest, ip->bw_restlen + l);
        if (n > ip->bw_restlen + *lenp) {
          // We have an incomplete byte sequence at the end to
          // be written.  We can't convert it without the
          // remaining bytes.  Keep them for the next call.
          if (ip->bw_restlen + *lenp > CONV_RESTLEN) {
            return FAIL;
          }
          ip->bw_restlen += *lenp;
          break;
        }
        if (n > 1) {
          c = (unsigned)utf_ptr2char((char *)ip->bw_rest);
        } else {
          c = ip->bw_rest[0];
        }
        if (n >= ip->bw_restlen) {
          n -= ip->bw_restlen;
          ip->bw_restlen = 0;
        } else {
          ip->bw_restlen -= n;
          memmove(ip->bw_rest, ip->bw_rest + n,
                  (size_t)ip->bw_restlen);
          n = 0;
        }
      } else {
        n = utf_ptr2len_len(*bufp + wlen, *lenp - wlen);
        if (n > *lenp - wlen) {
          // We have an incomplete byte sequence at the end to
          // be written.  We can't convert it without the
          // remaining bytes.  Keep them for the next call.
          if (*lenp - wlen > CONV_RESTLEN) {
            return FAIL;
          }
          ip->bw_restlen = *lenp - wlen;
          memmove(ip->bw_rest, *bufp + wlen,
                  (size_t)ip->bw_restlen);
          break;
        }
        if (n > 1) {
          c = (unsigned)utf_ptr2char(*bufp + wlen);
        } else {
          c = (uint8_t)(*bufp)[wlen];
        }
      }

      if (ucs2bytes(c, &p, flags) && !ip->bw_conv_error) {
        ip->bw_conv_error = true;
        ip->bw_conv_error_lnum = ip->bw_start_lnum;
      }
      if (c == NL) {
        ip->bw_start_lnum++;
      }
    }
    if (flags & FIO_LATIN1) {
      *lenp = (int)(p - *bufp);
    } else {
      *bufp = ip->bw_conv_buf;
      *lenp = (int)(p - ip->bw_conv_buf);
    }
  }

  if (ip->bw_iconv_fd != (iconv_t)-1) {
    if (buf_write_convert_with_iconv(ip, bufp, lenp) == FAIL) {
      return FAIL;
    }
  }

  return OK;
}

/// Call write() to write a number of bytes to the file.
/// Handles 'encoding' conversion.
///
/// @return  FAIL for failure, OK otherwise.
static int buf_write_bytes(struct bw_info *ip)
{
  char *buf = ip->bw_buf;    // data to write
  int len = ip->bw_len;      // length of data
  int flags = ip->bw_flags;  // extra flags

  // Skip conversion when writing the BOM.
  if (!(flags & FIO_NOCONVERT)) {
    if (buf_write_convert(ip, &buf, &len) == FAIL) {
      return FAIL;
    }
  }

  if (ip->bw_fd < 0) {
    // Only checking conversion, which is OK if we get here.
    return OK;
  }
  int wlen = (int)write_eintr(ip->bw_fd, buf, (size_t)len);
  return (wlen < len) ? FAIL : OK;
}

/// Convert a Unicode character to bytes.
///
/// @param c character to convert
/// @param[in,out] pp pointer to store the result at
/// @param flags FIO_ flags that specify which encoding to use
///
/// @return true for an error, false when it's OK.
static bool ucs2bytes(unsigned c, char **pp, int flags)
  FUNC_ATTR_NONNULL_ALL
{
  uint8_t *p = (uint8_t *)(*pp);
  bool error = false;

  if (flags & FIO_UCS4) {
    if (flags & FIO_ENDIAN_L) {
      *p++ = (uint8_t)c;
      *p++ = (uint8_t)(c >> 8);
      *p++ = (uint8_t)(c >> 16);
      *p++ = (uint8_t)(c >> 24);
    } else {
      *p++ = (uint8_t)(c >> 24);
      *p++ = (uint8_t)(c >> 16);
      *p++ = (uint8_t)(c >> 8);
      *p++ = (uint8_t)c;
    }
  } else if (flags & (FIO_UCS2 | FIO_UTF16)) {
    if (c >= 0x10000) {
      if (flags & FIO_UTF16) {
        // Make two words, ten bits of the character in each.  First
        // word is 0xd800 - 0xdbff, second one 0xdc00 - 0xdfff
        c -= 0x10000;
        if (c >= 0x100000) {
          error = true;
        }
        int cc = (int)(((c >> 10) & 0x3ff) + 0xd800);
        if (flags & FIO_ENDIAN_L) {
          *p++ = (uint8_t)cc;
          *p++ = (uint8_t)(cc >> 8);
        } else {
          *p++ = (uint8_t)(cc >> 8);
          *p++ = (uint8_t)cc;
        }
        c = (c & 0x3ff) + 0xdc00;
      } else {
        error = true;
      }
    }
    if (flags & FIO_ENDIAN_L) {
      *p++ = (uint8_t)c;
      *p++ = (uint8_t)(c >> 8);
    } else {
      *p++ = (uint8_t)(c >> 8);
      *p++ = (uint8_t)c;
    }
  } else {  // Latin1
    if (c >= 0x100) {
      error = true;
      *p++ = 0xBF;
    } else {
      *p++ = (uint8_t)c;
    }
  }

  *pp = (char *)p;
  return error;
}

/// Return true if file encoding "fenc" requires conversion from or to
/// 'encoding'.
///
/// @param fenc file encoding to check
///
/// @return true if conversion is required
static bool need_conversion(const char *fenc)
  FUNC_ATTR_NONNULL_ALL FUNC_ATTR_WARN_UNUSED_RESULT
{
  int same_encoding;
  int fenc_flags;

  if (*fenc == NUL || strcmp(p_enc, fenc) == 0) {
    same_encoding = true;
    fenc_flags = 0;
  } else {
    // Ignore difference between "ansi" and "latin1", "ucs-4" and
    // "ucs-4be", etc.
    int enc_flags = get_fio_flags(p_enc);
    fenc_flags = get_fio_flags(fenc);
    same_encoding = (enc_flags != 0 && fenc_flags == enc_flags);
  }
  if (same_encoding) {
    // Specified file encoding matches UTF-8.
    return false;
  }

  // Encodings differ.  However, conversion is not needed when 'enc' is any
  // Unicode encoding and the file is UTF-8.
  return !(fenc_flags == FIO_UTF8);
}

/// Return the FIO_ flags needed for the internal conversion if 'name' was
/// unicode or latin1, otherwise 0. If "name" is an empty string,
/// use 'encoding'.
///
/// @param name string to check for encoding
static int get_fio_flags(const char *name)
{
  if (*name == NUL) {
    name = p_enc;
  }
  int prop = enc_canon_props(name);
  if (prop & ENC_UNICODE) {
    if (prop & ENC_2BYTE) {
      if (prop & ENC_ENDIAN_L) {
        return FIO_UCS2 | FIO_ENDIAN_L;
      }
      return FIO_UCS2;
    }
    if (prop & ENC_4BYTE) {
      if (prop & ENC_ENDIAN_L) {
        return FIO_UCS4 | FIO_ENDIAN_L;
      }
      return FIO_UCS4;
    }
    if (prop & ENC_2WORD) {
      if (prop & ENC_ENDIAN_L) {
        return FIO_UTF16 | FIO_ENDIAN_L;
      }
      return FIO_UTF16;
    }
    return FIO_UTF8;
  }
  if (prop & ENC_LATIN1) {
    return FIO_LATIN1;
  }
  // must be ENC_DBCS, requires iconv()
  return 0;
}

/// Check for a Unicode BOM (Byte Order Mark) at the start of p[size].
/// "size" must be at least 2.
///
/// @return  the name of the encoding and set "*lenp" to the length or,
///          NULL when no BOM found.
static char *check_for_bom(const char *p_in, int size, int *lenp, int flags)
{
  const uint8_t *p = (const uint8_t *)p_in;
  char *name = NULL;
  int len = 2;

  if (p[0] == 0xef && p[1] == 0xbb && size >= 3 && p[2] == 0xbf
      && (flags == FIO_ALL || flags == FIO_UTF8 || flags == 0)) {
    name = "utf-8";             // EF BB BF
    len = 3;
  } else if (p[0] == 0xff && p[1] == 0xfe) {
    if (size >= 4 && p[2] == 0 && p[3] == 0
        && (flags == FIO_ALL || flags == (FIO_UCS4 | FIO_ENDIAN_L))) {
      name = "ucs-4le";         // FF FE 00 00
      len = 4;
    } else if (flags == (FIO_UCS2 | FIO_ENDIAN_L)) {
      name = "ucs-2le";         // FF FE
    } else if (flags == FIO_ALL
               || flags == (FIO_UTF16 | FIO_ENDIAN_L)) {
      // utf-16le is preferred, it also works for ucs-2le text
      name = "utf-16le";        // FF FE
    }
  } else if (p[0] == 0xfe && p[1] == 0xff
             && (flags == FIO_ALL || flags == FIO_UCS2 || flags ==
                 FIO_UTF16)) {
    // Default to utf-16, it works also for ucs-2 text.
    if (flags == FIO_UCS2) {
      name = "ucs-2";           // FE FF
    } else {
      name = "utf-16";          // FE FF
    }
  } else if (size >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0xfe
             && p[3] == 0xff && (flags == FIO_ALL || flags == FIO_UCS4)) {
    name = "ucs-4";             // 00 00 FE FF
    len = 4;
  }

  *lenp = len;
  return name;
}

/// Generate a BOM in "buf[4]" for encoding "name".
///
/// @return  the length of the BOM (zero when no BOM).
static int make_bom(char *buf_in, char *name)
{
  uint8_t *buf = (uint8_t *)buf_in;
  int flags = get_fio_flags(name);

  // Can't put a BOM in a non-Unicode file.
  if (flags == FIO_LATIN1 || flags == 0) {
    return 0;
  }

  if (flags == FIO_UTF8) {      // UTF-8
    buf[0] = 0xef;
    buf[1] = 0xbb;
    buf[2] = 0xbf;
    return 3;
  }
  char *p = (char *)buf;
  (void)ucs2bytes(0xfeff, &p, flags);
  return (int)((uint8_t *)p - buf);
}

/// Shorten filename of a buffer.
///
/// @param force  when true: Use full path from now on for files currently being
///               edited, both for file name and swap file name.  Try to shorten the file
///               names a bit, if safe to do so.
///               when false: Only try to shorten absolute file names.
///
/// For buffers that have buftype "nofile" or "scratch": never change the file
/// name.
void shorten_buf_fname(buf_T *buf, char *dirname, int force)
{
  if (buf->b_fname != NULL
      && !bt_nofilename(buf)
      && !path_with_url(buf->b_fname)
      && (force
          || buf->b_sfname == NULL
          || path_is_absolute(buf->b_sfname))) {
    if (buf->b_sfname != buf->b_ffname) {
      XFREE_CLEAR(buf->b_sfname);
    }
    char *p = path_shorten_fname(buf->b_ffname, dirname);
    if (p != NULL) {
      buf->b_sfname = xstrdup(p);
      buf->b_fname = buf->b_sfname;
    }
    if (p == NULL) {
      buf->b_fname = buf->b_ffname;
    }
  }
}

/// Shorten filenames for all buffers.
void shorten_fnames(int force)
{
  char dirname[MAXPATHL];

  os_dirname(dirname, MAXPATHL);
  FOR_ALL_BUFFERS(buf) {
    shorten_buf_fname(buf, (char *)dirname, force);

    // Always make the swap file name a full path, a "nofile" buffer may
    // also have a swap file.
    mf_fullname(buf->b_ml.ml_mfp);
  }
  status_redraw_all();
  redraw_tabline = true;
}

/// Get new filename ended by given extension.
///
/// @param fname        The original filename.
///                     If NULL, use current directory name and ext to
///                     compute new filename.
/// @param ext          The extension to add to the filename.
///                     4 chars max if prefixed with a dot, 3 otherwise.
/// @param prepend_dot  If true, prefix ext with a dot.
///                     Does nothing if ext already starts with a dot, or
///                     if fname is NULL.
///
/// @return [allocated] - A new filename, made up from:
///                       * fname + ext, if fname not NULL.
///                       * current dir + ext, if fname is NULL.
///                       Result is guaranteed to:
///                       * be ended by <ext>.
///                       * have a basename with at most BASENAMELEN chars:
///                         original basename is truncated if necessary.
///                       * be different than original: basename chars are
///                         replaced by "_" if necessary. If that can't be done
///                         because truncated value of original filename was
///                         made of all underscores, replace first "_" by "v".
///                     - NULL, if fname is NULL and there was a problem trying
///                       to get current directory.
char *modname(const char *fname, const char *ext, bool prepend_dot)
  FUNC_ATTR_NONNULL_ARG(2)
{
  char *retval;
  size_t fnamelen;
  size_t extlen = strlen(ext);

  // If there is no file name we must get the name of the current directory
  // (we need the full path in case :cd is used).
  if (fname == NULL || *fname == NUL) {
    retval = xmalloc(MAXPATHL + extlen + 3);  // +3 for PATHSEP, "_" (Win), NUL
    if (os_dirname(retval, MAXPATHL) == FAIL
        || strlen(retval) == 0) {
      xfree(retval);
      return NULL;
    }
    add_pathsep(retval);
    fnamelen = strlen(retval);
    prepend_dot = false;  // nothing to prepend a dot to
  } else {
    fnamelen = strlen(fname);
    retval = xmalloc(fnamelen + extlen + 3);
    strcpy(retval, fname);  // NOLINT(runtime/printf)
  }

  // Search backwards until we hit a '/', '\' or ':'.
  // Then truncate what is after the '/', '\' or ':' to BASENAMELEN characters.
  char *ptr = NULL;
  for (ptr = retval + fnamelen; ptr > retval; MB_PTR_BACK(retval, ptr)) {
    if (vim_ispathsep(*ptr)) {
      ptr++;
      break;
    }
  }

  // the file name has at most BASENAMELEN characters.
  if (strlen(ptr) > BASENAMELEN) {
    ptr[BASENAMELEN] = '\0';
  }

  char *s = ptr + strlen(ptr);

  // Append the extension.
  // ext can start with '.' and cannot exceed 3 more characters.
  strcpy(s, ext);  // NOLINT(runtime/printf)

  char *e;
  // Prepend the dot if needed.
  if (prepend_dot && *(e = path_tail(retval)) != '.') {
    STRMOVE(e + 1, e);
    *e = '.';
  }

  // Check that, after appending the extension, the file name is really
  // different.
  if (fname != NULL && strcmp(fname, retval) == 0) {
    // we search for a character that can be replaced by '_'
    while (--s >= ptr) {
      if (*s != '_') {
        *s = '_';
        break;
      }
    }
    if (s < ptr) {  // fname was "________.<ext>", how tricky!
      *ptr = 'v';
    }
  }
  return retval;
}

/// Like fgets(), but if the file line is too long, it is truncated and the
/// rest of the line is thrown away.
///
/// @param[out] buf buffer to fill
/// @param size size of the buffer
/// @param fp file to read from
///
/// @return true for EOF or error
bool vim_fgets(char *buf, int size, FILE *fp)
  FUNC_ATTR_NONNULL_ALL
{
  char *retval;

  assert(size > 0);
  buf[size - 2] = NUL;

  do {
    errno = 0;
    retval = fgets(buf, size, fp);
  } while (retval == NULL && errno == EINTR && ferror(fp));

  if (buf[size - 2] != NUL && buf[size - 2] != '\n') {
    char tbuf[200];

    buf[size - 1] = NUL;  // Truncate the line.

    // Now throw away the rest of the line:
    do {
      tbuf[sizeof(tbuf) - 2] = NUL;
      errno = 0;
      retval = fgets((char *)tbuf, sizeof(tbuf), fp);
      if (retval == NULL && (feof(fp) || errno != EINTR)) {
        break;
      }
    } while (tbuf[sizeof(tbuf) - 2] != NUL && tbuf[sizeof(tbuf) - 2] != '\n');
  }
  return retval == NULL;
}

/// Read 2 bytes from "fd" and turn them into an int, MSB first.
///
/// @return  -1 when encountering EOF.
int get2c(FILE *fd)
{
  const int n = getc(fd);
  if (n == EOF) {
    return -1;
  }
  const int c = getc(fd);
  if (c == EOF) {
    return -1;
  }
  return (n << 8) + c;
}

/// Read 3 bytes from "fd" and turn them into an int, MSB first.
///
/// @return  -1 when encountering EOF.
int get3c(FILE *fd)
{
  int n = getc(fd);
  if (n == EOF) {
    return -1;
  }
  int c = getc(fd);
  if (c == EOF) {
    return -1;
  }
  n = (n << 8) + c;
  c = getc(fd);
  if (c == EOF) {
    return -1;
  }
  return (n << 8) + c;
}

/// Read 4 bytes from "fd" and turn them into an int, MSB first.
///
/// @return  -1 when encountering EOF.
int get4c(FILE *fd)
{
  // Use unsigned rather than int otherwise result is undefined
  // when left-shift sets the MSB.
  unsigned n;

  int c = getc(fd);
  if (c == EOF) {
    return -1;
  }
  n = (unsigned)c;
  c = getc(fd);
  if (c == EOF) {
    return -1;
  }
  n = (n << 8) + (unsigned)c;
  c = getc(fd);
  if (c == EOF) {
    return -1;
  }
  n = (n << 8) + (unsigned)c;
  c = getc(fd);
  if (c == EOF) {
    return -1;
  }
  n = (n << 8) + (unsigned)c;
  return (int)n;
}

/// Read 8 bytes from `fd` and turn them into a time_t, MSB first.
///
/// @return  -1 when encountering EOF.
time_t get8ctime(FILE *fd)
{
  time_t n = 0;

  for (int i = 0; i < 8; i++) {
    const int c = getc(fd);
    if (c == EOF) {
      return -1;
    }
    n = (n << 8) + c;
  }
  return n;
}

/// Reads a string of length "cnt" from "fd" into allocated memory.
///
/// @return  pointer to the string or NULL when unable to read that many bytes.
char *read_string(FILE *fd, size_t cnt)
{
  char *str = xmallocz(cnt);
  for (size_t i = 0; i < cnt; i++) {
    int c = getc(fd);
    if (c == EOF) {
      xfree(str);
      return NULL;
    }
    str[i] = (char)c;
  }
  return str;
}

/// Writes a number to file "fd", most significant bit first, in "len" bytes.
///
/// @return  false in case of an error.
bool put_bytes(FILE *fd, uintmax_t number, size_t len)
{
  assert(len > 0);
  for (size_t i = len - 1; i < len; i--) {
    if (putc((int)(number >> (i * 8)), fd) == EOF) {
      return false;
    }
  }
  return true;
}

/// Writes time_t to file "fd" in 8 bytes.
///
/// @return  FAIL when the write failed.
int put_time(FILE *fd, time_t time_)
{
  uint8_t buf[8];
  time_to_bytes(time_, buf);
  return fwrite(buf, sizeof(uint8_t), ARRAY_SIZE(buf), fd) == 1 ? OK : FAIL;
}

static int rename_with_tmp(const char *const from, const char *const to)
{
  // Find a name that doesn't exist and is in the same directory.
  // Rename "from" to "tempname" and then rename "tempname" to "to".
  if (strlen(from) >= MAXPATHL - 5) {
    return -1;
  }

  char tempname[MAXPATHL + 1];
  STRCPY(tempname, from);
  for (int n = 123; n < 99999; n++) {
    char *tail = path_tail(tempname);
    snprintf(tail, (size_t)((MAXPATHL + 1) - (tail - tempname - 1)), "%d", n);

    if (!os_path_exists(tempname)) {
      if (os_rename(from, tempname) == OK) {
        if (os_rename(tempname, to) == OK) {
          return 0;
        }
        // Strange, the second step failed.  Try moving the
        // file back and return failure.
        (void)os_rename(tempname, from);
        return -1;
      }
      // If it fails for one temp name it will most likely fail
      // for any temp name, give up.
      return -1;
    }
  }
  return -1;
}

/// os_rename() only works if both files are on the same file system, this
/// function will (attempts to?) copy the file across if rename fails -- webb
///
/// @return  -1 for failure, 0 for success
int vim_rename(const char *from, const char *to)
  FUNC_ATTR_NONNULL_ALL
{
  char *errmsg = NULL;
  bool use_tmp_file = false;

  // When the names are identical, there is nothing to do.  When they refer
  // to the same file (ignoring case and slash/backslash differences) but
  // the file name differs we need to go through a temp file.
  if (path_fnamecmp(from, to) == 0) {
    if (p_fic && (strcmp(path_tail((char *)from), path_tail((char *)to))
                  != 0)) {
      use_tmp_file = true;
    } else {
      return 0;
    }
  }

  // Fail if the "from" file doesn't exist. Avoids that "to" is deleted.
  FileInfo from_info;
  if (!os_fileinfo((char *)from, &from_info)) {
    return -1;
  }

  // It's possible for the source and destination to be the same file.
  // This happens when "from" and "to" differ in case and are on a FAT32
  // filesystem. In that case go through a temp file name.
  FileInfo to_info;
  if (os_fileinfo((char *)to, &to_info)
      && os_fileinfo_id_equal(&from_info,  &to_info)) {
    use_tmp_file = true;
  }

  if (use_tmp_file) {
    return rename_with_tmp(from, to);
  }

  // Delete the "to" file, this is required on some systems to make the
  // os_rename() work, on other systems it makes sure that we don't have
  // two files when the os_rename() fails.

  os_remove((char *)to);

  // First try a normal rename, return if it works.
  if (os_rename(from, to) == OK) {
    return 0;
  }

  // Rename() failed, try copying the file.
  long perm = os_getperm(from);
  // For systems that support ACL: get the ACL from the original file.
  vim_acl_T acl = os_get_acl(from);
  int fd_in = os_open((char *)from, O_RDONLY, 0);
  if (fd_in < 0) {
    os_free_acl(acl);
    return -1;
  }

  // Create the new file with same permissions as the original.
  int fd_out = os_open((char *)to, O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW, (int)perm);
  if (fd_out < 0) {
    close(fd_in);
    os_free_acl(acl);
    return -1;
  }

  // Avoid xmalloc() here as vim_rename() is called by buf_write() when nvim
  // is `preserve_exit()`ing.
  char *buffer = try_malloc(BUFSIZE);
  if (buffer == NULL) {
    close(fd_out);
    close(fd_in);
    os_free_acl(acl);
    return -1;
  }

  int n;
  while ((n = (int)read_eintr(fd_in, buffer, BUFSIZE)) > 0) {
    if (write_eintr(fd_out, buffer, (size_t)n) != n) {
      errmsg = _("E208: Error writing to \"%s\"");
      break;
    }
  }

  xfree(buffer);
  close(fd_in);
  if (close(fd_out) < 0) {
    errmsg = _("E209: Error closing \"%s\"");
  }
  if (n < 0) {
    errmsg = _("E210: Error reading \"%s\"");
    to = from;
  }
#ifndef UNIX  // For Unix os_open() already set the permission.
  os_setperm(to, perm);
#endif
  os_set_acl(to, acl);
  os_free_acl(acl);
  if (errmsg != NULL) {
    semsg(errmsg, to);
    return -1;
  }
  os_remove((char *)from);
  return 0;
}

static int already_warned = false;

/// Check if any not hidden buffer has been changed.
/// Postpone the check if there are characters in the stuff buffer, a global
/// command is being executed, a mapping is being executed or an autocommand is
/// busy.
///
/// @param focus  called for GUI focus event
///
/// @return       true if some message was written (screen should be redrawn and cursor positioned).
int check_timestamps(int focus)
{
  // Don't check timestamps while system() or another low-level function may
  // cause us to lose and gain focus.
  if (no_check_timestamps > 0) {
    return false;
  }

  // Avoid doing a check twice.  The OK/Reload dialog can cause a focus
  // event and we would keep on checking if the file is steadily growing.
  // Do check again after typing something.
  if (focus && did_check_timestamps) {
    need_check_timestamps = true;
    return false;
  }

  int didit = 0;

  if (!stuff_empty() || global_busy || !typebuf_typed()
      || autocmd_busy || curbuf->b_ro_locked > 0
      || allbuf_lock > 0) {
    need_check_timestamps = true;               // check later
  } else {
    no_wait_return++;
    did_check_timestamps = true;
    already_warned = false;
    FOR_ALL_BUFFERS(buf) {
      // Only check buffers in a window.
      if (buf->b_nwindows > 0) {
        bufref_T bufref;
        set_bufref(&bufref, buf);
        const int n = buf_check_timestamp(buf);
        if (didit < n) {
          didit = n;
        }
        if (n > 0 && !bufref_valid(&bufref)) {
          // Autocommands have removed the buffer, start at the first one again.
          buf = firstbuf;
          continue;
        }
      }
    }
    no_wait_return--;
    need_check_timestamps = false;
    if (need_wait_return && didit == 2) {
      // make sure msg isn't overwritten
      msg_puts("\n");
      ui_flush();
    }
  }
  return didit;
}

/// Move all the lines from buffer "frombuf" to buffer "tobuf".
///
/// @return  OK or FAIL.
///          When FAIL "tobuf" is incomplete and/or "frombuf" is not empty.
static int move_lines(buf_T *frombuf, buf_T *tobuf)
{
  buf_T *tbuf = curbuf;
  int retval = OK;

  // Copy the lines in "frombuf" to "tobuf".
  curbuf = tobuf;
  for (linenr_T lnum = 1; lnum <= frombuf->b_ml.ml_line_count; lnum++) {
    char *p = xstrdup(ml_get_buf(frombuf, lnum, false));
    if (ml_append(lnum - 1, p, 0, false) == FAIL) {
      xfree(p);
      retval = FAIL;
      break;
    }
    xfree(p);
  }

  // Delete all the lines in "frombuf".
  if (retval != FAIL) {
    curbuf = frombuf;
    for (linenr_T lnum = curbuf->b_ml.ml_line_count; lnum > 0; lnum--) {
      if (ml_delete(lnum, false) == FAIL) {
        // Oops!  We could try putting back the saved lines, but that
        // might fail again...
        retval = FAIL;
        break;
      }
    }
  }

  curbuf = tbuf;
  return retval;
}

/// Check if buffer "buf" has been changed.
/// Also check if the file for a new buffer unexpectedly appeared.
///
/// @return  1 if a changed buffer was found or,
///          2 if a message has been displayed or,
///          0 otherwise.
int buf_check_timestamp(buf_T *buf)
  FUNC_ATTR_NONNULL_ALL
{
  int retval = 0;
  char *mesg = NULL;
  char *mesg2 = "";
  bool helpmesg = false;

  enum {
    RELOAD_NONE,
    RELOAD_NORMAL,
    RELOAD_DETECT,
  } reload = RELOAD_NONE;

  bool can_reload = false;
  uint64_t orig_size = buf->b_orig_size;
  int orig_mode = buf->b_orig_mode;
  static bool busy = false;

  bufref_T bufref;
  set_bufref(&bufref, buf);

  // If its a terminal, there is no file name, the buffer is not loaded,
  // 'buftype' is set, we are in the middle of a save or being called
  // recursively: ignore this buffer.
  if (buf->terminal
      || buf->b_ffname == NULL
      || buf->b_ml.ml_mfp == NULL
      || !bt_normal(buf)
      || buf->b_saving
      || busy) {
    return 0;
  }

  FileInfo file_info;
  bool file_info_ok;
  if (!(buf->b_flags & BF_NOTEDITED)
      && buf->b_mtime != 0
      && (!(file_info_ok = os_fileinfo(buf->b_ffname, &file_info))
          || time_differs(&file_info, buf->b_mtime, buf->b_mtime_ns)
          || (int)file_info.stat.st_mode != buf->b_orig_mode)) {
    const long prev_b_mtime = buf->b_mtime;

    retval = 1;

    // set b_mtime to stop further warnings (e.g., when executing
    // FileChangedShell autocmd)
    if (!file_info_ok) {
      // Check the file again later to see if it re-appears.
      buf->b_mtime = -1;
      buf->b_orig_size = 0;
      buf->b_orig_mode = 0;
    } else {
      buf_store_file_info(buf, &file_info);
    }

    if (os_isdir(buf->b_fname)) {
      // Don't do anything for a directory.  Might contain the file explorer.
    } else if ((buf->b_p_ar >= 0 ? buf->b_p_ar : p_ar)
               && !bufIsChanged(buf) && file_info_ok) {
      // If 'autoread' is set, the buffer has no changes and the file still
      // exists, reload the buffer.  Use the buffer-local option value if it
      // was set, the global option value otherwise.
      reload = RELOAD_NORMAL;
    } else {
      char *reason;
      if (!file_info_ok) {
        reason = "deleted";
      } else if (bufIsChanged(buf)) {
        reason = "conflict";
      } else if (orig_size != buf->b_orig_size || buf_contents_changed(buf)) {
        reason = "changed";
      } else if (orig_mode != buf->b_orig_mode) {
        reason = "mode";
      } else {
        reason = "time";
      }

      // Only give the warning if there are no FileChangedShell
      // autocommands.
      // Avoid being called recursively by setting "busy".
      busy = true;
      set_vim_var_string(VV_FCS_REASON, reason, -1);
      set_vim_var_string(VV_FCS_CHOICE, "", -1);
      allbuf_lock++;
      bool n = apply_autocmds(EVENT_FILECHANGEDSHELL, buf->b_fname, buf->b_fname, false, buf);
      allbuf_lock--;
      busy = false;
      if (n) {
        if (!bufref_valid(&bufref)) {
          emsg(_("E246: FileChangedShell autocommand deleted buffer"));
        }
        char *s = get_vim_var_str(VV_FCS_CHOICE);
        if (strcmp(s, "reload") == 0 && *reason != 'd') {
          reload = RELOAD_NORMAL;
        } else if (strcmp(s, "edit") == 0) {
          reload = RELOAD_DETECT;
        } else if (strcmp(s, "ask") == 0) {
          n = false;
        } else {
          return 2;
        }
      }
      if (!n) {
        if (*reason == 'd') {
          // Only give the message once.
          if (prev_b_mtime != -1) {
            mesg = _("E211: File \"%s\" no longer available");
          }
        } else {
          helpmesg = true;
          can_reload = true;

          // Check if the file contents really changed to avoid
          // giving a warning when only the timestamp was set (e.g.,
          // checked out of CVS).  Always warn when the buffer was
          // changed.
          if (reason[2] == 'n') {
            mesg = _(
                    "W12: Warning: File \"%s\" has changed and the buffer was changed in Vim as well");
            mesg2 = _("See \":help W12\" for more info.");
          } else if (reason[1] == 'h') {
            mesg = _("W11: Warning: File \"%s\" has changed since editing started");
            mesg2 = _("See \":help W11\" for more info.");
          } else if (*reason == 'm') {
            mesg = _("W16: Warning: Mode of file \"%s\" has changed since editing started");
            mesg2 = _("See \":help W16\" for more info.");
          } else {
            // Only timestamp changed, store it to avoid a warning
            // in check_mtime() later.
            buf->b_mtime_read = buf->b_mtime;
            buf->b_mtime_read_ns = buf->b_mtime_ns;
          }
        }
      }
    }
  } else if ((buf->b_flags & BF_NEW) && !(buf->b_flags & BF_NEW_W)
             && os_path_exists(buf->b_ffname)) {
    retval = 1;
    mesg = _("W13: Warning: File \"%s\" has been created after editing started");
    buf->b_flags |= BF_NEW_W;
    can_reload = true;
  }

  if (mesg != NULL) {
    char *path = home_replace_save(buf, buf->b_fname);
    if (!helpmesg) {
      mesg2 = "";
    }
    const size_t tbuf_len = strlen(path) + strlen(mesg) + strlen(mesg2) + 2;
    char *const tbuf = xmalloc(tbuf_len);
    snprintf(tbuf, tbuf_len, mesg, path);
    // Set warningmsg here, before the unimportant and output-specific
    // mesg2 has been appended.
    set_vim_var_string(VV_WARNINGMSG, tbuf, -1);
    if (can_reload) {
      if (*mesg2 != NUL) {
        xstrlcat(tbuf, "\n", tbuf_len - 1);
        xstrlcat(tbuf, mesg2, tbuf_len - 1);
      }
      switch (do_dialog(VIM_WARNING, _("Warning"), tbuf,
                        _("&OK\n&Load File\nLoad File &and Options"),
                        1, NULL, true)) {
      case 2:
        reload = RELOAD_NORMAL;
        break;
      case 3:
        reload = RELOAD_DETECT;
        break;
      }
    } else if (State > MODE_NORMAL_BUSY || (State & MODE_CMDLINE) || already_warned) {
      if (*mesg2 != NUL) {
        xstrlcat(tbuf, "; ", tbuf_len - 1);
        xstrlcat(tbuf, mesg2, tbuf_len - 1);
      }
      emsg(tbuf);
      retval = 2;
    } else {
      if (!autocmd_busy) {
        msg_start();
        msg_puts_attr(tbuf, HL_ATTR(HLF_E) + MSG_HIST);
        if (*mesg2 != NUL) {
          msg_puts_attr(mesg2, HL_ATTR(HLF_W) + MSG_HIST);
        }
        msg_clr_eos();
        (void)msg_end();
        if (emsg_silent == 0 && !in_assert_fails) {
          ui_flush();
          // give the user some time to think about it
          os_delay(1004L, true);

          // don't redraw and erase the message
          redraw_cmdline = false;
        }
      }
      already_warned = true;
    }

    xfree(path);
    xfree(tbuf);
  }

  if (reload != RELOAD_NONE) {
    // Reload the buffer.
    buf_reload(buf, orig_mode, reload == RELOAD_DETECT);
    if (buf->b_p_udf && buf->b_ffname != NULL) {
      uint8_t hash[UNDO_HASH_SIZE];

      // Any existing undo file is unusable, write it now.
      u_compute_hash(buf, hash);
      u_write_undo(NULL, false, buf, hash);
    }
  }

  // Trigger FileChangedShell when the file was changed in any way.
  if (bufref_valid(&bufref) && retval != 0) {
    (void)apply_autocmds(EVENT_FILECHANGEDSHELLPOST, buf->b_fname, buf->b_fname, false, buf);
  }
  return retval;
}

/// Reload a buffer that is already loaded.
/// Used when the file was changed outside of Vim.
/// "orig_mode" is buf->b_orig_mode before the need for reloading was detected.
/// buf->b_orig_mode may have been reset already.
void buf_reload(buf_T *buf, int orig_mode, bool reload_options)
{
  exarg_T ea;
  int old_ro = buf->b_p_ro;
  buf_T *savebuf;
  bufref_T bufref;
  int saved = OK;
  aco_save_T aco;
  int flags = READ_NEW;

  // Set curwin/curbuf for "buf" and save some things.
  aucmd_prepbuf(&aco, buf);

  // Unless reload_options is set, we only want to read the text from the
  // file, not reset the syntax highlighting, clear marks, diff status, etc.
  // Force the fileformat and encoding to be the same.
  if (reload_options) {
    CLEAR_FIELD(ea);
  } else {
    prep_exarg(&ea, buf);
  }

  pos_T old_cursor = curwin->w_cursor;
  linenr_T old_topline = curwin->w_topline;

  if (p_ur < 0 || curbuf->b_ml.ml_line_count <= p_ur) {
    // Save all the text, so that the reload can be undone.
    // Sync first so that this is a separate undo-able action.
    u_sync(false);
    saved = u_savecommon(curbuf, 0, curbuf->b_ml.ml_line_count + 1, 0, true);
    flags |= READ_KEEP_UNDO;
  }

  // To behave like when a new file is edited (matters for
  // BufReadPost autocommands) we first need to delete the current
  // buffer contents.  But if reading the file fails we should keep
  // the old contents.  Can't use memory only, the file might be
  // too big.  Use a hidden buffer to move the buffer contents to.
  if (buf_is_empty(curbuf) || saved == FAIL) {
    savebuf = NULL;
  } else {
    // Allocate a buffer without putting it in the buffer list.
    savebuf = buflist_new(NULL, NULL, (linenr_T)1, BLN_DUMMY);
    set_bufref(&bufref, savebuf);
    if (savebuf != NULL && buf == curbuf) {
      // Open the memline.
      curbuf = savebuf;
      curwin->w_buffer = savebuf;
      saved = ml_open(curbuf);
      curbuf = buf;
      curwin->w_buffer = buf;
    }
    if (savebuf == NULL || saved == FAIL || buf != curbuf
        || move_lines(buf, savebuf) == FAIL) {
      semsg(_("E462: Could not prepare for reloading \"%s\""),
            buf->b_fname);
      saved = FAIL;
    }
  }

  if (saved == OK) {
    curbuf->b_flags |= BF_CHECK_RO;           // check for RO again
    keep_filetype = true;                     // don't detect 'filetype'
    if (readfile(buf->b_ffname, buf->b_fname, (linenr_T)0, (linenr_T)0,
                 (linenr_T)MAXLNUM, &ea, flags, false) != OK) {
      if (!aborting()) {
        semsg(_("E321: Could not reload \"%s\""), buf->b_fname);
      }
      if (savebuf != NULL && bufref_valid(&bufref) && buf == curbuf) {
        // Put the text back from the save buffer.  First
        // delete any lines that readfile() added.
        while (!buf_is_empty(curbuf)) {
          if (ml_delete(buf->b_ml.ml_line_count, false) == FAIL) {
            break;
          }
        }
        (void)move_lines(savebuf, buf);
      }
    } else if (buf == curbuf) {  // "buf" still valid.
      // Mark the buffer as unmodified and free undo info.
      unchanged(buf, true, true);
      if ((flags & READ_KEEP_UNDO) == 0) {
        u_blockfree(buf);
        u_clearall(buf);
      } else {
        // Mark all undo states as changed.
        u_unchanged(curbuf);
      }
      buf_updates_unload(curbuf, true);
      curbuf->b_mod_set = true;
    }
  }
  xfree(ea.cmd);

  if (savebuf != NULL && bufref_valid(&bufref)) {
    wipe_buffer(savebuf, false);
  }

  // Invalidate diff info if necessary.
  diff_invalidate(curbuf);

  // Restore the topline and cursor position and check it (lines may
  // have been removed).
  if (old_topline > curbuf->b_ml.ml_line_count) {
    curwin->w_topline = curbuf->b_ml.ml_line_count;
  } else {
    curwin->w_topline = old_topline;
  }
  curwin->w_cursor = old_cursor;
  check_cursor();
  update_topline(curwin);
  keep_filetype = false;

  // Update folds unless they are defined manually.
  FOR_ALL_TAB_WINDOWS(tp, wp) {
    if (wp->w_buffer == curwin->w_buffer
        && !foldmethodIsManual(wp)) {
      foldUpdateAll(wp);
    }
  }

  // If the mode didn't change and 'readonly' was set, keep the old
  // value; the user probably used the ":view" command.  But don't
  // reset it, might have had a read error.
  if (orig_mode == curbuf->b_orig_mode) {
    curbuf->b_p_ro |= old_ro;
  }

  // Modelines must override settings done by autocommands.
  do_modelines(0);

  // restore curwin/curbuf and a few other things
  aucmd_restbuf(&aco);
  // Careful: autocommands may have made "buf" invalid!
}

void buf_store_file_info(buf_T *buf, FileInfo *file_info)
  FUNC_ATTR_NONNULL_ALL
{
  buf->b_mtime = file_info->stat.st_mtim.tv_sec;
  buf->b_mtime_ns = file_info->stat.st_mtim.tv_nsec;
  buf->b_orig_size = os_fileinfo_size(file_info);
  buf->b_orig_mode = (int)file_info->stat.st_mode;
}

/// Adjust the line with missing eol, used for the next write.
/// Used for do_filter(), when the input lines for the filter are deleted.
void write_lnum_adjust(linenr_T offset)
{
  if (curbuf->b_no_eol_lnum != 0) {     // only if there is a missing eol
    curbuf->b_no_eol_lnum += offset;
  }
}

#if defined(BACKSLASH_IN_FILENAME)
/// Convert all backslashes in fname to forward slashes in-place,
/// unless when it looks like a URL.
void forward_slash(char *fname)
{
  char *p;

  if (path_with_url(fname)) {
    return;
  }
  for (p = fname; *p != NUL; p++) {
    if (*p == '\\') {
      *p = '/';
    }
  }
}
#endif

/// Path to Nvim's own temp dir. Ends in a slash.
static char *vim_tempdir = NULL;
#ifdef HAVE_DIRFD_AND_FLOCK
DIR *vim_tempdir_dp = NULL;  ///< File descriptor of temp dir
#endif

/// Creates a directory for private use by this instance of Nvim, trying each of
/// `TEMP_DIR_NAMES` until one succeeds.
///
/// Only done once, the same directory is used for all temp files.
/// This method avoids security problems because of symlink attacks et al.
/// It's also a bit faster, because we only need to check for an existing
/// file when creating the directory and not for each temp file.
static void vim_mktempdir(void)
{
  static const char *temp_dirs[] = TEMP_DIR_NAMES;  // Try each of these until one succeeds.
  char tmp[TEMP_FILE_PATH_MAXLEN];
  char path[TEMP_FILE_PATH_MAXLEN];
  char user[40] = { 0 };

  (void)os_get_username(user, sizeof(user));
  // Usernames may contain slashes! #19240
  memchrsub(user, '/', '_', sizeof(user));
  memchrsub(user, '\\', '_', sizeof(user));

  // Make sure the umask doesn't remove the executable bit.
  // "repl" has been reported to use "0177".
  mode_t umask_save = umask(0077);
  for (size_t i = 0; i < ARRAY_SIZE(temp_dirs); i++) {
    // Expand environment variables, leave room for "/tmp/nvim.<user>/XXXXXX/999999999".
    expand_env((char *)temp_dirs[i], tmp, TEMP_FILE_PATH_MAXLEN - 64);
    if (!os_isdir(tmp)) {
      continue;
    }

    // "/tmp/" exists, now try to create "/tmp/nvim.<user>/".
    add_pathsep(tmp);

    const char *appname = get_appname();
    xstrlcat(tmp, appname, sizeof(tmp));
    xstrlcat(tmp, ".", sizeof(tmp));
    xstrlcat(tmp, user, sizeof(tmp));
    (void)os_mkdir(tmp, 0700);  // Always create, to avoid a race.
    bool owned = os_file_owned(tmp);
    bool isdir = os_isdir(tmp);
#ifdef UNIX
    int perm = os_getperm(tmp);  // XDG_RUNTIME_DIR must be owned by the user, mode 0700.
    bool valid = isdir && owned && 0700 == (perm & 0777);
#else
    bool valid = isdir && owned;  // TODO(justinmk): Windows ACL?
#endif
    if (valid) {
      add_pathsep(tmp);
    } else {
      if (!owned) {
        ELOG("tempdir root not owned by current user (%s): %s", user, tmp);
      } else if (!isdir) {
        ELOG("tempdir root not a directory: %s", tmp);
      }
#ifdef UNIX
      if (0700 != (perm & 0777)) {
        ELOG("tempdir root has invalid permissions (%o): %s", perm, tmp);
      }
#endif
      // If our "root" tempdir is invalid or fails, proceed without "<user>/".
      // Else user1 could break user2 by creating "/tmp/nvim.user2/".
      tmp[strlen(tmp) - strlen(user)] = '\0';
    }

    // Now try to create "/tmp/nvim.<user>/XXXXXX".
    xstrlcat(tmp, "XXXXXX", sizeof(tmp));  // mkdtemp "template", will be replaced with random alphanumeric chars.
    int r = os_mkdtemp(tmp, path);
    if (r != 0) {
      WLOG("tempdir create failed: %s: %s", os_strerror(r), tmp);
      continue;
    }

    if (vim_settempdir(path)) {
      // Successfully created and set temporary directory so stop trying.
      break;
    }
    // Couldn't set `vim_tempdir` to `path` so remove created directory.
    os_rmdir(path);
  }
  (void)umask(umask_save);
}

/// Core part of "readdir()" function.
/// Retrieve the list of files/directories of "path" into "gap".
///
/// @return  OK for success, FAIL for failure.
int readdir_core(garray_T *gap, const char *path, void *context, CheckItem checkitem)
  FUNC_ATTR_NONNULL_ARG(1, 2)
{
  ga_init(gap, (int)sizeof(char *), 20);

  Directory dir;
  if (!os_scandir(&dir, path)) {
    smsg(_(e_notopen), path);
    return FAIL;
  }

  for (;;) {
    const char *p = os_scandir_next(&dir);
    if (p == NULL) {
      break;
    }

    bool ignore = (p[0] == '.' && (p[1] == NUL || (p[1] == '.' && p[2] == NUL)));
    if (!ignore && checkitem != NULL) {
      varnumber_T r = checkitem(context, p);
      if (r < 0) {
        break;
      }
      if (r == 0) {
        ignore = true;
      }
    }

    if (!ignore) {
      ga_grow(gap, 1);
      ((char **)gap->ga_data)[gap->ga_len++] = xstrdup(p);
    }
  }

  os_closedir(&dir);

  if (gap->ga_len > 0) {
    sort_strings(gap->ga_data, gap->ga_len);
  }

  return OK;
}

/// Delete "name" and everything in it, recursively.
///
/// @param name  The path which should be deleted.
///
/// @return  0 for success, -1 if some file was not deleted.
int delete_recursive(const char *name)
  FUNC_ATTR_NONNULL_ALL
{
  int result = 0;

  if (os_isrealdir(name)) {
    char *exp = xstrdup(name);
    garray_T ga;
    if (readdir_core(&ga, exp, NULL, NULL) == OK) {
      for (int i = 0; i < ga.ga_len; i++) {
        vim_snprintf(NameBuff, MAXPATHL, "%s/%s", exp, ((char **)ga.ga_data)[i]);
        if (delete_recursive((const char *)NameBuff) != 0) {
          // Remember the failure but continue deleting any further
          // entries.
          result = -1;
        }
      }
      ga_clear_strings(&ga);
      if (os_rmdir(exp) != 0) {
        result = -1;
      }
    } else {
      result = -1;
    }
    xfree(exp);
  } else {
    // Delete symlink only.
    result = os_remove(name) == 0 ? 0 : -1;
  }

  return result;
}

#ifdef HAVE_DIRFD_AND_FLOCK
/// Open temporary directory and take file lock to prevent
/// to be auto-cleaned.
static void vim_opentempdir(void)
{
  if (vim_tempdir_dp != NULL) {
    return;
  }

  DIR *dp = opendir(vim_tempdir);
  if (dp == NULL) {
    return;
  }

  vim_tempdir_dp = dp;
  flock(dirfd(vim_tempdir_dp), LOCK_SH);
}

/// Close temporary directory - it automatically release file lock.
static void vim_closetempdir(void)
{
  if (vim_tempdir_dp == NULL) {
    return;
  }

  closedir(vim_tempdir_dp);
  vim_tempdir_dp = NULL;
}
#endif

/// Delete the temp directory and all files it contains.
void vim_deltempdir(void)
{
  if (vim_tempdir == NULL) {
    return;
  }

#ifdef HAVE_DIRFD_AND_FLOCK
  vim_closetempdir();
#endif
  // remove the trailing path separator
  path_tail(vim_tempdir)[-1] = NUL;
  delete_recursive(vim_tempdir);
  XFREE_CLEAR(vim_tempdir);
}

/// Gets path to Nvim's own temp dir (ending with slash).
///
/// Creates the directory on the first call.
char *vim_gettempdir(void)
{
  static int notfound = 0;
  bool exists = false;
  if (vim_tempdir == NULL || !(exists = os_isdir(vim_tempdir))) {
    if (vim_tempdir != NULL && !exists) {
      notfound++;
      if (notfound == 1) {
        ELOG("tempdir disappeared (antivirus or broken cleanup job?): %s", vim_tempdir);
      }
      if (notfound > 1) {
        msg_schedule_semsg("E5431: tempdir disappeared (%d times)", notfound);
      }
      XFREE_CLEAR(vim_tempdir);
    }
    vim_mktempdir();
  }
  return vim_tempdir;
}

/// Sets Nvim's own temporary directory name to `tempdir`. This directory must
/// already exist. Expands the name to a full path and put it in `vim_tempdir`.
/// This avoids that using `:cd` would confuse us.
///
/// @param tempdir must be no longer than MAXPATHL.
///
/// @return false if we run out of memory.
static bool vim_settempdir(char *tempdir)
{
  char *buf = verbose_try_malloc(MAXPATHL + 2);
  if (buf == NULL) {
    return false;
  }

  vim_FullName(tempdir, buf, MAXPATHL, false);
  add_pathsep(buf);
  vim_tempdir = xstrdup(buf);
#ifdef HAVE_DIRFD_AND_FLOCK
  vim_opentempdir();
#endif
  xfree(buf);
  return true;
}

/// Return a unique name that can be used for a temp file.
///
/// @note The temp file is NOT created.
///
/// @return  pointer to the temp file name or NULL if Nvim can't create
///          temporary directory for its own temporary files.
char *vim_tempname(void)
{
  // Temp filename counter.
  static uint64_t temp_count;

  char *tempdir = vim_gettempdir();
  if (!tempdir) {
    return NULL;
  }

  // There is no need to check if the file exists, because we own the directory
  // and nobody else creates a file in it.
  char templ[TEMP_FILE_PATH_MAXLEN];
  snprintf(templ, TEMP_FILE_PATH_MAXLEN, "%s%" PRIu64, tempdir, temp_count++);
  return xstrdup(templ);
}

/// Tries matching a filename with a "pattern" ("prog" is NULL), or use the
/// precompiled regprog "prog" ("pattern" is NULL).  That avoids calling
/// vim_regcomp() often.
///
/// Used for autocommands and 'wildignore'.
///
/// @param pattern pattern to match with
/// @param prog pre-compiled regprog or NULL
/// @param fname full path of the file name
/// @param sfname short file name or NULL
/// @param tail tail of the path
/// @param allow_dirs Allow matching with dir
///
/// @return true if there is a match, false otherwise
bool match_file_pat(char *pattern, regprog_T **prog, char *fname, char *sfname, char *tail,
                    int allow_dirs)
{
  regmatch_T regmatch;
  bool result = false;

  regmatch.rm_ic = p_fic;   // ignore case if 'fileignorecase' is set
  regmatch.regprog = prog != NULL ? *prog : vim_regcomp(pattern, RE_MAGIC);

  // Try for a match with the pattern with:
  // 1. the full file name, when the pattern has a '/'.
  // 2. the short file name, when the pattern has a '/'.
  // 3. the tail of the file name, when the pattern has no '/'.
  if (regmatch.regprog != NULL
      && ((allow_dirs
           && (vim_regexec(&regmatch, fname, (colnr_T)0)
               || (sfname != NULL
                   && vim_regexec(&regmatch, sfname, (colnr_T)0))))
          || (!allow_dirs && vim_regexec(&regmatch, tail, (colnr_T)0)))) {
    result = true;
  }

  if (prog != NULL) {
    *prog = regmatch.regprog;
  } else {
    vim_regfree(regmatch.regprog);
  }
  return result;
}

/// Check if a file matches with a pattern in "list".
/// "list" is a comma-separated list of patterns, like 'wildignore'.
/// "sfname" is the short file name or NULL, "ffname" the long file name.
///
/// @param list list of patterns to match
/// @param sfname short file name
/// @param ffname full file name
///
/// @return true if there was a match
bool match_file_list(char *list, char *sfname, char *ffname)
  FUNC_ATTR_WARN_UNUSED_RESULT FUNC_ATTR_NONNULL_ARG(1, 3)
{
  char *tail = path_tail(sfname);

  // try all patterns in 'wildignore'
  char *p = list;
  while (*p) {
    char buf[100];
    copy_option_part(&p, buf, ARRAY_SIZE(buf), ",");
    char allow_dirs;
    char *regpat = file_pat_to_reg_pat(buf, NULL, &allow_dirs, false);
    if (regpat == NULL) {
      break;
    }
    bool match = match_file_pat(regpat, NULL, ffname, sfname, tail, (int)allow_dirs);
    xfree(regpat);
    if (match) {
      return true;
    }
  }
  return false;
}

/// Convert the given pattern "pat" which has shell style wildcards in it, into
/// a regular expression, and return the result in allocated memory.  If there
/// is a directory path separator to be matched, then true is put in
/// allow_dirs, otherwise false is put there -- webb.
/// Handle backslashes before special characters, like "\*" and "\ ".
///
/// @param pat_end     first char after pattern or NULL
/// @param allow_dirs  Result passed back out in here
/// @param no_bslash   Don't use a backward slash as pathsep
///
/// @return            NULL on failure.
char *file_pat_to_reg_pat(const char *pat, const char *pat_end, char *allow_dirs, int no_bslash)
  FUNC_ATTR_NONNULL_ARG(1)
{
  if (allow_dirs != NULL) {
    *allow_dirs = false;
  }

  if (pat_end == NULL) {
    pat_end = pat + strlen(pat);
  }

  if (pat_end == pat) {
    return xstrdup("^$");
  }

  size_t size = 2;  // '^' at start, '$' at end.

  for (const char *p = pat; p < pat_end; p++) {
    switch (*p) {
    case '*':
    case '.':
    case ',':
    case '{':
    case '}':
    case '~':
      size += 2;                // extra backslash
      break;
#ifdef BACKSLASH_IN_FILENAME
    case '\\':
    case '/':
      size += 4;                // could become "[\/]"
      break;
#endif
    default:
      size++;
      break;
    }
  }
  char *reg_pat = xmalloc(size + 1);

  size_t i = 0;

  if (pat[0] == '*') {
    while (pat[0] == '*' && pat < pat_end - 1) {
      pat++;
    }
  } else {
    reg_pat[i++] = '^';
  }
  const char *endp = pat_end - 1;
  bool add_dollar = true;
  if (endp >= pat && *endp == '*') {
    while (endp - pat > 0 && *endp == '*') {
      endp--;
    }
    add_dollar = false;
  }
  int nested = 0;
  for (const char *p = pat; *p && nested >= 0 && p <= endp; p++) {
    switch (*p) {
    case '*':
      reg_pat[i++] = '.';
      reg_pat[i++] = '*';
      while (p[1] == '*') {  // "**" matches like "*"
        p++;
      }
      break;
    case '.':
    case '~':
      reg_pat[i++] = '\\';
      reg_pat[i++] = *p;
      break;
    case '?':
      reg_pat[i++] = '.';
      break;
    case '\\':
      if (p[1] == NUL) {
        break;
      }
#ifdef BACKSLASH_IN_FILENAME
      if (!no_bslash) {
        // translate:
        // "\x" to "\\x"  e.g., "dir\file"
        // "\*" to "\\.*" e.g., "dir\*.c"
        // "\?" to "\\."  e.g., "dir\??.c"
        // "\+" to "\+"   e.g., "fileX\+.c"
        if ((vim_isfilec((uint8_t)p[1]) || p[1] == '*' || p[1] == '?')
            && p[1] != '+') {
          reg_pat[i++] = '[';
          reg_pat[i++] = '\\';
          reg_pat[i++] = '/';
          reg_pat[i++] = ']';
          if (allow_dirs != NULL) {
            *allow_dirs = true;
          }
          break;
        }
      }
#endif
      // Undo escaping from ExpandEscape():
      // foo\?bar -> foo?bar
      // foo\%bar -> foo%bar
      // foo\,bar -> foo,bar
      // foo\ bar -> foo bar
      // Don't unescape \, * and others that are also special in a
      // regexp.
      // An escaped { must be unescaped since we use magic not
      // verymagic.  Use "\\\{n,m\}"" to get "\{n,m}".
      if (*++p == '?' && (!BACKSLASH_IN_FILENAME_BOOL || no_bslash)) {
        reg_pat[i++] = '?';
      } else if (*p == ',' || *p == '%' || *p == '#'
                 || ascii_isspace(*p) || *p == '{' || *p == '}') {
        reg_pat[i++] = *p;
      } else if (*p == '\\' && p[1] == '\\' && p[2] == '{') {
        reg_pat[i++] = '\\';
        reg_pat[i++] = '{';
        p += 2;
      } else {
        if (allow_dirs != NULL && vim_ispathsep(*p)
            && (!BACKSLASH_IN_FILENAME_BOOL || (!no_bslash || *p != '\\'))) {
          *allow_dirs = true;
        }
        reg_pat[i++] = '\\';
        reg_pat[i++] = *p;
      }
      break;
#ifdef BACKSLASH_IN_FILENAME
    case '/':
      reg_pat[i++] = '[';
      reg_pat[i++] = '\\';
      reg_pat[i++] = '/';
      reg_pat[i++] = ']';
      if (allow_dirs != NULL) {
        *allow_dirs = true;
      }
      break;
#endif
    case '{':
      reg_pat[i++] = '\\';
      reg_pat[i++] = '(';
      nested++;
      break;
    case '}':
      reg_pat[i++] = '\\';
      reg_pat[i++] = ')';
      nested--;
      break;
    case ',':
      if (nested) {
        reg_pat[i++] = '\\';
        reg_pat[i++] = '|';
      } else {
        reg_pat[i++] = ',';
      }
      break;
    default:
      if (allow_dirs != NULL && vim_ispathsep(*p)) {
        *allow_dirs = true;
      }
      reg_pat[i++] = *p;
      break;
    }
  }
  if (add_dollar) {
    reg_pat[i++] = '$';
  }
  reg_pat[i] = NUL;
  if (nested != 0) {
    if (nested < 0) {
      emsg(_("E219: Missing {."));
    } else {
      emsg(_("E220: Missing }."));
    }
    XFREE_CLEAR(reg_pat);
  }
  return reg_pat;
}

#if defined(EINTR)

/// Version of read() that retries when interrupted by EINTR (possibly
/// by a SIGWINCH).
long read_eintr(int fd, void *buf, size_t bufsize)
{
  long ret;

  for (;;) {
    ret = read(fd, buf, (unsigned int)bufsize);
    if (ret >= 0 || errno != EINTR) {
      break;
    }
  }
  return ret;
}

/// Version of write() that retries when interrupted by EINTR (possibly
/// by a SIGWINCH).
long write_eintr(int fd, void *buf, size_t bufsize)
{
  long ret = 0;

  // Repeat the write() so long it didn't fail, other than being interrupted
  // by a signal.
  while (ret < (long)bufsize) {
    long wlen = write(fd, (char *)buf + ret, (unsigned int)(bufsize - (size_t)ret));
    if (wlen < 0) {
      if (errno != EINTR) {
        break;
      }
    } else {
      ret += wlen;
    }
  }
  return ret;
}
#endif

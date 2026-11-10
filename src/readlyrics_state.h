// readlyrics_state.h -- the state of the READ LYRICS reader (App::rd_), kept apart from app.h so that a change here only rebuilds the
// files that include this header (app_readlyrics.cpp and the few that draw or open the reader) and not the whole program.
#pragma once
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include "lyrics_fetcher.h"
#include "lyrics_translate.h"

namespace muisc {

namespace fs = std::filesystem;

// A lyrics file found by the search over the lyrics folders (CTRL+s, "Add manually").
struct LyricsFileHit { fs::path path; std::string name; std::string lname; std::string folder; };

// READ LYRICS (CTRL+L): the files of the track, the lyrics shown, the translation, the overlays (translate, export, search, add manually).
struct ReadLyricsState {
    fs::path track;                                 // track the files belong to
    std::string title;                              // "Artist - Title" for the pane's border
    std::vector<LyricsVersion> versions;
    int sel = 0;                                    // chosen file
    int first = 0;                                  // first file shown on the one-line chooser (scrolls with the choice)
    std::vector<std::string> lines;                 // plain lyrics of the chosen file
    std::vector<LyricLine> timed;                   // the same lines with their times (timed[i].full_text == lines[i]): the highlight while the track plays
    double delay = 0.0;                             // the [offset:] of the chosen file (as LyricsResult::delay)
    bool follow = true;                             // the view scrolls along with the song (any scrolling turns it off, [f] on again)
    std::vector<TranslationInfo> tlist;             // the translations saved for the chosen file (TRANSLATION pane)
    int tsel = 0;                                   // the one chosen / shown
    int tfirst = 0;                                 // first entry shown on the one-line chooser
    int pane = 0;                                   // pane with the keys: 0 READ LYRICS, 1 TRANSLATION (TAB)
    bool confirm_del = false;                       // "delete this translation?" is asked
    std::vector<std::string> trans;                 // the chosen translation, one entry per line
    std::string from;                               // language the translation came from ("de")
    std::string to, svc;                        // language it is in ("en") and the service that made it ("google")
    bool opts_open = false;                         // the translate overlay ([t]): service, translate into, lyrics are in
    int opts_row = 0;
    bool opts_dirty = false;
    std::string opts_warn;                          // warning inside the translate overlay
    bool exp_open = false;                          // the export overlay ([e]): original only / + translation / + transliteration
    int exp_choice = 0;                             // what the export holds: bit 1 = the translation, bit 2 = the Latin spelling
    std::string exp_msg;                            // what the last export did (shown in the overlay, which stays open)
    bool exp_warn = false;
    int watch_epoch = -1;                           // the lyrics fetch the reader waits for after the re-fetch overlay (lyrics_.epoch at that time), -1 = none
    bool exp_wait = false;                          // the last ENTER only said "the kanji reader is still working": the next one exports anyway
    bool show = false;                              // translation visible
    bool same = false;                              // the lyrics already are in the target language
    bool busy = false;                              // a translation is running (it goes on when the overlay is closed)
    std::string job_to, job_svc;
    fs::path job_track;
    LyricsVersion job_version;
    int scroll = 0;
    std::string status;
    bool status_warn = false;                       // the status is a warning / error (drawn in red)
    std::mutex mutex;
    bool result_ready = false;
    LyricsTranslation result;
    bool search_open = false;
    std::string search_q;
    std::vector<LyricsFileHit> search_all;          // every lyrics file found when the search was opened
    std::vector<int> search_view;                   // indices into search_all that match the query
    size_t search_dirs = 0;                         // lyrics folders that were searched
    int search_sel = 0;
    int search_first = 0;
    LyricsFileHit opened_hit;                       // the reader shows this lyrics file, opened from the search (empty: it shows the lyrics of a track)
    bool add_open = false;
    int add_pane = 0;                               // 0 = lyrics file, 1 = track
    std::string add_fq, add_tq;                 // what is typed in each pane
    std::vector<LyricsFileHit> add_files;           // every lyrics file found when the overlay was opened
    std::vector<int> add_fview;                     // indices into add_files that match; -1 = the typed path
    LyricsFileHit add_typed;                        // a path typed into the file pane that names a lyrics file (path empty = none)
    std::vector<int> add_tview;                     // tracks that match: -1 = the current track, else an index into all_local_tracks_
    int add_fsel = 0, add_tsel = 0;
    size_t add_dirs = 0;
    fs::path add_cur;                               // the current track (the one the overlay was opened for), empty = none
    std::string add_cur_title;
    std::string add_msg;
    bool add_warn = false;
    LyricsFileHit add_lock_file;                    // the lyrics file locked in with ENTER (path empty = none yet)
    bool add_lock_auto = false;                     // the locked lyrics file is the manually added copy of the locked track (not picked by hand)
    bool add_lock_cur = true;                       // the locked track is the current track (the default)
    fs::path add_lock_track;                        // else the track locked in from the library list
    std::string add_lock_title;
    std::string add_lock_info;                      // what lyrics the locked track has now ("" = none)
    std::vector<std::string> add_thay;              // per library track: the lowercase text that is searched (artist - title, or the file name)
    bool add_thay_names = false;                    // ... which of the two it is
    bool add_tprev_ok = false, add_fprev_ok = false;   // the lists below hold the result of add*prev_q_ (for the flags in add_tprev_flags)
    std::string add_tprev_q, add_fprev_q;
    int add_tprev_flags = 0;
    bool add_nolyr = false;                         // CTRL+f: the track list holds only tracks without saved lyrics
    std::vector<char> add_has_lyr;                  // per library track: it has a saved lyrics copy (read when the filter is switched on)
    bool add_names = false;                         // the track list shows (and searches) file names instead of "folder - title"
    bool add_show_orig = true;                      // the locked-in pane names a manually added file by the file it came from (CTRL+o: by its copy)
    std::string add_orig_name;                      // ... that file's name, when the copy knows it
    bool add_manual = false;                        // the chosen track has lyrics that were added manually
    bool add_confirm = false;                       // "unassign?" is asked: DEL again confirms
    bool te_open = false;
    std::vector<std::string> te_lines;              // the translation being written: exactly one entry per lyrics line
    mutable int te_first = 0;                               // (kept for the drawing: the first line shown)
    int te_row = 0;                                 // the line with the caret (never a line that is empty in the original)
    std::string te_to, te_svc;                  // language it is in and the id it is saved under ("manual", "manual2" ...)
    std::string te_orig_to;                         // when a manual translation is edited: its language when opened (a changed language moves the file)
    bool te_editing = false;                        // an existing manual translation is changed (else a new "Manual Translation" is made)
    int te_focus = 0;                               // 0 the lines, 1 the language
    bool te_dirty = false, te_ask = false, te_new_ask = false, te_warn = false;
    std::string te_msg;
    int te_mark = -1;                               // SHIFT+up/down: the line where the marking began (-1: no lines marked)
    struct TeSnap { std::vector<std::string> lines; int row = 0; };
    std::vector<TeSnap> te_hist;                    // the last (up to 10) states of the translation, oldest first (CTRL+z / CTRL+y undo)
    std::vector<TeSnap> te_redo;                    // the states that were undone (CTRL+r / CTRL+SHIFT+z bring them back)
    int te_kind = 0;                                // what the last step was: 1 = typed characters of a run, 2 = anything else, 0 = none
    int te_kind_row = -1;
};

} // namespace muisc

// The Latin name lookup: artists written in Chinese characters are asked for on MusicBrainz in the background, one at a time.
#include "bg_tasks.h"
#include "app.h"

#include <memory>
#include <thread>

#include "console_log.h"
#include "translit.h"

namespace muisc {

void App::poll_name_lookup() {
    const auto now = std::chrono::steady_clock::now();
    if (name_job_ && name_job_->done.load()) {   // the answer of the running lookup
        const std::shared_ptr<NameLookupJob> job = name_job_;
        name_job_.reset();
        const NameLookupResult& r = job->result;
        if (r.ok) {
            translit_lookup_found(job->name, r.latin);
            status_line_ = "name lookup: " + job->name + " = " + r.latin;
            name_lookup_fails_ = 0;
            name_lookup_next_ = now + std::chrono::milliseconds(1500);
        } else if (r.notfound) {
            translit_lookup_none(job->name);
            name_lookup_fails_ = 0;
            name_lookup_next_ = now + std::chrono::milliseconds(1500);
        } else if (r.script_missing || r.python_missing) {
            name_lookup_broken_ = true;
            status_line_ = "name lookup: " + r.error;
        } else {   // not reachable, MusicBrainz busy ...: later (the pause grows up to ten minutes)
            translit_lookup_retry(job->name);
            name_lookup_fails_ = std::min(name_lookup_fails_ + 1, 5);
            name_lookup_next_ = now + std::chrono::seconds(std::min(600, 20 << name_lookup_fails_));
        }
    }
    if (!settings_.latin_lookup || name_job_ || name_lookup_broken_ || now < name_lookup_next_) return;
    const std::vector<std::string> names = translit_take_pending(1);
    if (names.empty()) return;
    auto job = std::make_shared<NameLookupJob>();
    job->name = names[0];
    name_job_ = job;
    const fs::path script = name_lookup_script_;
    bg_async([job, script]() {
        job->result = lookup_artist_name(script, job->name);
        job->done.store(true);
        return 0;
    });
}

void App::poll_kanji_reader() {
    const auto now = std::chrono::steady_clock::now();
    if (kanji_job_ && kanji_job_->done.load()) {
        const std::shared_ptr<KanjiJob> job = kanji_job_;
        kanji_job_.reset();
        const KanjiReadResult& r = job->result;
        if (r.ok) {
            for (const auto& [text, reading] : r.readings) translit_kanji_found(text, reading);
            for (const std::string& t : job->texts) translit_kanji_failed(t);   // (the ones without a reading are not asked again)
            kanji_fails_ = 0;
            if (!r.readings.empty()) status_line_ = "kanji readings: " + std::to_string(r.readings.size()) + " titles read (pykakasi)";
            kanji_next_ = now + std::chrono::milliseconds(1500);
        } else if (r.missing) {
            kanji_broken_ = true;
            for (const std::string& t : job->texts) translit_kanji_failed(t);
            status_line_ = "kanji readings: " + r.error;
        } else {   // the helper itself failed (no answer, an error): say why, ask again a few times, then give up for this session
            status_line_ = "kanji readings: " + r.error;
            ConsoleLog::instance().log_basic("kanji readings failed: " + r.error);
            if (++kanji_fails_ >= 3) {
                kanji_broken_ = true;
                for (const std::string& t : job->texts) translit_kanji_failed(t);
            } else {
                for (const std::string& t : job->texts) translit_kanji_retry(t);
                kanji_next_ = now + std::chrono::seconds(20 * kanji_fails_);
            }
        }
    }
    if (!settings_.latin_letters || kanji_job_ || kanji_broken_ || now < kanji_next_) return;
    std::vector<std::string> texts = translit_take_kanji_pending(60);
    if (texts.empty()) return;
    auto job = std::make_shared<KanjiJob>();
    job->texts = std::move(texts);
    kanji_job_ = job;
    const fs::path script = kanji_script_;
    bg_async([job, script]() {
        job->result = read_kanji(script, job->texts);
        job->done.store(true);
        return 0;
    });
}

}  // namespace muisc

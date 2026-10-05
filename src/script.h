#pragma once
// Scripted input for automated usability testing (--script file.txt).
//
// A script drives the real editor through synthetic mouse / keyboard /
// trackpad events, one line per step, so whole workflows can be replayed and
// checked without a person at the keyboard:
//
//   wait 5                     run 5 frames
//   move 400 300               put the mouse at (400, 300) (window pixels)
//   click 400 300 [right|middle]
//   dclick 400 300             double-click
//   drag 400 300 500 320 [frames] [right|middle] [C-|S-|A- modifiers, e.g. A-]
//   scroll 2 [x y]             vertical wheel notches (+ = away / up)
//   hscroll -1 [x y]           horizontal (two-finger sideways) scroll
//   pinch 1 [x y]              touchpad pinch (zoom)
//   key C-K                    press and release a key binding ("C-S-Z", "F5", "Minus")
//   hold Up 30                 hold a key for 30 frames
//   type bevel                 type text (search box, typed values)
//   cmd Bevel edges            run a command by name (as the command search would)
//   shot out.png               save a screenshot of the next frame
//   fuzz 2000 7                random clicks / drags / keys / scrolls for 2000 frames, seed 7;
//                              the scene is checked for corruption after every frame
//   fuzzcmd 500 3              500 random edit-mode selections + random commands (tools,
//                              last-operation tweaks, undo / redo ...), checked every step
//   check                      verify scene invariants now (fails the script if broken)
//   expect <text>              fail unless the status line contains <text>
//   print                      write a one-line scene summary to stdout
//   # comment
// The script's exit code is non-zero if any check or expectation failed.
#include "platform.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class Editor;

class ScriptPlayer {
public:
    bool load(const std::string& path, std::string& error);
    // Feeds this frame's synthetic events into `in` (after the OS events were
    // pumped). Returns false when the script has finished.
    bool step(Input& in, Editor& editor, int width, int height);
    // Called after the frame was rendered (screenshots).
    void afterFrame(Editor& editor, int width, int height);
    int failures() const { return failures_; }
    int frame() const { return frame_; }

private:
    using Action = std::function<void(Input&)>;
    void at(int frameOffset, Action a);  // schedule for a later frame (0 = this one)
    bool runLine(const std::string& line, Input& in, Editor& editor, int width, int height);
    void fuzzFrame(Input& in, int width, int height);
    void fail(const std::string& what);

    std::vector<std::string> lines_;
    size_t pc_ = 0;
    int wait_ = 0;
    int frame_ = 0;
    int failures_ = 0;
    std::deque<std::vector<Action>> queue_;  // queue_[k] runs k frames from now
    std::string pendingShot_;
    // fuzzing
    int fuzzLeft_ = 0;
    int fuzzCmdLeft_ = 0;
    int fuzzCmdCount_ = 0;
    uint32_t rng_ = 1;
    bool fuzzHeld_[3] = {};
    uint64_t fuzzEvents_ = 0;
    uint32_t next();
};

#include "script.h"

#include "editor.h"
#include "image_io.h"
#include "input_map.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {
int buttonFrom(const std::string& s) {
    if (s == "right") return MOUSE_RIGHT;
    if (s == "middle") return MOUSE_MIDDLE;
    return MOUSE_LEFT;
}
void setMods(Input& in, bool ctrl, bool shift, bool alt) {
    in.onKey(KEY_CONTROL, ctrl);
    in.onKey(KEY_SHIFT, shift);
    in.onKey(KEY_ALT, alt);
}
}  // namespace

bool ScriptPlayer::load(const std::string& path, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "Cannot open script " + path;
        return false;
    }
    char buf[1024];
    while (std::fgets(buf, sizeof buf, f)) {
        std::string l = buf;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos || l[a] == '#') continue;
        lines_.push_back(l.substr(a));
    }
    std::fclose(f);
    return true;
}

void ScriptPlayer::at(int k, Action a) {
    while ((int)queue_.size() <= k) queue_.emplace_back();
    queue_[k].push_back(std::move(a));
}

void ScriptPlayer::fail(const std::string& what) {
    ++failures_;
    std::printf("SCRIPT FAIL (frame %d): %s\n", frame_, what.c_str());
    std::fflush(stdout);
}

uint32_t ScriptPlayer::next() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

// One frame of random input: anything a confused user (or a cat on the
// keyboard) might do. Mouse buttons and keys are always released again so the
// editor never sees impossible states.
void ScriptPlayer::fuzzFrame(Input& in, int w, int h) {
    static const int keys[] = {'A', 'B', 'D', 'E', 'F', 'G', 'I', 'J', 'L', 'M', 'Q', 'R', 'S', 'U', 'W', 'X', 'Y', 'Z',
                               '1', '2', '3', '5', '7', '0', '=', '-', KEY_TAB, KEY_ENTER, KEY_ESCAPE, KEY_DELETE,
                               KEY_BACKSPACE, KEY_SPACE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
    const int nKeys = (int)(sizeof keys / sizeof keys[0]);
    // Release whatever was held last frame with some probability.
    for (int b = 0; b < 3; ++b)
        if (fuzzHeld_[b] && next() % 4 == 0) in.onMouseButton(b, false), fuzzHeld_[b] = false;
    if (fuzzKeyHeld_ && next() % 3 == 0) in.onKey(fuzzKeyHeld_, false), fuzzKeyHeld_ = 0;
    // Mouse motion: mostly small moves, sometimes jumps.
    float x = in.mouseX, y = in.mouseY;
    if (next() % 8 == 0) x = (float)(next() % (uint32_t)w), y = (float)(next() % (uint32_t)h);
    else x += (float)((int)(next() % 41) - 20), y += (float)((int)(next() % 41) - 20);
    in.onMouseMove(std::max(0.0f, std::min(x, w - 1.0f)), std::max(0.0f, std::min(y, h - 1.0f)));
    const uint32_t r = next() % 100;
    if (r < 10) {
        int b = next() % 10 < 7 ? MOUSE_LEFT : (next() % 2 ? MOUSE_RIGHT : MOUSE_MIDDLE);
        if (!fuzzHeld_[b]) in.onMouseButton(b, true), fuzzHeld_[b] = true;
    } else if (r < 22) {
        int k = keys[next() % nKeys];
        bool ctrl = next() % 5 == 0, shift = next() % 6 == 0, alt = next() % 8 == 0;
        setMods(in, ctrl, shift, alt);
        in.onKey(k, true);
        in.keyRepeat[k] = true;
        if (k >= 32 && k < 127 && !ctrl && !alt) in.text.push_back((char)(shift ? k : std::tolower(k)));
        // Release next frame (modifiers too).
        at(1, [k](Input& i) {
            i.onKey(k, false);
            setMods(i, false, false, false);
        });
    } else if (r < 27) {
        in.wheel += (float)((int)(next() % 5) - 2);
    } else if (r < 30) {
        in.wheelX += (float)((int)(next() % 3) - 1);
        if (next() % 3 == 0) in.pinch = true;
    } else if (r < 32) {
        // Type a number (typed values in modal tools, fields).
        in.text += std::to_string(next() % 100) + (next() % 2 ? "." : "");
    }
    ++fuzzEvents_;
}

bool ScriptPlayer::runLine(const std::string& line, Input& in, Editor& ed, int w, int h) {
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    std::string rest;
    std::getline(ss, rest);
    size_t a = rest.find_first_not_of(' ');
    rest = a == std::string::npos ? std::string() : rest.substr(a);
    std::istringstream args(rest);
    float x = in.mouseX, y = in.mouseY;
    if (cmd == "wait") {
        int n = 1;
        args >> n;
        wait_ = std::max(1, n);
        return false;  // consumes frames
    }
    if (cmd == "move") {
        args >> x >> y;
        in.onMouseMove(x, y);
        wait_ = 1;
        return false;
    }
    if (cmd == "click" || cmd == "dclick") {
        std::string b;
        args >> x >> y >> b;
        const int button = buttonFrom(b);
        in.onMouseMove(x, y);
        // The press comes a frame after the move so hover state is current.
        at(1, [button](Input& i) { i.onMouseButton(button, true); });
        at(2, [button](Input& i) { i.onMouseButton(button, false); });
        if (cmd == "dclick") {
            at(4, [button](Input& i) { i.onMouseButton(button, true); });
            at(5, [button](Input& i) { i.onMouseButton(button, false); });
            wait_ = 7;
        } else {
            wait_ = 4;
        }
        return false;
    }
    if (cmd == "drag") {
        float x2 = 0, y2 = 0;
        int frames = 10;
        std::string b, mods;
        args >> x >> y >> x2 >> y2 >> frames >> b >> mods;
        if (b.size() >= 2 && b[1] == '-') std::swap(b, mods);  // "drag ... 10 A-"
        frames = std::max(1, frames);
        const int button = buttonFrom(b);
        const bool c = mods.find("C-") != std::string::npos, s = mods.find("S-") != std::string::npos,
                   al = mods.find("A-") != std::string::npos;
        in.onMouseMove(x, y);
        at(1, [=](Input& i) {
            setMods(i, c, s, al);
            i.onMouseButton(button, true);
        });
        for (int k = 1; k <= frames; ++k) {
            float t = (float)k / frames;
            at(1 + k, [=](Input& i) { i.onMouseMove(x + (x2 - x) * t, y + (y2 - y) * t); });
        }
        at(frames + 2, [=](Input& i) {
            i.onMouseButton(button, false);
            setMods(i, false, false, false);
        });
        wait_ = frames + 4;
        return false;
    }
    if (cmd == "scroll" || cmd == "hscroll" || cmd == "pinch") {
        float n = 1;
        args >> n;
        if (args >> x >> y) in.onMouseMove(x, y);
        at(1, [=](Input& i) {
            if (cmd == "hscroll") i.wheelX += n;
            else i.wheel += n;
            if (cmd == "pinch") i.pinch = true;
        });
        wait_ = 3;
        return false;
    }
    if (cmd == "key" || cmd == "hold") {
        std::string name;
        int frames = 1;
        args >> name >> frames;
        input::Binding b;
        if (!input::parse(name, b) || !b.bound()) {
            fail("unknown key " + name);
            return true;
        }
        setMods(in, b.ctrl, b.shift, b.alt);
        at(1, [b](Input& i) {
            i.onKey(b.key, true);
            i.keyRepeat[b.key] = true;
            if (b.key >= 32 && b.key < 127 && !b.ctrl && !b.alt) {
                char ch = (char)b.key;
                if (b.shift && ch == '/') ch = '?';
                else if (!b.shift) ch = (char)std::tolower(ch);
                i.text.push_back(ch);
            }
        });
        const int hold = cmd == "hold" ? std::max(1, frames) : 1;
        at(1 + hold, [b](Input& i) {
            i.onKey(b.key, false);
            setMods(i, false, false, false);
        });
        wait_ = hold + 3;
        return false;
    }
    if (cmd == "type") {
        at(1, [rest](Input& i) { i.text += rest; });
        wait_ = 3;
        return false;
    }
    if (cmd == "cmd") {
        std::string matched;
        if (!ed.runNamedCommand(rest, matched)) fail("no command matches \"" + rest + "\"");
        wait_ = 2;
        return false;
    }
    if (cmd == "shot") {
        pendingShot_ = rest;
        wait_ = 1;
        return false;
    }
    if (cmd == "fuzz") {
        int n = 100;
        uint32_t seed = 1;
        args >> n >> seed;
        fuzzLeft_ = std::max(1, n);
        rng_ = seed ? seed * 2654435761u : 1;
        return false;
    }
    if (cmd == "check") {
        std::string err;
        if (!ed.checkInvariants(err)) fail("invariant: " + err);
        return true;
    }
    if (cmd == "expect") {
        if (ed.statusText().find(rest) == std::string::npos)
            fail("expected status containing \"" + rest + "\", got \"" + ed.statusText() + "\"");
        return true;
    }
    if (cmd == "print") {
        std::printf("[frame %d] %s\n", frame_, ed.summary().c_str());
        std::fflush(stdout);
        return true;
    }
    fail("unknown script command: " + line);
    (void)w, (void)h;
    return true;
}

bool ScriptPlayer::step(Input& in, Editor& ed, int w, int h) {
    ++frame_;
    if (!queue_.empty()) {
        for (Action& a : queue_.front()) a(in);
        queue_.pop_front();
    }
    if (fuzzLeft_ > 0) {
        fuzzFrame(in, w, h);
        --fuzzLeft_;
        std::string err;
        if (!ed.checkInvariants(err)) {
            fail("fuzz frame broke the scene: " + err);
            fuzzLeft_ = 0;
        }
        if (fuzzLeft_ == 0) {
            // Let go of everything and close any modal state.
            for (int b = 0; b < 3; ++b)
                if (fuzzHeld_[b]) in.onMouseButton(b, false), fuzzHeld_[b] = false;
            if (fuzzKeyHeld_) in.onKey(fuzzKeyHeld_, false), fuzzKeyHeld_ = 0;
            setMods(in, false, false, false);
            std::printf("fuzz: %llu frames of random input, scene: %s\n", (unsigned long long)fuzzEvents_,
                        ed.summary().c_str());
            std::fflush(stdout);
        }
        return true;
    }
    if (wait_ > 0 && --wait_ > 0) return true;
    while (pc_ < lines_.size()) {
        if (!runLine(lines_[pc_++], in, ed, w, h)) return true;
        if (fuzzLeft_ > 0) return true;
    }
    return wait_ > 0 || !queue_.empty();
}

void ScriptPlayer::afterFrame(Editor& ed, int w, int h) {
    if (pendingShot_.empty()) return;
    std::vector<uint8_t> px;
    ed.readPixels(w, h, px);
    if (!writePNG(pendingShot_, w, h, px)) fail("could not write " + pendingShot_);
    else std::printf("shot %s\n", pendingShot_.c_str());
    pendingShot_.clear();
}

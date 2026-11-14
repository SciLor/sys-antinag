// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 SciLor
#define TESLA_INIT_IMPL // only define this in the main file
#define STBTT_STATIC
#include <tesla.hpp>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr auto CONFIG_PATH = "/config/sys-antinag/config.ini";
constexpr auto STATUS_PATH = "/config/sys-antinag/status.txt";
constexpr auto TEST_PATH = "/config/sys-antinag/test.txt";
constexpr auto TEST_REQ_PATH = "/config/sys-antinag/test.req";

// libtesla does not mount the SD card for stdio; AntinagOverlay::initServices() does (device "sdmc:")
FILE* open_sd(const char* path, const char* mode) {
    return fopen((std::string("sdmc:") + path).c_str(), mode);
}

std::string read_key(const char* path, const char* key, const char* fallback) {
    std::string value = fallback;
    if (auto f = open_sd(path, "r")) {
        char ln[200]; size_t n = strlen(key);
        while (fgets(ln, sizeof ln, f)) {
            if (!strncmp(ln, key, n) && ln[n] == '=') { value = ln + n + 1; while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back(); }
        }
        fclose(f);
    }
    return value;
}

struct Config {
    bool enabled{true};
    bool debug{false};
    bool show_debug{false};   // hides the Developer section; only set by hand in config.ini

    void load() {
        if (auto f = open_sd(CONFIG_PATH, "r")) {
            char ln[128]; int v;
            while (fgets(ln, sizeof ln, f)) {
                if (sscanf(ln, " enabled = %d", &v) == 1) enabled = v != 0;
                else if (sscanf(ln, " debug = %d", &v) == 1) debug = v != 0;
                else if (sscanf(ln, " show_debug = %d", &v) == 1) show_debug = v != 0;
            }
            fclose(f);
        }
    }

    void save() const {
        if (auto f = open_sd(CONFIG_PATH, "w")) {
            fprintf(f, "[antinag]\nenabled=%d\ndebug=%d\nshow_debug=%d\n", enabled, debug, show_debug);
            fclose(f);
        }
    }
};

// the sysmodule writes a long sentence; the overlay only has room for a few words
std::string short_verdict(const std::string& v) {
    if (v.rfind("OK", 0) == 0) return "works";
    if (v.rfind("FAIL", 0) == 0) return "FAILED";
    if (v.rfind("inconclusive", 0) == 0) return "turn WiFi off";
    return "error";
}

#ifndef ANTINAG_VERSION
#define ANTINAG_VERSION "dev"
#endif

// overlay version; if the sysmodule on the SD card is a different build, show both
std::string version_text() {
    std::string sys = read_key(STATUS_PATH, "version", "");
    if (sys.empty() || sys == ANTINAG_VERSION) return ANTINAG_VERSION;
    return std::string("ovl ") + ANTINAG_VERSION + " / sys " + sys;
}

std::string read_state() { return read_key(STATUS_PATH, "state", "sysmodule not running"); }

class GuiMain final : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        config.load();
        auto frame = new tsl::elm::OverlayFrame("SciLor's Antinag", "airplane-mode popup remover - by SciLor");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Options"));
        auto enabled = new tsl::elm::ToggleListItem("Block airplane popup", config.enabled);
        enabled->setStateChangedListener([this](bool v) { config.enabled = v; config.save(); });
        list->addItem(enabled);

        list->addItem(new tsl::elm::CategoryHeader("Status"));
        status = new tsl::elm::ListItem("nifm patch", read_state());
        list->addItem(status);
        list->addItem(new tsl::elm::ListItem("Version", version_text()));

        list->addItem(new tsl::elm::CategoryHeader("Test"));
        auto test = new tsl::elm::ListItem("Run test (WiFi off)");
        test->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                remove((std::string("sdmc:") + TEST_PATH).c_str());   // forget the last verdict
                if (auto f = open_sd(TEST_REQ_PATH, "w")) { fputs("1", f); fclose(f); }
                waiting_for_test = true; result_item->setValue("running...");
                return true;
            }
            return false;
        });
        list->addItem(test);
        result_item = new tsl::elm::ListItem("Last test", "-");   // own row, short text only
        list->addItem(result_item);

        if (config.show_debug) {   // config.ini: show_debug=1
            list->addItem(new tsl::elm::CategoryHeader("Developer"));
            auto debug = new tsl::elm::ToggleListItem("Debug (peek/poke files)", config.debug);
            debug->setStateChangedListener([this](bool v) { config.debug = v; config.save(); });
            list->addItem(debug);
        }

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (++frames % 30) return;
        if (status) status->setValue(read_state());
        if (waiting_for_test && result_item) {
            auto v = read_key(TEST_PATH, "verdict", "");
            if (!v.empty()) { result_item->setValue(short_verdict(v)); waiting_for_test = false; }
        }
    }

private:
    Config config;
    tsl::elm::ListItem* status{};
    tsl::elm::ListItem* result_item{};
    bool waiting_for_test{};
    unsigned frames{};
};

class AntinagOverlay final : public tsl::Overlay {
public:
    void initServices() override { fsdevMountSdmc(); }
    void exitServices() override { fsdevUnmountDevice("sdmc"); }

    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<GuiMain>(); }
};

} // namespace

int main(int argc, char** argv) {
    return tsl::loop<AntinagOverlay>(argc, argv);
}

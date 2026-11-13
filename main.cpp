// Bufferium V1.00 by rippd - companion for the predictive input buffer (ai.predictivebuffer,
// build stamp 0x6ad5519c). Use it together with FWCmain-quiet.geode.
//   Right Ctrl  show / hide the panel      F6  (the buffer's own key) makes the panel peek in
//
// The buffer keeps its strength in one static struct. This mod finds that struct inside the
// loaded DLL, checks the DLL is exactly the build it was written for, and only then reads or
// writes it. If the check fails nothing is touched and the panel says "NOT FOUND".
#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <Windows.h>

using namespace geode::prelude;

namespace {

// ---------------------------------------------------------------------------
// Link to the buffer's state (offsets found by reading its code)
//   +0x18 int   stage 0..4 (4 = off)   +0x1c u8   buffer enabled
//   +0x20 int   ticks (all modes)      +0x24 int  ticks (wave)
//   +0x28 int   pending-press timer (-1 = none)   +0x2d u8  pending flag
// ---------------------------------------------------------------------------
constexpr uint32_t kTimeStamp = 0x6ad5519c;
constexpr uint32_t kImageSize = 0x891000;
constexpr size_t   kCycleFn   = 0x75370;     // the buffer's own "next strength" function
constexpr size_t   kStateRva  = 0x7d4000;
constexpr int      kOff       = 4;

uint8_t* g_st = nullptr;

bool linkState() {
    if (g_st) return true;
    HMODULE h = GetModuleHandleA("ai.predictivebuffer.dll");
    if (!h) return false;
    auto base = reinterpret_cast<uint8_t*>(h);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kTimeStamp) return false;
    if (nt->OptionalHeader.SizeOfImage != kImageSize) return false;
    static const uint8_t sig[] = {0x44, 0x8b, 0x41, 0x18, 0xb8, 0x67, 0x66, 0x66,
                                  0x66, 0x41, 0xff, 0xc0, 0xc7, 0x41, 0x28, 0xff};
    if (std::memcmp(base + kCycleFn, sig, sizeof(sig)) != 0) return false;
    auto st = base + kStateRva;
    int stage = *reinterpret_cast<int*>(st + 0x18);
    if (stage < 0 || stage > 4) return false;
    g_st = st;
    return true;
}

int readStage() {
    if (!linkState()) return -1;
    int s = *reinterpret_cast<int*>(g_st + 0x18);
    return (s >= 0 && s <= 4) ? s : -1;
}

// does exactly what the buffer's own F6 handler does for each stage
void writeStage(int s) {
    if (!linkState() || s < 0 || s > 4) return;
    static const int all[5]  = {2, 4, 8, 16, 0};
    static const int wave[5] = {2, 4, 8, 8, 0};
    *reinterpret_cast<int*>(g_st + 0x28) = -1;
    *reinterpret_cast<uint8_t*>(g_st + 0x2d) = 0;
    *reinterpret_cast<int*>(g_st + 0x20) = all[s];
    *reinterpret_cast<int*>(g_st + 0x24) = wave[s];
    *reinterpret_cast<uint8_t*>(g_st + 0x1c) = s < 4 ? 1 : 0;
    *reinterpret_cast<int*>(g_st + 0x18) = s;
}

void applySavedOnce() {
    static bool done = false;
    if (done || !linkState()) return;
    done = true;
    int64_t s = Mod::get()->getSavedValue<int64_t>("stage", -1);
    if (s >= 0 && s <= 4) writeStage((int)s);
}

// ---------------------------------------------------------------------------
// Live console (only exists when the user opens it)
// ---------------------------------------------------------------------------
std::atomic<bool> g_userConsole{false};

BOOL WINAPI ctrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            return TRUE;                 // swallow: Ctrl+C in the console must not kill the game
        case CTRL_CLOSE_EVENT:
            g_userConsole = false;
            FreeConsole();               // detach first, so closing the window can't take the game with it
            writeStage(kOff);            // ...and switch the buffer off instead
            return TRUE;
        default:
            return FALSE;
    }
}

void openConsole() {
    if (GetConsoleWindow()) { g_userConsole = true; return; }
    if (!AllocConsole()) return;
    g_userConsole = true;
    SetConsoleTitleA("Bufferium - Live Console");
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);   // the buffer prints through the shared C runtime
    freopen_s(&f, "CONOUT$", "w", stderr);
    static bool handlerSet = false;
    if (!handlerSet) { SetConsoleCtrlHandler(ctrlHandler, TRUE); handlerSet = true; }
    std::printf("[Bufferium V1.00] Live console open.\n");
    std::printf("[Bufferium V1.00] Closing this window switches the buffer OFF (the game keeps running).\n");
    std::fflush(stdout);
}

void closeConsole() {
    g_userConsole = false;
    if (GetConsoleWindow()) FreeConsole();
}

// safety net if someone still runs the unpatched original: close ITS console only
void closeOriginalConsole() {
    if (g_userConsole || !GetConsoleWindow()) return;
    char t[256] = {};
    GetConsoleTitleA(t, sizeof(t));
    if (std::strstr(t, "Predictive Input Buffer")) FreeConsole();
}

// Right Ctrl. Depending on the build GD reports it either as KEY_RightControl or as the generic
// KEY_Control, so for the generic one we ask Windows which physical Ctrl is actually down.
bool isRightCtrl(enumKeyCodes key) {
    if (key == KEY_RightControl) return true;
    if (key == KEY_Control) return (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
    return false;
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------
struct Stage {
    char const* chip;
    int all;
    int wave;
    ccColor3B color;
};

constexpr int kChips = 4;
Stage const kStages[kChips] = {
    {"2/2",  2,  2,  {90, 220, 130}},
    {"4/4",  4,  4,  {170, 225, 80}},
    {"8/8",  8,  8,  {255, 190, 60}},
    {"16/8", 16, 8,  {255, 110, 70}},
};
ccColor3B const kOffColor = {140, 145, 160};

void playSfx(char const* name, float pitch = 1.f, float volume = 1.f) {
    FMODAudioEngine::sharedEngine()->playEffect(name, pitch, 0.f, volume);
}

CCNode* makePill(char const* text, float w, float h, ccColor3B col) {
    auto n = CCNode::create();
    n->setContentSize({w, h});
    auto bg = CCScale9Sprite::create("square02b_001.png");
    bg->setContentSize({w, h});
    bg->setPosition({w / 2.f, h / 2.f});
    bg->setColor(col);
    n->addChild(bg, 0);
    auto l = CCLabelBMFont::create(text, "bigFont.fnt");
    l->setScale(.33f);
    l->setPosition({w / 2.f, h / 2.f});
    n->addChild(l, 1);
    return n;
}

class BufferHud : public CCNode {
protected:
    static constexpr float W = 288.f;
    static constexpr float H = 184.f;
    static constexpr float kChipH = 30.f;
    static constexpr float kGap = 6.f;
    static constexpr float kMargin = 14.f;

    CCNode* m_panel = nullptr;
    CCLayerColor* m_accent = nullptr;
    CCScale9Sprite* m_indicator = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_detail = nullptr;
    CCMenuItemToggler* m_check = nullptr;
    std::array<CCLabelBMFont*, kChips> m_chips{};
    bool m_closing = false;
    bool m_peek = false;
    bool m_busy = false;
    int m_shown = -1;
    int m_lastOn = 0;
    float m_cw = 0.f;

    CCPoint shownPos() {
        auto win = CCDirector::get()->getWinSize();
        return {kMargin, win.height * .5f};
    }
    CCPoint hiddenPos() {
        auto p = shownPos();
        return {-W - 24.f, p.y};
    }
    CCPoint chipPos(int i) {
        return {16.f + m_cw / 2.f + i * (m_cw + kGap), H - 100.f};
    }

    bool setup() {
        if (!CCNode::init()) return false;
        m_cw = (W - 32.f - kGap * (kChips - 1)) / kChips;
        int64_t lo = Mod::get()->getSavedValue<int64_t>("last-on", 0);
        m_lastOn = (lo >= 0 && lo < kChips) ? (int)lo : 0;

        m_panel = CCNode::create();
        m_panel->setContentSize({W, H});
        m_panel->setAnchorPoint({0.f, .5f});
        this->addChild(m_panel);

        // soft shadow + dark body
        auto shadow = CCScale9Sprite::create("square02b_001.png");
        shadow->setContentSize({W + 8.f, H + 8.f});
        shadow->setPosition({W / 2.f, H / 2.f - 3.f});
        shadow->setColor({0, 0, 0});
        shadow->setOpacity(70);
        m_panel->addChild(shadow, 0);

        auto bg = CCScale9Sprite::create("square02b_001.png");
        bg->setContentSize({W, H});
        bg->setPosition({W / 2.f, H / 2.f});
        bg->setColor({16, 18, 26});
        bg->setOpacity(215);
        m_panel->addChild(bg, 1);

        // accent strip on the left edge (changes color with strength)
        m_accent = CCLayerColor::create(ccc4(255, 255, 255, 255), 3.f, H - 28.f);
        m_accent->setPosition({0.f, 14.f});
        m_panel->addChild(m_accent, 2);

        // header
        auto title = CCLabelBMFont::create("Bufferium .V1.00", "bigFont.fnt");
        title->setScale(.46f);
        title->setAnchorPoint({0.f, .5f});
        title->setPosition({16.f, H - 20.f});
        title->setColor({235, 238, 248});
        m_panel->addChild(title, 3);

        m_status = CCLabelBMFont::create("", "bigFont.fnt");
        m_status->setScale(.32f);
        m_status->setAnchorPoint({1.f, .5f});
        m_status->setPosition({W - 16.f, H - 20.f});
        m_panel->addChild(m_status, 3);

        auto divider = CCLayerColor::create(ccc4(255, 255, 255, 26), W - 32.f, 1.f);
        divider->setPosition({16.f, H - 36.f});
        m_panel->addChild(divider, 3);

        // sliding selection pill (behind the chip labels)
        m_indicator = CCScale9Sprite::create("square02b_001.png");
        m_indicator->setContentSize({m_cw, kChipH});
        m_indicator->setPosition(chipPos(m_lastOn));
        m_panel->addChild(m_indicator, 3);

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setContentSize({W, H});
        m_panel->addChild(menu, 5);

        // on / off checkbox
        m_check = CCMenuItemExt::createTogglerWithStandardSprites(.55f, [this](CCMenuItemToggler* t) {
            if (m_busy) return;
            if (!linkState()) {
                this->shake();
                playSfx("quitSound_01.ogg", .6f, .6f);
                this->scheduleOnce(schedule_selector(BufferHud::resync), 0.f);
                return;
            }
            bool on = !t->isToggled();
            this->select(on ? m_lastOn : kOff, true, true);
        });
        m_check->setPosition({32.f, H - 62.f});
        menu->addChild(m_check);

        auto cl = CCLabelBMFont::create("Buffer enabled", "bigFont.fnt");
        cl->setScale(.4f);
        cl->setAnchorPoint({0.f, .5f});
        cl->setPosition({54.f, H - 62.f});
        cl->setColor({205, 210, 226});
        m_panel->addChild(cl, 3);

        // strength chips
        for (int i = 0; i < kChips; i++) {
            auto lbl = CCLabelBMFont::create(kStages[i].chip, "bigFont.fnt");
            lbl->setScale(.42f);
            lbl->setPosition({m_cw / 2.f, kChipH / 2.f});
            m_chips[i] = lbl;

            auto hit = CCNode::create();
            hit->setContentSize({m_cw, kChipH});
            hit->addChild(lbl);

            auto item = CCMenuItemExt::createSpriteExtra(hit, [this, i](CCObject*) {
                if (!linkState()) {
                    this->shake();
                    playSfx("quitSound_01.ogg", .6f, .6f);
                    return;
                }
                if (i != m_shown) this->select(i, true, true);
            });
            item->setPosition(chipPos(i));
            menu->addChild(item);
        }

        m_detail = CCLabelBMFont::create("", "chatFont.fnt");
        m_detail->setScale(.62f);
        m_detail->setColor({170, 178, 200});
        m_detail->setPosition({W / 2.f, 58.f});
        m_panel->addChild(m_detail, 3);

        // live console buttons
        float bw = (W - 32.f - 8.f) / 2.f;
        auto openPill = makePill("OPEN CONSOLE", bw, 26.f, {40, 48, 74});
        auto openItem = CCMenuItemExt::createSpriteExtra(openPill, [](CCObject*) {
            openConsole();
            playSfx("chestClick.ogg", 1.1f, .6f);
        });
        openItem->setPosition({16.f + bw / 2.f, 34.f});
        menu->addChild(openItem);

        auto closePill = makePill("CLOSE CONSOLE", bw, 26.f, {74, 40, 48});
        auto closeItem = CCMenuItemExt::createSpriteExtra(closePill, [](CCObject*) {
            closeConsole();
            playSfx("quitSound_01.ogg", 1.f, .5f);
        });
        closeItem->setPosition({W - 16.f - bw / 2.f, 34.f});
        menu->addChild(closeItem);

        auto hint = CCLabelBMFont::create("F6 next   |   Right Ctrl hide", "chatFont.fnt");
        hint->setScale(.5f);
        hint->setColor({120, 126, 146});
        hint->setPosition({W / 2.f, 10.f});
        m_panel->addChild(hint, 3);

        int cur = readStage();
        if (cur < 0) cur = (int)Mod::get()->getSavedValue<int64_t>("stage", 0);
        if (cur < 0 || cur > kOff) cur = 0;
        this->select(cur, false, false);

        // follow changes made by the buffer's own F6 key (or by closing the console)
        this->schedule(schedule_selector(BufferHud::poll), .1f);
        return true;
    }

    void poll(float) {
        int cur = readStage();
        if (cur >= 0 && cur != m_shown) {
            this->select(cur, true, false);
            if (m_peek) this->armPeekTimer();
        }
    }

    void resync(float) {
        int cur = readStage();
        this->select(cur >= 0 ? cur : (m_shown >= 0 ? m_shown : kOff), false, false);
    }

    void select(int i, bool animate, bool write) {
        if (write) writeStage(i);
        m_shown = i;
        bool linked = linkState();
        bool on = i < kOff;
        if (on) m_lastOn = i;
        if (linked) {
            Mod::get()->setSavedValue<int64_t>("stage", (int64_t)i);
            if (on) Mod::get()->setSavedValue<int64_t>("last-on", (int64_t)i);
        }

        ccColor3B col = on ? kStages[i].color : kOffColor;
        GLubyte op = on ? 255 : 60;

        auto target = chipPos(m_lastOn);
        m_indicator->stopAllActions();
        if (animate) {
            m_indicator->runAction(CCEaseExponentialOut::create(CCMoveTo::create(.28f, target)));
            m_indicator->runAction(CCTintTo::create(.28f, col.r, col.g, col.b));
            m_indicator->runAction(CCFadeTo::create(.2f, op));
        } else {
            m_indicator->setPosition(target);
            m_indicator->setColor(col);
            m_indicator->setOpacity(op);
        }
        m_accent->setColor(col);

        m_busy = true;
        m_check->toggle(on);
        m_busy = false;

        if (!linked) {
            m_status->setColor({255, 90, 90});
            m_status->setString("NOT FOUND");
        } else {
            m_status->setColor(col);
            m_status->setString(on ? "ACTIVE" : "OFF");
        }

        for (int k = 0; k < kChips; k++) {
            bool sel = on && k == i;
            m_chips[k]->stopAllActions();
            if (!on) m_chips[k]->setColor({110, 115, 132});
            else m_chips[k]->setColor(sel ? ccColor3B{16, 18, 26} : ccColor3B{170, 176, 196});
            if (animate && sel) {
                m_chips[k]->runAction(CCSequence::create(
                    CCScaleTo::create(.07f, .30f),
                    CCEaseBackOut::create(CCScaleTo::create(.26f, .46f)),
                    nullptr));
            } else {
                m_chips[k]->setScale(sel ? .46f : .42f);
            }
        }

        if (!linked) {
            m_detail->setString("buffer mod not detected (wrong build?)");
        } else if (!on) {
            m_detail->setString("buffer off - 100% raw input");
        } else {
            auto const& st = kStages[i];
            m_detail->setString(fmt::format("ALL {} ticks ({:g}f)    WAVE {} ticks ({:g}f)",
                st.all, st.all / 4.f, st.wave, st.wave / 4.f).c_str());
        }

        if (animate) {
            // pitch climbs with strength; switching off gets the "close" sound
            if (!on) playSfx("quitSound_01.ogg", 1.f, .5f);
            else playSfx("chestClick.ogg", .8f + .2f * i, .8f);
        }
    }

    void shake() {
        m_panel->runAction(CCSequence::create(
            CCMoveBy::create(.04f, {-6.f, 0.f}),
            CCMoveBy::create(.08f, {12.f, 0.f}),
            CCMoveBy::create(.08f, {-12.f, 0.f}),
            CCMoveBy::create(.04f, {6.f, 0.f}),
            nullptr));
    }

    void animateIn() {
        m_panel->setPosition(hiddenPos());
        m_panel->setScale(.9f);
        m_panel->runAction(CCSpawn::create(
            CCEaseBackOut::create(CCMoveTo::create(.42f, shownPos())),
            CCEaseExponentialOut::create(CCScaleTo::create(.42f, 1.f)),
            nullptr));
    }

    void animateOut() {
        if (m_closing) return;
        m_closing = true;
        this->unschedule(schedule_selector(BufferHud::autoHide));
        m_panel->stopAllActions();
        m_panel->runAction(CCSpawn::create(
            CCEaseBackIn::create(CCMoveTo::create(.3f, hiddenPos())),
            CCScaleTo::create(.3f, .92f),
            nullptr));
        this->scheduleOnce(schedule_selector(BufferHud::finishHide), .32f);
    }

    void autoHide(float) { this->animateOut(); }
    void finishHide(float) { this->removeFromParentAndCleanup(true); }

    void armPeekTimer() {
        this->unschedule(schedule_selector(BufferHud::autoHide));
        this->scheduleOnce(schedule_selector(BufferHud::autoHide), 1.8f);
    }

public:
    static inline BufferHud* s_inst = nullptr;

    ~BufferHud() override {
        if (s_inst == this) s_inst = nullptr;
    }

    static BufferHud* open(bool peek) {
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) return nullptr;
        auto ret = new BufferHud();
        if (!ret->setup()) { delete ret; return nullptr; }
        ret->autorelease();
        ret->m_peek = peek;
        scene->addChild(ret, 99999);
        s_inst = ret;
        ret->animateIn();
        if (!peek) playSfx("chestClick.ogg", 1.25f, .6f);
        if (peek) ret->armPeekTimer();
        return ret;
    }

    static bool visible() { return s_inst && !s_inst->m_closing; }

    // Right Ctrl
    static void toggle() {
        if (visible()) {
            playSfx("quitSound_01.ogg", 1.f, .5f);
            s_inst->animateOut();
        } else {
            open(false);
        }
    }

    // F6 was pressed (the buffer handles the actual change; we just show it)
    static void peek() {
        if (visible()) {
            if (s_inst->m_peek) s_inst->armPeekTimer();
        } else {
            open(true);
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// hooks
// ---------------------------------------------------------------------------
class $modify(BFKeys, CCKeyboardDispatcher) {
    // run before other mods' keyboard hooks, so nobody can swallow the key before we see it
    static void onModify(auto& self) {
        auto r = self.setHookPriority("CCKeyboardDispatcher::dispatchKeyboardMSG", -1000);
        if (r.isErr()) log::warn("Bufferium: could not set keyboard hook priority");
    }

    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        if (down && !repeat) {
            bool inLevel = PlayLayer::get() != nullptr;
            if (isRightCtrl(key)) {
                if (inLevel) {
                    log::info("Bufferium: Right Ctrl pressed, toggling panel");
                    BufferHud::toggle();
                } else {
                    log::info("Bufferium: Right Ctrl pressed but PlayLayer::get() is null");
                }
                // not consumed: Ctrl is a modifier key, let the game see it too
            }
            if (key == KEY_F6 && inLevel) BufferHud::peek();   // not consumed: the buffer still sees it
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};

class $modify(BFMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        closeOriginalConsole();
        static bool shown = false;
        if (!shown) {
            shown = true;
            log::info("Bufferium v1.0.2 hooks are running (MenuLayer)");
            Notification::create("Bufferium v1.0.2 loaded", NotificationIcon::Success, 3.f)->show();
        }
        return true;
    }
};

class $modify(BFPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        closeOriginalConsole();
        applySavedOnce();
        return true;
    }
};

// fallback: a button on the pause screen, so the panel can be opened without any key
class $modify(BFPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        log::info("Bufferium: adding pause button");
        auto pill = makePill("BUFFER", 64.f, 26.f, {40, 48, 74});
        auto item = CCMenuItemExt::createSpriteExtra(pill, [](CCObject*) {
            BufferHud::toggle();
        });
        auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("right-button-menu"));
        if (menu) {
            menu->addChild(item);
            menu->updateLayout();
        } else {
            auto win = CCDirector::get()->getWinSize();
            auto own = CCMenu::create();
            own->setPosition({win.width - 50.f, 40.f});
            own->addChild(item);
            this->addChild(own);
        }
    }
};

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

using namespace geode::prelude;

// ── Game Modes ──

enum class GameMode { Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swing };

static GameMode getMode(PlayerObject* p) {
    if (p->m_isShip) return GameMode::Ship;
    if (p->m_isBall) return GameMode::Ball;
    if (p->m_isBird) return GameMode::UFO;
    if (p->m_isDart) return GameMode::Wave;
    if (p->m_isRobot) return GameMode::Robot;
    if (p->m_isSpider) return GameMode::Spider;
    if (p->m_isSwing) return GameMode::Swing;
    return GameMode::Cube;
}

static const char* modeName(GameMode m) {
    switch (m) {
        case GameMode::Cube: return "Cube";
        case GameMode::Ship: return "Ship";
        case GameMode::Ball: return "Ball";
        case GameMode::UFO: return "UFO";
        case GameMode::Wave: return "Wave";
        case GameMode::Robot: return "Robot";
        case GameMode::Spider: return "Spider";
        case GameMode::Swing: return "Swing";
    }
    return "?";
}

static bool isHoldMode(GameMode m) {
    return m == GameMode::Ship || m == GameMode::Wave || m == GameMode::Swing;
}

// ── Hazard & Orb Object IDs ──

static const std::unordered_set<int> HAZARD_IDS = {
    8, 39, 103, 104, 135, 136, 137, 138, 139, 142, 143, 144, 145, 146,
    183, 184, 185, 186, 187, 188, 363, 364, 365, 392, 393, 394,
    421, 422, 446, 447, 667, 678, 679, 680, 720, 740, 741, 768, 769, 989, 991
};

static const std::unordered_set<int> ORB_IDS = {
    35, 36, 84, 141, 747, 1022, 1330, 1332, 1333, 1594, 1704, 1751, 1764
};

static const std::unordered_set<int> DASH_ORB_IDS = { 1704, 1751 };

// ── Trajectory Simulation ──

struct Sim {
    float x, y;
    double yv;
    bool flip;
    float hh;
};

static void simTick(Sim& s, GameMode m, bool hold, float groundY, float ceilY, double grav) {
    float gd = s.flip ? -1.f : 1.f;
    double g = grav * gd;

    switch (m) {
        case GameMode::Cube:
        case GameMode::Ball:
        case GameMode::Robot:
        case GameMode::Spider:
        case GameMode::UFO:
            s.yv -= g;
            s.y += static_cast<float>(s.yv);
            break;
        case GameMode::Ship:
        case GameMode::Swing: {
            double accel = hold ? 0.8 : -0.8;
            s.yv += accel * gd - g * 0.4;
            s.yv = std::clamp(s.yv, -8.0, 8.0);
            s.y += static_cast<float>(s.yv);
            break;
        }
        case GameMode::Wave:
            s.y += (hold ? 6.f : -6.f) * gd;
            break;
    }

    if (s.y - s.hh < groundY) { s.y = groundY + s.hh; s.yv = 0; }
    if (s.y + s.hh > ceilY)   { s.y = ceilY - s.hh;   s.yv = 0; }
}

static void simJump(Sim& s, GameMode m) {
    float gd = s.flip ? -1.f : 1.f;
    switch (m) {
        case GameMode::Cube:
        case GameMode::Robot:
            s.yv = 6.0 * gd;
            break;
        case GameMode::UFO:
            s.yv = 4.5 * gd;
            break;
        case GameMode::Ball:
        case GameMode::Spider:
            s.flip = !s.flip;
            s.yv = 0;
            break;
        default:
            break;
    }
}

static bool hitsAny(float x, float y, float hh, const std::vector<CCRect>& rects) {
    CCRect pr(x - 12.f, y - hh, 24.f, hh * 2.f);
    for (const auto& r : rects) {
        if (pr.intersectsRect(r)) return true;
    }
    return false;
}

static bool onSurface(const Sim& s, float groundY, float ceilY) {
    if (!s.flip) return (s.y - s.hh) <= (groundY + 4.f);
    return (s.y + s.hh) >= (ceilY - 4.f);
}

// ── PlayLayer Hook ──

class $modify(AIPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* m_debugLabel = nullptr;
        CCDrawNode* m_trajNode = nullptr;
        bool m_holding = false;
        int m_holdTimer = 0;
        int m_cooldown = 0;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        if (Mod::get()->getSettingValue<bool>("show-debug")) {
            auto label = CCLabelBMFont::create("AI", "bigFont.fnt");
            label->setAnchorPoint({0.f, 1.f});
            label->setScale(0.3f);
            auto ws = CCDirector::sharedDirector()->getWinSize();
            label->setPosition({5.f, ws.height - 5.f});
            this->addChild(label, 1000);
            m_fields->m_debugLabel = label;
        }

        auto drawNode = CCDrawNode::create();
        this->m_objectLayer->addChild(drawNode, 10000);
        m_fields->m_trajNode = drawNode;

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_holding = false;
        m_fields->m_holdTimer = 0;
        m_fields->m_cooldown = 0;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        if (!Mod::get()->getSettingValue<bool>("ai-enabled")) {
            if (m_fields->m_trajNode) m_fields->m_trajNode->clear();
            return;
        }

        auto player = m_player1;
        if (!player || player->m_isDead) return;

        GameMode mode = getMode(player);
        float px = player->getPositionX();
        float py = player->getPositionY();
        double pyv = player->m_yVelocity;
        bool flip = player->m_isUpsideDown;
        float hh = player->getContentSize().height * player->getScaleY() * 0.5f;
        float la = static_cast<float>(Mod::get()->getSettingValue<double>("lookahead"));
        int maxJ = static_cast<int>(Mod::get()->getSettingValue<int64_t>("sim-jumps"));
        double grav = player->m_gravity;
        float xspd = player->m_playerSpeed * 5.77f;
        float groundY = 90.f;
        float ceilY = 690.f;

        if (m_fields->m_cooldown > 0) m_fields->m_cooldown--;

        // ── Gather hazards and orbs in lookahead range ──

        std::vector<CCRect> hazards;
        bool orbNear = false;
        bool dashOrbNear = false;

        if (m_objects) {
            for (int i = 0; i < m_objects->count(); i++) {
                auto obj = static_cast<GameObject*>(m_objects->objectAtIndex(i));
                if (!obj) continue;
                int id = obj->m_objectID;
                float ox = obj->getPositionX();
                if (ox - px < -50.f || ox - px > la) continue;

                if (HAZARD_IDS.count(id)) {
                    hazards.push_back(obj->getObjectRect());
                }
                if (ORB_IDS.count(id)) {
                    CCRect orbRect = obj->getObjectRect();
                    CCRect nearRect(px - 5.f, py - hh * 3.f, 60.f, hh * 6.f);
                    if (orbRect.intersectsRect(nearRect)) {
                        orbNear = true;
                        if (DASH_ORB_IDS.count(id)) dashOrbNear = true;
                    }
                }
            }
        }

        // ── Simulate trajectories ──

        int steps = std::clamp(static_cast<int>(la / std::max(xspd, 1.f)), 30, 300);

        // No-action path
        std::vector<CCPoint> noActPath;
        Sim sN = {px, py, pyv, flip, hh};
        bool noActDies = false;
        int noActDeathStep = steps + 1;

        for (int i = 0; i < steps; i++) {
            sN.x += xspd;
            simTick(sN, mode, false, groundY, ceilY, grav);
            noActPath.push_back(ccp(sN.x, sN.y));
            if (!noActDies && hitsAny(sN.x, sN.y, sN.hh, hazards)) {
                noActDies = true;
                noActDeathStep = i;
            }
        }

        // Action path (jump / hold)
        std::vector<CCPoint> actPath;
        Sim sA = {px, py, pyv, flip, hh};
        bool actDies = false;

        if (!isHoldMode(mode)) {
            if (onSurface(sA, groundY, ceilY) ||
                mode == GameMode::UFO ||
                mode == GameMode::Ball ||
                mode == GameMode::Spider) {
                simJump(sA, mode);
            }
            bool wasGround = false;
            int jumpsUsed = 1;
            for (int i = 0; i < steps; i++) {
                sA.x += xspd;
                simTick(sA, mode, false, groundY, ceilY, grav);
                actPath.push_back(ccp(sA.x, sA.y));

                bool onG = onSurface(sA, groundY, ceilY);
                if (onG && !wasGround && jumpsUsed < maxJ) {
                    Sim peek = sA;
                    bool danger = false;
                    for (int j = 0; j < 40; j++) {
                        peek.x += xspd;
                        simTick(peek, mode, false, groundY, ceilY, grav);
                        if (hitsAny(peek.x, peek.y, peek.hh, hazards)) { danger = true; break; }
                    }
                    if (danger) { simJump(sA, mode); jumpsUsed++; }
                }
                wasGround = onG;

                if (hitsAny(sA.x, sA.y, sA.hh, hazards)) { actDies = true; break; }
            }
        } else {
            for (int i = 0; i < steps; i++) {
                sA.x += xspd;
                simTick(sA, mode, true, groundY, ceilY, grav);
                actPath.push_back(ccp(sA.x, sA.y));
                if (hitsAny(sA.x, sA.y, sA.hh, hazards)) { actDies = true; break; }
            }
        }

        // ── Decision ──

        bool shouldAct = false;
        std::string action = "wait";

        if (orbNear) {
            shouldAct = true;
            action = dashOrbNear ? "DASH" : "ORB";
        } else if (isHoldMode(mode)) {
            shouldAct = noActDies && !actDies;
            if (shouldAct) action = "HOLD";
            if (noActDies && actDies && actPath.size() > noActPath.size()) {
                shouldAct = true;
                action = "HOLD";
            }
        } else {
            if (noActDies && !actDies) {
                shouldAct = true;
                action = "JUMP";
            }
            if (noActDies && actDies && actPath.size() > static_cast<size_t>(noActDeathStep)) {
                shouldAct = true;
                action = "JUMP(risk)";
            }
        }

        // ── Execute action ──

        if (shouldAct && m_fields->m_cooldown <= 0) {
            if (isHoldMode(mode)) {
                float gdir = flip ? -1.f : 1.f;
                if (mode == GameMode::Wave) {
                    player->m_yVelocity = 6.0 * gdir;
                } else {
                    player->m_yVelocity += 0.8 * gdir;
                    player->m_yVelocity = std::clamp(player->m_yVelocity, -8.0, 8.0);
                }
                m_fields->m_holding = true;
            } else if (orbNear) {
                auto kb = CCDirector::sharedDirector()->getKeyboardDispatcher();
                kb->dispatchKeyboardMSG(enumKeyCodes::KEY_Up, true, false, 0.0);
                m_fields->m_holding = true;
                m_fields->m_holdTimer = 3;
                m_fields->m_cooldown = 2;
            } else {
                Sim sCur = {px, py, pyv, flip, hh};
                bool canJump = onSurface(sCur, groundY, ceilY) ||
                    mode == GameMode::UFO ||
                    mode == GameMode::Ball ||
                    mode == GameMode::Spider;

                if (canJump) {
                    float gdir = flip ? -1.f : 1.f;
                    switch (mode) {
                        case GameMode::Cube:
                        case GameMode::Robot:
                            player->m_yVelocity = 6.0 * gdir;
                            break;
                        case GameMode::UFO:
                            player->m_yVelocity = 4.5 * gdir;
                            break;
                        case GameMode::Ball:
                        case GameMode::Spider:
                            player->m_isUpsideDown = !player->m_isUpsideDown;
                            player->m_yVelocity = 0;
                            break;
                        default:
                            break;
                    }
                    m_fields->m_cooldown = 8;
                    action += "!";
                }
            }
        } else if (!shouldAct && m_fields->m_holding && isHoldMode(mode)) {
            m_fields->m_holding = false;
        }

        // Release orb key
        if (m_fields->m_holdTimer > 0) {
            m_fields->m_holdTimer--;
            if (m_fields->m_holdTimer == 0) {
                auto kb = CCDirector::sharedDirector()->getKeyboardDispatcher();
                kb->dispatchKeyboardMSG(enumKeyCodes::KEY_Up, false, false, 0.0);
                m_fields->m_holding = false;
            }
        }

        // ── Draw trajectory lines ──

        if (m_fields->m_trajNode) {
            m_fields->m_trajNode->clear();

            if (noActPath.size() > 1) {
                for (size_t i = 0; i + 1 < noActPath.size() && i < 120; i++) {
                    m_fields->m_trajNode->drawSegment(
                        noActPath[i], noActPath[i + 1], 0.5f,
                        ccc4f(1.f, 0.2f, 0.2f, 0.6f));
                }
            }
            if (actPath.size() > 1) {
                for (size_t i = 0; i + 1 < actPath.size() && i < 120; i++) {
                    m_fields->m_trajNode->drawSegment(
                        actPath[i], actPath[i + 1], 0.5f,
                        ccc4f(0.2f, 1.f, 0.2f, 0.6f));
                }
            }
        }

        // ── Debug label ──

        if (m_fields->m_debugLabel) {
            auto txt = fmt::format("{} | {:.0f},{:.0f} | {} | H:{}",
                modeName(mode), px, py, action, hazards.size());
            m_fields->m_debugLabel->setString(txt.c_str());
        }
    }
};

// ── Pause Menu Settings ──

class $modify(AIPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        // Background panel
        auto bg = CCScale9Sprite::create("square02_small.png");
        bg->setContentSize({220.f, 160.f});
        bg->setOpacity(180);
        bg->setPosition({winSize.width - 125.f, winSize.height / 2.f});
        this->addChild(bg, 100);

        float startY = winSize.height / 2.f + 55.f;
        float cx = winSize.width - 125.f;

        // Title
        auto title = CCLabelBMFont::create("AI Bot Settings", "bigFont.fnt");
        title->setScale(0.35f);
        title->setPosition({cx, startY});
        this->addChild(title, 101);

        // ── AI Enabled toggle ──
        bool aiOn = Mod::get()->getSettingValue<bool>("ai-enabled");
        auto aiLabel = CCLabelBMFont::create("AI Enabled", "bigFont.fnt");
        aiLabel->setScale(0.25f);
        aiLabel->setPosition({cx - 40.f, startY - 30.f});
        this->addChild(aiLabel, 101);

        auto aiToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(AIPauseLayer::onToggleAI), 0.5f);
        aiToggle->toggle(aiOn);
        aiToggle->setPosition({85.f, 0.f});
        auto aiMenu = CCMenu::create();
        aiMenu->setPosition({cx + 40.f, startY - 30.f});
        aiMenu->addChild(aiToggle);
        this->addChild(aiMenu, 101);

        // ── Debug toggle ──
        bool dbgOn = Mod::get()->getSettingValue<bool>("show-debug");
        auto dbgLabel = CCLabelBMFont::create("Show Debug", "bigFont.fnt");
        dbgLabel->setScale(0.25f);
        dbgLabel->setPosition({cx - 40.f, startY - 55.f});
        this->addChild(dbgLabel, 101);

        auto dbgToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(AIPauseLayer::onToggleDebug), 0.5f);
        dbgToggle->toggle(dbgOn);
        dbgToggle->setPosition({85.f, 0.f});
        auto dbgMenu = CCMenu::create();
        dbgMenu->setPosition({cx + 40.f, startY - 55.f});
        dbgMenu->addChild(dbgToggle);
        this->addChild(dbgMenu, 101);

        // ── Lookahead value ──
        double laVal = Mod::get()->getSettingValue<double>("lookahead");
        auto laLabel = CCLabelBMFont::create(
            fmt::format("Lookahead: {:.0f}", laVal).c_str(), "bigFont.fnt");
        laLabel->setScale(0.25f);
        laLabel->setPosition({cx, startY - 80.f});
        laLabel->setTag(500);
        this->addChild(laLabel, 101);

        auto laMinus = CCMenuItemSpriteExtra::create(
            CCLabelBMFont::create("-", "bigFont.fnt"),
            this, menu_selector(AIPauseLayer::onLookaheadMinus));
        laMinus->setScale(0.5f);
        laMinus->setPosition({-50.f, 0.f});
        auto laPlus = CCMenuItemSpriteExtra::create(
            CCLabelBMFont::create("+", "bigFont.fnt"),
            this, menu_selector(AIPauseLayer::onLookaheadPlus));
        laPlus->setScale(0.5f);
        laPlus->setPosition({50.f, 0.f});
        auto laMenu = CCMenu::create();
        laMenu->setPosition({cx, startY - 95.f});
        laMenu->addChild(laMinus);
        laMenu->addChild(laPlus);
        this->addChild(laMenu, 101);

        // ── Sim Jumps value ──
        int64_t sjVal = Mod::get()->getSettingValue<int64_t>("sim-jumps");
        auto sjLabel = CCLabelBMFont::create(
            fmt::format("Sim Jumps: {}", sjVal).c_str(), "bigFont.fnt");
        sjLabel->setScale(0.25f);
        sjLabel->setPosition({cx, startY - 115.f});
        sjLabel->setTag(501);
        this->addChild(sjLabel, 101);

        auto sjMinus = CCMenuItemSpriteExtra::create(
            CCLabelBMFont::create("-", "bigFont.fnt"),
            this, menu_selector(AIPauseLayer::onSimJumpsMinus));
        sjMinus->setScale(0.5f);
        sjMinus->setPosition({-50.f, 0.f});
        auto sjPlus = CCMenuItemSpriteExtra::create(
            CCLabelBMFont::create("+", "bigFont.fnt"),
            this, menu_selector(AIPauseLayer::onSimJumpsPlus));
        sjPlus->setScale(0.5f);
        sjPlus->setPosition({50.f, 0.f});
        auto sjMenu = CCMenu::create();
        sjMenu->setPosition({cx, startY - 130.f});
        sjMenu->addChild(sjMinus);
        sjMenu->addChild(sjPlus);
        this->addChild(sjMenu, 101);
    }

    void onToggleAI(CCObject*) {
        bool cur = Mod::get()->getSettingValue<bool>("ai-enabled");
        Mod::get()->setSettingValue("ai-enabled", !cur);
    }

    void onToggleDebug(CCObject*) {
        bool cur = Mod::get()->getSettingValue<bool>("show-debug");
        Mod::get()->setSettingValue("show-debug", !cur);
    }

    void onLookaheadMinus(CCObject*) {
        double val = Mod::get()->getSettingValue<double>("lookahead");
        val = std::max(50.0, val - 50.0);
        Mod::get()->setSettingValue("lookahead", val);
        if (auto lbl = static_cast<CCLabelBMFont*>(this->getChildByTag(500))) {
            lbl->setString(fmt::format("Lookahead: {:.0f}", val).c_str());
        }
    }

    void onLookaheadPlus(CCObject*) {
        double val = Mod::get()->getSettingValue<double>("lookahead");
        val = std::min(1000.0, val + 50.0);
        Mod::get()->setSettingValue("lookahead", val);
        if (auto lbl = static_cast<CCLabelBMFont*>(this->getChildByTag(500))) {
            lbl->setString(fmt::format("Lookahead: {:.0f}", val).c_str());
        }
    }

    void onSimJumpsMinus(CCObject*) {
        int64_t val = Mod::get()->getSettingValue<int64_t>("sim-jumps");
        val = std::max((int64_t)1, val - 1);
        Mod::get()->setSettingValue("sim-jumps", val);
        if (auto lbl = static_cast<CCLabelBMFont*>(this->getChildByTag(501))) {
            lbl->setString(fmt::format("Sim Jumps: {}", val).c_str());
        }
    }

    void onSimJumpsPlus(CCObject*) {
        int64_t val = Mod::get()->getSettingValue<int64_t>("sim-jumps");
        val = std::min((int64_t)10, val + 1);
        Mod::get()->setSettingValue("sim-jumps", val);
        if (auto lbl = static_cast<CCLabelBMFont*>(this->getChildByTag(501))) {
            lbl->setString(fmt::format("Sim Jumps: {}", val).c_str());
        }
    }
};

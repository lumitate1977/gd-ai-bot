#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

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
        case GameMode::Cube: return "Cube"; case GameMode::Ship: return "Ship";
        case GameMode::Ball: return "Ball"; case GameMode::UFO: return "UFO";
        case GameMode::Wave: return "Wave"; case GameMode::Robot: return "Robot";
        case GameMode::Spider: return "Spider"; case GameMode::Swing: return "Swing";
    }
    return "?";
}

static bool isHoldMode(GameMode m) {
    return m == GameMode::Ship || m == GameMode::Wave || m == GameMode::Swing;
}

// ── Object IDs ──

static const std::unordered_set<int> HAZARD_IDS = {
    8, 39, 103, 104, 135, 136, 137, 138, 139, 142, 143, 144, 145, 146,
    183, 184, 185, 186, 187, 188, 363, 364, 365, 392, 393, 394,
    421, 422, 446, 447, 667, 678, 679, 680, 720, 740, 741, 768, 769, 989, 991
};

static const std::unordered_set<int> ORB_IDS = {
    35, 36, 84, 141, 747, 1022, 1330, 1332, 1333, 1594, 1704, 1751, 1764
};

static const std::unordered_set<int> DASH_ORB_IDS = { 1704, 1751 };

// ── Simulation ──

struct Sim {
    float x, y;
    double yv;
    bool flip;
    float hh;
};

static void simTick(Sim& s, GameMode m, bool hold, float groundY, float ceilY, float grav) {
    float gd = s.flip ? -1.f : 1.f;
    float g = grav * gd;
    float dt = 1.f;

    switch (m) {
        case GameMode::Cube: case GameMode::Ball:
        case GameMode::Robot: case GameMode::Spider: case GameMode::UFO:
            s.yv -= g * dt;
            s.y += (float)(s.yv * dt);
            break;
        case GameMode::Ship: case GameMode::Swing: {
            float a = hold ? 0.8f : -0.8f;
            s.yv += (a * gd - g * 0.4) * dt;
            s.yv = std::clamp(s.yv, -8.0, 8.0);
            s.y += (float)(s.yv * dt);
            break;
        }
        case GameMode::Wave:
            s.y += (hold ? 6.f : -6.f) * gd * dt;
            break;
    }

    if (s.y - s.hh < groundY) { s.y = groundY + s.hh; s.yv = 0; }
    if (s.y + s.hh > ceilY) { s.y = ceilY - s.hh; s.yv = 0; }
}

static void simJump(Sim& s, GameMode m) {
    float gd = s.flip ? -1.f : 1.f;
    switch (m) {
        case GameMode::Cube: case GameMode::Robot: s.yv = 6.0 * gd; break;
        case GameMode::UFO: s.yv = 4.5 * gd; break;
        case GameMode::Ball: case GameMode::Spider: s.flip = !s.flip; s.yv = 0; break;
        default: break;
    }
}

static bool hitsAny(float x, float y, float hh, const std::vector<CCRect>& rects) {
    CCRect pr(x - 12.f, y - hh, 24.f, hh * 2.f);
    for (auto& r : rects) if (pr.intersectsRect(r)) return true;
    return false;
}

static bool onSurface(Sim& s, float groundY, float ceilY) {
    if (!s.flip) return (s.y - s.hh) <= (groundY + 4.f);
    return (s.y + s.hh) >= (ceilY - 4.f);
}

// ── Main ──

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

        // Debug label
        if (Mod::get()->getSettingValue<bool>("show-debug")) {
            auto label = CCLabelBMFont::create("AI", "bigFont.fnt");
            label->setAnchorPoint({0.f, 1.f});
            label->setScale(0.3f);
            auto ws = CCDirector::sharedDirector()->getWinSize();
            label->setPosition({5.f, ws.height - 5.f});
            this->addChild(label, 1000);
            m_fields->m_debugLabel = label;
        }

        // Trajectory draw node
        auto dn = CCDrawNode::create();
        this->m_objectLayer->addChild(dn, 10000);
        m_fields->m_trajNode = dn;

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_holding = false;
        m_fields->m_holdTimer = 0;
        m_fields->m_cooldown = 0;
    }

    void update(float dt) {
        // Run game first
        PlayLayer::update(dt);

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
        float la = 200.f;
        int maxJ = 3;
        float grav = (float)player->m_gravity;
        float xspd = player->m_playerSpeed * 5.77f;
        float groundY = 90.f;
        float ceilY = 690.f;

        if (m_fields->m_cooldown > 0) m_fields->m_cooldown--;

        // ── Gather hazards + orbs ──
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
        int steps = std::clamp((int)(la / std::max(xspd, 1.f)), 30, 300);

        // Path without action
        std::vector<CCPoint> noActPath;
        Sim sN = {px, py, pyv, flip, hh};
        bool noActDies = false;
        int noActDeathStep = steps + 1;

        for (int i = 0; i < steps; i++) {
            sN.x += xspd;
            simTick(sN, mode, false, groundY, ceilY, grav);
            noActPath.push_back({sN.x, sN.y});
            if (!noActDies && hitsAny(sN.x, sN.y, sN.hh, hazards)) {
                noActDies = true;
                noActDeathStep = i;
            }
        }

        // Path with action (jump now / hold)
        std::vector<CCPoint> actPath;
        Sim sA = {px, py, pyv, flip, hh};
        bool actDies = false;

        if (!isHoldMode(mode)) {
            // Click mode: simulate jump
            if (onSurface(sA, groundY, ceilY) || mode == GameMode::UFO || mode == GameMode::Ball || mode == GameMode::Spider) {
                simJump(sA, mode);
            }
            bool wasGround = false;
            int jumpsUsed = 1;
            for (int i = 0; i < steps; i++) {
                sA.x += xspd;
                simTick(sA, mode, false, groundY, ceilY, grav);
                actPath.push_back({sA.x, sA.y});

                // Multi-jump on landing
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
            // Hold mode: simulate holding
            for (int i = 0; i < steps; i++) {
                sA.x += xspd;
                simTick(sA, mode, true, groundY, ceilY, grav);
                actPath.push_back({sA.x, sA.y});
                if (hitsAny(sA.x, sA.y, sA.hh, hazards)) { actDies = true; break; }
            }
        }

        // ── Decide ──
        bool shouldAct = false;
        std::string action = "wait";

        if (orbNear) {
            shouldAct = true;
            action = dashOrbNear ? "DASH" : "ORB";
        } else if (isHoldMode(mode)) {
            // Hold if not-holding path dies and holding path survives longer
            shouldAct = noActDies && !actDies;
            if (shouldAct) action = "HOLD";
            // Also hold if both die but holding survives longer
            if (noActDies && actDies && actPath.size() > noActPath.size()) {
                shouldAct = true;
                action = "HOLD";
            }
        } else {
            // Jump if no-jump dies and jumping survives
            if (noActDies && !actDies) {
                shouldAct = true;
                action = "JUMP";
            }
            // Jump if both die but jumping is better
            if (noActDies && actDies && actPath.size() > (size_t)noActDeathStep) {
                shouldAct = true;
                action = "JUMP(risk)";
            }
        }

        // ── Execute: directly manipulate player physics ──
        if (shouldAct && m_fields->m_cooldown <= 0) {
            if (isHoldMode(mode)) {
                // Ship/wave/swing: set velocity directly
                float gd = flip ? -1.f : 1.f;
                if (mode == GameMode::Wave) {
                    player->m_yVelocity = 6.0 * gd;
                } else {
                    player->m_yVelocity += 0.8 * gd;
                    player->m_yVelocity = std::clamp(player->m_yVelocity, -8.0, 8.0);
                }
                m_fields->m_holding = true;
            } else if (orbNear) {
                // For orbs: use keyboard simulation
                auto kb = CCDirector::sharedDirector()->getKeyboardDispatcher();
                kb->dispatchKeyboardMSG(enumKeyCodes::KEY_Up, true, false, 0);
                m_fields->m_holding = true;
                m_fields->m_holdTimer = 3;
                m_fields->m_cooldown = 2;
            } else {
                // Click modes: apply jump velocity directly
                Sim sCur = {px, py, pyv, flip, hh};
                bool canJump = onSurface(sCur, groundY, ceilY) ||
                    mode == GameMode::UFO ||
                    mode == GameMode::Ball ||
                    mode == GameMode::Spider;

                if (canJump) {
                    float gd = flip ? -1.f : 1.f;
                    switch (mode) {
                        case GameMode::Cube:
                        case GameMode::Robot:
                            player->m_yVelocity = 6.0 * gd;
                            break;
                        case GameMode::UFO:
                            player->m_yVelocity = 4.5 * gd;
                            break;
                        case GameMode::Ball:
                        case GameMode::Spider:
                            player->m_isUpsideDown = !player->m_isUpsideDown;
                            player->m_yVelocity = 0;
                            break;
                        default: break;
                    }
                    m_fields->m_cooldown = 8;
                    action += "!";
                }
            }
        } else if (!shouldAct && m_fields->m_holding && isHoldMode(mode)) {
            m_fields->m_holding = false;
        }

        // Release orb key press
        if (m_fields->m_holdTimer > 0) {
            m_fields->m_holdTimer--;
            if (m_fields->m_holdTimer == 0) {
                auto kb = CCDirector::sharedDirector()->getKeyboardDispatcher();
                kb->dispatchKeyboardMSG(enumKeyCodes::KEY_Up, false, false, 0);
                m_fields->m_holding = false;
            }
        }

        // ── Draw trajectory ──
        if (m_fields->m_trajNode) {
            m_fields->m_trajNode->clear();

            // No-action path: red
            if (noActPath.size() > 1) {
                for (size_t i = 0; i + 1 < noActPath.size() && i < 120; i++) {
                    m_fields->m_trajNode->drawSegment(
                        noActPath[i], noActPath[i + 1], 0.5f,
                        ccc4f(1.f, 0.2f, 0.2f, 0.6f));
                }
            }
            // Action path: green
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

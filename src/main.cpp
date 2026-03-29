#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;

// ── Game Mode Detection ──

enum class GameMode {
    Cube, Ship, Ball, UFO, Wave, Robot, Spider, Swingcopter
};

static GameMode getGameMode(PlayerObject* p) {
    if (p->m_isShip) return GameMode::Ship;
    if (p->m_isBall) return GameMode::Ball;
    if (p->m_isBird) return GameMode::UFO;
    if (p->m_isDart) return GameMode::Wave;
    if (p->m_isRobot) return GameMode::Robot;
    if (p->m_isSpider) return GameMode::Spider;
    if (p->m_isSwing) return GameMode::Swingcopter;
    return GameMode::Cube;
}

static const char* gameModeName(GameMode m) {
    switch (m) {
        case GameMode::Cube: return "Cube";
        case GameMode::Ship: return "Ship";
        case GameMode::Ball: return "Ball";
        case GameMode::UFO: return "UFO";
        case GameMode::Wave: return "Wave";
        case GameMode::Robot: return "Robot";
        case GameMode::Spider: return "Spider";
        case GameMode::Swingcopter: return "Swing";
    }
    return "?";
}

static bool isHoldMode(GameMode m) {
    return m == GameMode::Ship || m == GameMode::Wave || m == GameMode::Swingcopter;
}

// ── Object ID Sets ──

static const std::unordered_set<int> HAZARD_IDS = {
    // Spikes
    8, 39, 103, 104, 135, 136, 137, 138, 139, 142, 143, 144, 145, 146,
    183, 184, 185, 186, 187, 188,
    363, 364, 365, 392, 393, 394,
    421, 422, 446, 447,
    667, 678, 679, 680,
    720, 740, 741, 768, 769, 989, 991
};

static const std::unordered_set<int> ORB_IDS = {
    // Yellow=36, Pink=35, Red=1022, Blue=84, Green=1332,
    // Black/drop=1333, Spider=1594, Dash=1704, Green Dash=1751,
    // Swing=1764, Teleport=747
    35, 36, 84, 141, 747, 1022, 1330, 1332, 1333,
    1594, 1704, 1751, 1764
};

static const std::unordered_set<int> DASH_ORB_IDS = {
    1704, 1751 // Dash orb, Green dash orb
};

// ── Death Tracker (persists across attempts) ──

static std::vector<float> g_deathPositions;
static const int MAX_DEATHS = 200;

static bool nearDeathSpot(float x, float range) {
    for (float dx : g_deathPositions) {
        if (std::abs(x - dx) < range) return true;
    }
    return false;
}

// ── Simulation ──

struct SimState {
    float x, y;
    double yVel;
    bool upsideDown;
    float halfH;
};

static void simStep(SimState& s, GameMode mode, bool holding, float dt, float groundY, float ceilY, float gravity) {
    float gDir = s.upsideDown ? -1.f : 1.f;
    float g = gravity * gDir;

    switch (mode) {
        case GameMode::Cube:
        case GameMode::Ball:
        case GameMode::Robot:
        case GameMode::Spider:
        case GameMode::UFO:
            s.yVel -= g * dt * 60.0;
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        case GameMode::Ship:
        case GameMode::Swingcopter: {
            float accel = holding ? 0.8f : -0.8f;
            s.yVel += (accel * gDir - g * 0.4) * dt * 60.0;
            s.yVel = std::clamp(s.yVel, -8.0, 8.0);
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        }
        case GameMode::Wave: {
            float spd = 6.f;
            s.y += (holding ? spd : -spd) * gDir * dt * 60.f;
            break;
        }
    }

    // Clamp to bounds
    if (s.y - s.halfH < groundY) { s.y = groundY + s.halfH; s.yVel = 0; }
    if (s.y + s.halfH > ceilY)   { s.y = ceilY - s.halfH;   s.yVel = 0; }
}

static void simJump(SimState& s, GameMode mode) {
    float gDir = s.upsideDown ? -1.f : 1.f;
    switch (mode) {
        case GameMode::Cube:
        case GameMode::Robot:
            s.yVel = 6.0 * gDir;
            break;
        case GameMode::UFO:
            s.yVel = 4.5 * gDir;
            break;
        case GameMode::Ball:
        case GameMode::Spider:
            s.upsideDown = !s.upsideDown;
            s.yVel = 0;
            break;
        default:
            break;
    }
}

static bool collidesAny(float x, float y, float halfH,
    const std::vector<CCRect>& rects)
{
    CCRect pr(x - 10.f, y - halfH, 20.f, halfH * 2.f);
    for (auto& r : rects) {
        if (pr.intersectsRect(r)) return true;
    }
    return false;
}

// ── Hook: Record Deaths ──

class $modify(AIPlayerObject, PlayerObject) {
    void playerDestroyed(bool p0) {
        PlayerObject::playerDestroyed(p0);

        if (this == PlayLayer::get()->m_player1) {
            float deathX = this->getPositionX();
            if (g_deathPositions.size() < MAX_DEATHS) {
                g_deathPositions.push_back(deathX);
            }
        }
    }
};

// ── Main Hook ──

class $modify(AIPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* m_debugLabel = nullptr;
        bool m_holding = false;
        int m_holdFrames = 0;
        std::string m_lastAction;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        if (Mod::get()->getSettingValue<bool>("show-debug")) {
            auto label = CCLabelBMFont::create("AI Bot", "bigFont.fnt");
            label->setAnchorPoint({0.f, 1.f});
            label->setScale(0.3f);
            auto ws = CCDirector::sharedDirector()->getWinSize();
            label->setPosition({5.f, ws.height - 5.f});
            this->addChild(label, 100);
            m_fields->m_debugLabel = label;
        }

        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        m_fields->m_holding = false;
        m_fields->m_holdFrames = 0;
    }

    // KEY FIX: Run AI logic BEFORE physics by sending input first,
    // then calling the original update
    void update(float dt) {
        if (!Mod::get()->getSettingValue<bool>("ai-enabled")) {
            PlayLayer::update(dt);
            return;
        }

        auto player = m_player1;
        if (!player || player->m_isDead) {
            PlayLayer::update(dt);
            return;
        }

        // ── Gather state ──
        GameMode mode = getGameMode(player);
        float px = player->getPositionX();
        float py = player->getPositionY();
        double pyVel = player->m_yVelocity;
        bool flipped = player->m_isUpsideDown;
        float halfH = player->getContentSize().height * player->getScaleY() * 0.5f;
        float lookahead = static_cast<float>(Mod::get()->getSettingValue<double>("lookahead"));
        int maxJumps = static_cast<int>(Mod::get()->getSettingValue<int64_t>("sim-jumps"));
        float gravity = static_cast<float>(player->m_gravity);
        float xSpeed = player->m_playerSpeed * 5.77f;
        float groundY = 90.f;
        float ceilY = groundY + 600.f;

        // Increase lookahead near known death spots
        if (nearDeathSpot(px, 200.f)) {
            lookahead *= 1.5f;
        }

        // ── Collect hazards and orbs ──
        std::vector<CCRect> hazards;
        std::vector<std::pair<CCRect, bool>> orbs; // rect, isDash

        auto objects = m_objects;
        if (objects) {
            for (int i = 0; i < objects->count(); i++) {
                auto obj = static_cast<GameObject*>(objects->objectAtIndex(i));
                if (!obj) continue;

                int id = obj->m_objectID;
                float ox = obj->getPositionX();
                float dist = ox - px;
                if (dist < -50.f || dist > lookahead) continue;

                if (HAZARD_IDS.count(id)) {
                    hazards.push_back(obj->getObjectRect());
                }
                if (ORB_IDS.count(id)) {
                    bool isDash = DASH_ORB_IDS.count(id) > 0;
                    orbs.push_back({obj->getObjectRect(), isDash});
                }
            }
        }

        // ── Check for orbs in immediate range ──
        CCRect playerRect(px - 15.f, py - halfH, 30.f, halfH * 2.f);
        bool orbNearby = false;
        bool dashOrbNearby = false;
        for (auto& [rect, isDash] : orbs) {
            // Check if orb is within ~40 units ahead and overlapping vertically
            if (rect.intersectsRect(CCRect(px - 5.f, py - halfH * 2.f, 50.f, halfH * 4.f))) {
                orbNearby = true;
                if (isDash) dashOrbNearby = true;
            }
        }

        // ── Decide action ──
        bool shouldPress = false;
        bool shouldRelease = false;
        std::string action = "wait";

        float simDt = 1.f / 60.f;
        int simStepCount = static_cast<int>(lookahead / std::max(xSpeed * simDt * 60.f, 1.f));
        simStepCount = std::clamp(simStepCount, 30, 300);

        if (orbNearby) {
            // Always activate orbs - press and release quickly
            shouldPress = true;
            action = dashOrbNearby ? "DASH ORB" : "ORB";
        } else if (isHoldMode(mode)) {
            // Simulate hold vs no-hold
            SimState sH = {px, py, pyVel, flipped, halfH};
            SimState sN = {px, py, pyVel, flipped, halfH};
            int holdDie = simStepCount + 1;
            int noholdDie = simStepCount + 1;

            for (int i = 0; i < simStepCount; i++) {
                sH.x += xSpeed * simDt * 60.f;
                sN.x += xSpeed * simDt * 60.f;
                simStep(sH, mode, true, simDt, groundY, ceilY, gravity);
                simStep(sN, mode, false, simDt, groundY, ceilY, gravity);

                if (holdDie > simStepCount && collidesAny(sH.x, sH.y, sH.halfH, hazards))
                    holdDie = i;
                if (noholdDie > simStepCount && collidesAny(sN.x, sN.y, sN.halfH, hazards))
                    noholdDie = i;
            }

            if (noholdDie <= holdDie) {
                shouldPress = true;
                action = "HOLD";
            } else {
                shouldRelease = true;
                action = "release";
            }
        } else {
            // Click modes: simulate no-jump, then test if jumping saves us

            // No-jump path
            SimState sN = {px, py, pyVel, flipped, halfH};
            bool noJumpDies = false;
            for (int i = 0; i < simStepCount; i++) {
                sN.x += xSpeed * simDt * 60.f;
                simStep(sN, mode, false, simDt, groundY, ceilY, gravity);
                if (collidesAny(sN.x, sN.y, sN.halfH, hazards)) {
                    noJumpDies = true;
                    break;
                }
            }

            if (noJumpDies) {
                // Can we jump on a surface right now?
                bool onSurface = false;
                if (!flipped) {
                    onSurface = (py - halfH) <= (groundY + 3.f);
                } else {
                    onSurface = (py + halfH) >= (ceilY - 3.f);
                }
                // UFO/Ball/Spider can jump anytime
                bool canJump = onSurface ||
                    mode == GameMode::UFO ||
                    mode == GameMode::Ball ||
                    mode == GameMode::Spider;

                if (canJump) {
                    // Test jump-now path with multi-jump support
                    SimState sJ = {px, py, pyVel, flipped, halfH};
                    simJump(sJ, mode);
                    bool jumpDies = false;
                    int jumpsUsed = 1;
                    bool wasOnGround = false;

                    for (int i = 0; i < simStepCount; i++) {
                        sJ.x += xSpeed * simDt * 60.f;
                        simStep(sJ, mode, false, simDt, groundY, ceilY, gravity);

                        // Multi-jump: if we land and there's still danger, jump again
                        bool onSurf = false;
                        if (!sJ.upsideDown) {
                            onSurf = (sJ.y - sJ.halfH) <= (groundY + 3.f);
                        } else {
                            onSurf = (sJ.y + sJ.halfH) >= (ceilY - 3.f);
                        }

                        if (onSurf && !wasOnGround && jumpsUsed < maxJumps) {
                            // Look ahead a bit - do we need another jump?
                            SimState peek = sJ;
                            bool willDie = false;
                            for (int j = 0; j < 40; j++) {
                                peek.x += xSpeed * simDt * 60.f;
                                simStep(peek, mode, false, simDt, groundY, ceilY, gravity);
                                if (collidesAny(peek.x, peek.y, peek.halfH, hazards)) {
                                    willDie = true;
                                    break;
                                }
                            }
                            if (willDie) {
                                simJump(sJ, mode);
                                jumpsUsed++;
                            }
                        }
                        wasOnGround = onSurf;

                        if (collidesAny(sJ.x, sJ.y, sJ.halfH, hazards)) {
                            jumpDies = true;
                            break;
                        }
                    }

                    if (!jumpDies) {
                        shouldPress = true;
                        action = "JUMP";
                    } else {
                        // Jumping also dies - but it might still be better than not jumping
                        // Jump anyway if we're very close to death without jumping
                        action = "JUMP(risky)";
                        shouldPress = true;
                    }
                }
            }
        }

        // ── Execute input BEFORE the game update ──
        if (shouldPress && !m_fields->m_holding) {
            // Press button
            GJBaseGameLayer::handleButton(true, 0, true);
            m_fields->m_holding = true;
            m_fields->m_holdFrames = 0;
        }

        if (m_fields->m_holding) {
            m_fields->m_holdFrames++;

            // For click modes, release after a few frames
            if (!isHoldMode(mode) && !orbNearby) {
                if (m_fields->m_holdFrames >= 4) {
                    GJBaseGameLayer::handleButton(false, 0, true);
                    m_fields->m_holding = false;
                    m_fields->m_holdFrames = 0;
                }
            }
            // For hold modes, release when told to
            if (isHoldMode(mode) && shouldRelease) {
                GJBaseGameLayer::handleButton(false, 0, true);
                m_fields->m_holding = false;
                m_fields->m_holdFrames = 0;
            }
            // For orbs, quick release
            if (orbNearby && m_fields->m_holdFrames >= 2) {
                GJBaseGameLayer::handleButton(false, 0, true);
                m_fields->m_holding = false;
                m_fields->m_holdFrames = 0;
            }
        }

        // ── NOW run the game physics ──
        PlayLayer::update(dt);

        // ── Debug overlay ──
        if (m_fields->m_debugLabel) {
            m_fields->m_lastAction = action;
            std::string text = fmt::format(
                "{} | {:.0f},{:.0f} | v:{:.1f} | {} | H:{} O:{} D:{}",
                gameModeName(mode), px, py, pyVel,
                action,
                hazards.size(), orbs.size(), g_deathPositions.size()
            );
            m_fields->m_debugLabel->setString(text.c_str());
        }
    }
};

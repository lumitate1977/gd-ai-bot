#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

enum class GameMode {
    Cube,
    Ship,
    Ball,
    UFO,
    Wave,
    Robot,
    Spider,
    Swingcopter
};

struct SimState {
    float x;
    float y;
    double yVel;
    bool upsideDown;
    float halfHeight;
};

static GameMode getGameMode(PlayerObject* player) {
    if (player->m_isShip) return GameMode::Ship;
    if (player->m_isBall) return GameMode::Ball;
    if (player->m_isBird) return GameMode::UFO;
    if (player->m_isDart) return GameMode::Wave;
    if (player->m_isRobot) return GameMode::Robot;
    if (player->m_isSpider) return GameMode::Spider;
    if (player->m_isSwing) return GameMode::Swingcopter;
    return GameMode::Cube;
}

static const char* gameModeName(GameMode mode) {
    switch (mode) {
        case GameMode::Cube: return "Cube";
        case GameMode::Ship: return "Ship";
        case GameMode::Ball: return "Ball";
        case GameMode::UFO: return "UFO";
        case GameMode::Wave: return "Wave";
        case GameMode::Robot: return "Robot";
        case GameMode::Spider: return "Spider";
        case GameMode::Swingcopter: return "Swing";
    }
    return "Unknown";
}

// Simulate one physics step for a given game mode
// Returns true if the player "jumped" this step
static void simStep(SimState& s, GameMode mode, bool holding, float dt, float groundY, float ceilY, float gravity) {
    float gravDir = s.upsideDown ? -1.f : 1.f;
    float g = gravity * gravDir;

    switch (mode) {
        case GameMode::Cube: {
            s.yVel -= g * dt * 60.0;
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        }
        case GameMode::Ship: {
            float accel = holding ? 0.8f : -0.8f;
            s.yVel += (accel * gravDir - g * 0.4) * dt * 60.0;
            if (s.yVel > 8.0) s.yVel = 8.0;
            if (s.yVel < -8.0) s.yVel = -8.0;
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        }
        case GameMode::Ball: {
            // Ball just falls with gravity, click swaps gravity
            s.yVel -= g * dt * 60.0;
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        }
        case GameMode::UFO: {
            // UFO: gravity always, boost on click frame
            s.yVel -= g * dt * 60.0;
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        }
        case GameMode::Wave: {
            float waveSpeed = 6.f;
            if (holding) {
                s.y += waveSpeed * gravDir * dt * 60.f;
            } else {
                s.y -= waveSpeed * gravDir * dt * 60.f;
            }
            break;
        }
        case GameMode::Robot: {
            s.yVel -= g * dt * 60.0;
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        }
        case GameMode::Spider: {
            s.yVel -= g * dt * 60.0;
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        }
        case GameMode::Swingcopter: {
            float accel = holding ? 0.8f : -0.8f;
            // Swing alternates: gravity pulls one way, input pushes other
            s.yVel += (accel - g * 0.4) * dt * 60.0;
            if (s.yVel > 8.0) s.yVel = 8.0;
            if (s.yVel < -8.0) s.yVel = -8.0;
            s.y += static_cast<float>(s.yVel * dt * 60.0);
            break;
        }
    }

    // Clamp to ground/ceiling
    if (!s.upsideDown) {
        if (s.y - s.halfHeight < groundY) {
            s.y = groundY + s.halfHeight;
            s.yVel = 0;
        }
        if (s.y + s.halfHeight > ceilY) {
            s.y = ceilY - s.halfHeight;
            s.yVel = 0;
        }
    } else {
        if (s.y + s.halfHeight > ceilY) {
            s.y = ceilY - s.halfHeight;
            s.yVel = 0;
        }
        if (s.y - s.halfHeight < groundY) {
            s.y = groundY + s.halfHeight;
            s.yVel = 0;
        }
    }
}

// Apply a jump impulse based on game mode
static void simJump(SimState& s, GameMode mode, float gravity) {
    float gravDir = s.upsideDown ? -1.f : 1.f;

    switch (mode) {
        case GameMode::Cube:
            s.yVel = 6.0 * gravDir;
            break;
        case GameMode::Ball:
            s.upsideDown = !s.upsideDown;
            s.yVel = 0;
            break;
        case GameMode::UFO:
            s.yVel = 4.5 * gravDir;
            break;
        case GameMode::Robot:
            s.yVel = 6.0 * gravDir;
            break;
        case GameMode::Spider:
            s.upsideDown = !s.upsideDown;
            s.yVel = 0;
            break;
        case GameMode::Ship:
        case GameMode::Wave:
        case GameMode::Swingcopter:
            // These modes use hold-based input, no impulse jump
            break;
    }
}

static bool isHoldMode(GameMode mode) {
    return mode == GameMode::Ship || mode == GameMode::Wave || mode == GameMode::Swingcopter;
}

// Check if a position collides with any hazard
static bool collidesWithHazard(float x, float y, float halfH,
    const std::vector<std::pair<cocos2d::CCRect, int>>& hazards)
{
    CCRect playerRect(x - 10.f, y - halfH, 20.f, halfH * 2.f);
    for (auto& [rect, id] : hazards) {
        if (playerRect.intersectsRect(rect)) {
            return true;
        }
    }
    return false;
}

class $modify(AIPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* m_debugLabel = nullptr;
        bool m_holding = false;
        int m_jumpCooldown = 0;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        if (Mod::get()->getSettingValue<bool>("show-debug")) {
            auto label = CCLabelBMFont::create("AI Debug", "bigFont.fnt");
            label->setAnchorPoint({0.f, 1.f});
            label->setScale(0.3f);

            auto winSize = CCDirector::sharedDirector()->getWinSize();
            label->setPosition({5.f, winSize.height - 5.f});

            this->addChild(label, 100);
            m_fields->m_debugLabel = label;
        }

        return true;
    }

    void update(float dt) {
        PlayLayer::update(dt);

        if (!Mod::get()->getSettingValue<bool>("ai-enabled")) return;

        auto player = m_player1;
        if (!player) return;
        if (player->m_isDead) return;

        if (m_fields->m_jumpCooldown > 0) {
            m_fields->m_jumpCooldown--;
        }

        GameMode mode = getGameMode(player);
        float playerX = player->getPositionX();
        float playerY = player->getPositionY();
        double playerYVel = player->m_yVelocity;
        bool upsideDown = player->m_isUpsideDown;
        float playerHalfH = player->getContentSize().height * player->getScaleY() * 0.5f;
        float lookahead = static_cast<float>(Mod::get()->getSettingValue<double>("lookahead"));
        int maxJumps = static_cast<int>(Mod::get()->getSettingValue<int64_t>("sim-jumps"));
        float gravity = static_cast<float>(player->m_gravity);

        // Estimate ground and ceiling
        float groundY = 90.f;
        float ceilY = groundY + 600.f;

        // Gather hazards in lookahead range
        // Hazard object IDs: spikes (8,39,135,136,137,138,139,142,143,144,145,146,
        //   363,364,365,392,393,394,446,447,667,989,991,720,421,422,768,769),
        //   sawblades (103,104,183,184,185,186,187,188,678,679,680,740,741)
        static const std::unordered_set<int> hazardIDs = {
            8, 39, 103, 104, 135, 136, 137, 138, 139, 142, 143, 144, 145, 146,
            183, 184, 185, 186, 187, 188,
            363, 364, 365, 392, 393, 394,
            421, 422, 446, 447,
            667, 678, 679, 680,
            720, 740, 741, 768, 769, 989, 991
        };

        std::vector<std::pair<CCRect, int>> hazards;
        auto objects = m_objects;
        if (objects) {
            for (int i = 0; i < objects->count(); i++) {
                auto obj = static_cast<GameObject*>(objects->objectAtIndex(i));
                if (!obj) continue;

                int id = obj->m_objectID;
                if (hazardIDs.find(id) == hazardIDs.end()) continue;

                float objX = obj->getPositionX();
                float dist = objX - playerX;
                if (dist < -50.f || dist > lookahead) continue;

                auto rect = obj->getObjectRect();
                hazards.push_back({rect, id});
            }
        }

        // Get horizontal speed (pixels per frame at 60fps)
        float xSpeed = player->m_playerSpeed * 5.77f; // approximate px/frame

        // Simulate trajectory
        float simDt = 1.f / 60.f;
        int simSteps = static_cast<int>(lookahead / (xSpeed * simDt * 60.f));
        if (simSteps < 30) simSteps = 30;
        if (simSteps > 300) simSteps = 300;

        bool shouldAct = false; // Should we press/hold now?

        if (isHoldMode(mode)) {
            // For hold modes (ship, wave, swing): simulate both holding and not holding
            // Pick whichever path avoids hazards better

            SimState sHold = {playerX, playerY, playerYVel, upsideDown, playerHalfH};
            SimState sNoHold = {playerX, playerY, playerYVel, upsideDown, playerHalfH};

            int holdCollideStep = simSteps + 1;
            int noHoldCollideStep = simSteps + 1;

            for (int i = 0; i < simSteps; i++) {
                sHold.x += xSpeed * simDt * 60.f;
                sNoHold.x += xSpeed * simDt * 60.f;

                simStep(sHold, mode, true, simDt, groundY, ceilY, gravity);
                simStep(sNoHold, mode, false, simDt, groundY, ceilY, gravity);

                if (holdCollideStep > simSteps &&
                    collidesWithHazard(sHold.x, sHold.y, sHold.halfHeight, hazards)) {
                    holdCollideStep = i;
                }
                if (noHoldCollideStep > simSteps &&
                    collidesWithHazard(sNoHold.x, sNoHold.y, sNoHold.halfHeight, hazards)) {
                    noHoldCollideStep = i;
                }
            }

            // Hold if not-holding dies sooner, or if both are safe and we need to stay centered
            shouldAct = (noHoldCollideStep < holdCollideStep);

        } else {
            // For click modes (cube, ball, ufo, robot, spider):
            // Try simulating N jumps ahead and see if jumping NOW avoids death

            // First: simulate no-jump trajectory
            SimState sNoJump = {playerX, playerY, playerYVel, upsideDown, playerHalfH};
            bool noJumpDies = false;
            int noJumpDeathStep = simSteps + 1;

            for (int i = 0; i < simSteps; i++) {
                sNoJump.x += xSpeed * simDt * 60.f;
                simStep(sNoJump, mode, false, simDt, groundY, ceilY, gravity);
                if (collidesWithHazard(sNoJump.x, sNoJump.y, sNoJump.halfHeight, hazards)) {
                    noJumpDies = true;
                    noJumpDeathStep = i;
                    break;
                }
            }

            if (noJumpDies) {
                // Try jumping now and simulate the rest
                // Test multiple jump timings within the next few frames
                bool bestFound = false;
                int bestSurvival = -1;

                // Try jumping at frame 0 (now)
                for (int jumpFrame = 0; jumpFrame <= 5; jumpFrame++) {
                    SimState s = {playerX, playerY, playerYVel, upsideDown, playerHalfH};
                    bool died = false;
                    int jumpsUsed = 0;
                    bool onGround = true;

                    for (int i = 0; i < simSteps; i++) {
                        s.x += xSpeed * simDt * 60.f;

                        // Jump at the planned frame
                        if (i == jumpFrame && jumpsUsed < maxJumps) {
                            bool isOnSurface = false;
                            if (!s.upsideDown) {
                                isOnSurface = (s.y - s.halfHeight) <= (groundY + 2.f);
                            } else {
                                isOnSurface = (s.y + s.halfHeight) >= (ceilY - 2.f);
                            }

                            if (isOnSurface || mode == GameMode::UFO || mode == GameMode::Ball || mode == GameMode::Spider) {
                                simJump(s, mode, gravity);
                                jumpsUsed++;
                            }
                        }

                        simStep(s, mode, false, simDt, groundY, ceilY, gravity);

                        if (collidesWithHazard(s.x, s.y, s.halfHeight, hazards)) {
                            died = true;
                            if (i > bestSurvival) {
                                bestSurvival = i;
                            }
                            break;
                        }
                    }

                    if (!died && jumpFrame == 0) {
                        // Jumping now leads to survival
                        bestFound = true;
                        break;
                    }
                }

                // Also try multi-jump: jump now, then jump again when landing
                if (!bestFound) {
                    SimState s = {playerX, playerY, playerYVel, upsideDown, playerHalfH};
                    int jumpsUsed = 0;
                    bool died = false;

                    // Jump immediately
                    bool isOnSurface = false;
                    if (!s.upsideDown) {
                        isOnSurface = (s.y - s.halfHeight) <= (groundY + 2.f);
                    } else {
                        isOnSurface = (s.y + s.halfHeight) >= (ceilY - 2.f);
                    }
                    if (isOnSurface || mode == GameMode::UFO || mode == GameMode::Ball || mode == GameMode::Spider) {
                        simJump(s, mode, gravity);
                        jumpsUsed++;
                    }

                    bool wasOnGround = false;
                    for (int i = 0; i < simSteps; i++) {
                        s.x += xSpeed * simDt * 60.f;
                        simStep(s, mode, false, simDt, groundY, ceilY, gravity);

                        // Check if we landed and can jump again
                        bool onSurface = false;
                        if (!s.upsideDown) {
                            onSurface = (s.y - s.halfHeight) <= (groundY + 2.f);
                        } else {
                            onSurface = (s.y + s.halfHeight) >= (ceilY - 2.f);
                        }

                        if (onSurface && !wasOnGround && jumpsUsed < maxJumps) {
                            // Check if we need another jump
                            SimState test = s;
                            bool willDie = false;
                            for (int j = 0; j < 30; j++) {
                                test.x += xSpeed * simDt * 60.f;
                                simStep(test, mode, false, simDt, groundY, ceilY, gravity);
                                if (collidesWithHazard(test.x, test.y, test.halfHeight, hazards)) {
                                    willDie = true;
                                    break;
                                }
                            }
                            if (willDie) {
                                simJump(s, mode, gravity);
                                jumpsUsed++;
                            }
                        }
                        wasOnGround = onSurface;

                        if (collidesWithHazard(s.x, s.y, s.halfHeight, hazards)) {
                            died = true;
                            break;
                        }
                    }

                    if (!died) {
                        bestFound = true;
                    }
                }

                shouldAct = bestFound;
            }
        }

        // Execute input via handleButton on the game layer
        if (isHoldMode(mode)) {
            if (shouldAct && !m_fields->m_holding) {
                this->handleButton(true, 0, true);
                m_fields->m_holding = true;
            } else if (!shouldAct && m_fields->m_holding) {
                this->handleButton(false, 0, true);
                m_fields->m_holding = false;
            }
        } else {
            if (shouldAct && m_fields->m_jumpCooldown <= 0) {
                this->handleButton(true, 0, true);
                m_fields->m_holding = true;
                m_fields->m_jumpCooldown = 4; // hold for a few frames
            } else if (m_fields->m_holding && m_fields->m_jumpCooldown <= 0) {
                this->handleButton(false, 0, true);
                m_fields->m_holding = false;
            }
        }

        // Debug label
        if (m_fields->m_debugLabel) {
            std::string debugText = fmt::format(
                "{} | Pos: {:.0f},{:.0f} | Vel: {:.1f} | {} | Hazards: {}",
                gameModeName(mode), playerX, playerY, playerYVel,
                shouldAct ? "JUMP" : "wait",
                hazards.size()
            );
            m_fields->m_debugLabel->setString(debugText.c_str());
        }
    }
};

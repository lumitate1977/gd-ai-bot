#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

class $modify(AIPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* m_debugLabel = nullptr;
        bool m_holding = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        if (Mod::get()->getSettingValue<bool>("show-debug")) {
            auto label = CCLabelBMFont::create("AI Debug", "bigFont.fnt");
            label->setAnchorPoint({0.f, 1.f});
            label->setScale(0.35f);

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
        if (m_player1->m_isDead) return;

        auto player = m_player1;
        if (!player) return;

        float playerX = player->getPositionX();
        float playerY = player->getPositionY();
        float playerHeight = player->getContentSize().height * player->getScaleY();
        float lookahead = Mod::get()->getSettingValue<double>("lookahead");

        float nearestDist = -1.f;
        float nearestY = 0.f;
        bool obstacleFound = false;

        auto objects = m_objects;
        if (objects) {
            for (int i = 0; i < objects->count(); i++) {
                auto obj = static_cast<GameObject*>(objects->objectAtIndex(i));
                if (!obj) continue;

                int id = obj->m_objectID;
                // Spikes: 8, 39; Sawblade: 103
                if (id != 8 && id != 39 && id != 103) continue;

                float objX = obj->getPositionX();
                float objY = obj->getPositionY();
                float dist = objX - playerX;

                if (dist < 0.f || dist > lookahead) continue;

                // Check if object is in the player's vertical path
                float objHeight = obj->getContentSize().height * obj->getScaleY();
                float playerTop = playerY + playerHeight / 2.f;
                float playerBottom = playerY - playerHeight / 2.f;
                float objTop = objY + objHeight / 2.f;
                float objBottom = objY - objHeight / 2.f;

                bool verticalOverlap = playerBottom < objTop && playerTop > objBottom;
                if (!verticalOverlap) continue;

                if (!obstacleFound || dist < nearestDist) {
                    nearestDist = dist;
                    nearestY = objY;
                    obstacleFound = true;
                }
            }
        }

        if (obstacleFound && !m_fields->m_holding) {
            player->pushButton(PlayerButton::Jump);
            m_fields->m_holding = true;
        } else if (!obstacleFound && m_fields->m_holding) {
            player->releaseButton(PlayerButton::Jump);
            m_fields->m_holding = false;
        }

        // Update debug label
        if (m_fields->m_debugLabel) {
            std::string debugText;
            if (obstacleFound) {
                debugText = fmt::format("Pos: {:.0f}, {:.0f} | Obstacle: dist={:.0f} y={:.0f}",
                    playerX, playerY, nearestDist, nearestY);
            } else {
                debugText = fmt::format("Pos: {:.0f}, {:.0f} | No obstacle",
                    playerX, playerY);
            }
            m_fields->m_debugLabel->setString(debugText.c_str());
        }
    }
};

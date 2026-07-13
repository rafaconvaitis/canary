/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

#ifndef USE_PRECOMPILED_HEADERS
	#include <cstdint>
	#include <memory>
#endif

class Player;
class Creature;
struct Position;

class PlayerProtocolObserver {
public:
	virtual ~PlayerProtocolObserver() = default;

	virtual void onPlayerCancelWalk(const std::shared_ptr<const Player> &viewer) { }
	virtual void onPlayerCreatureAppear(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature, const Position &pos, bool isLogin) { }
	virtual void onPlayerCreatureMove(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature, const Position &newPos, int32_t newStackPos, const Position &oldPos, int32_t oldStackPos, bool teleport) { }
	virtual void onPlayerCreatureTurn(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) { }
	virtual void onPlayerCreatureHealth(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) { }
	virtual void onPlayerCreatureBecameVisible(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) { }
	virtual void onPlayerCreatureBecameInvisible(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) { }
};

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
class Item;
class Tile;
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
	virtual void onPlayerCreatureRemovedFromWorld(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) { }
	virtual void onPlayerTileItemAdded(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Tile> &tile, const Position &position, const std::shared_ptr<Item> &item) { }
	virtual void onPlayerTileItemUpdated(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Tile> &tile, const Position &position, const std::shared_ptr<Item> &item) { }
	virtual void onPlayerTileItemRemoved(const std::shared_ptr<const Player> &viewer, const Position &position, const std::shared_ptr<Item> &item) { }
	virtual void onPlayerInventoryUpdated(const std::shared_ptr<const Player> &viewer, uint8_t slotIndex, const std::shared_ptr<Item> &item) { }
	virtual void onPlayerCombatResult(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &attacker, const std::shared_ptr<Creature> &target, int32_t damage, bool targetDied) { }
};

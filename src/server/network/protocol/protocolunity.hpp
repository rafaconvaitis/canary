/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

#include "server/network/protocol/protocol.hpp"
#include "server/network/protocol/protocolunity_contract.hpp"
#include "creatures/players/player_protocol_observer.hpp"

class Player;
class Creature;
class Item;
class Tile;
struct Position;

#ifndef USE_PRECOMPILED_HEADERS
	#include <optional>
	#include <functional>
	#include <memory>
	#include <span>
	#include <string>
	#include <unordered_map>
	#include <unordered_set>
	#include <vector>
#endif

enum class ProtocolUnitySessionState : uint8_t {
	Connected,
	AwaitingHello,
	HelloAccepted,
	AwaitingAuthentication,
	Authenticated,
	CharacterSelected,
	EnteringWorld,
	InWorld,
	Closing,
	Closed,
};

struct ProtocolUnityWorldPosition {
	int32_t x = 0;
	int32_t y = 0;
	int32_t floor = 0;
};

struct ProtocolUnityCharacterSummary {
	uint32_t characterId = 0;
	std::string name {};
	ProtocolUnityWorldPosition position {};
};

struct ProtocolUnityLoginResponse {
	bool success = false;
	std::string message {};
	uint32_t accountId = 0;
	std::vector<ProtocolUnityCharacterSummary> characters {};
};

struct ProtocolUnityEnterWorldResponse {
	bool success = false;
	bool selectionAccepted = false;
	std::string message {};
	uint32_t actorId = 0;
	ProtocolUnityWorldPosition spawnPosition {};
};

enum class ProtocolUnityActorKind : uint8_t {
	Player = 1,
	Monster = 2,
};

enum class ProtocolUnityActorDisposition : uint8_t {
	Friendly = 1,
	Hostile = 2,
	Neutral = 3,
};

struct ProtocolUnityActorState {
	uint32_t actorId = 0;
	std::string name {};
	ProtocolUnityActorKind kind = ProtocolUnityActorKind::Player;
	ProtocolUnityWorldPosition position {};
	uint8_t direction = 0;
	uint16_t health = 0;
	uint16_t maxHealth = 0;
	uint16_t mana = 0;
	uint16_t maxMana = 0;
	ProtocolUnityActorDisposition disposition = ProtocolUnityActorDisposition::Neutral;
	bool isDead = false;
};

struct ProtocolUnityTileState {
	int16_t localX = 0;
	int16_t localY = 0;
	bool blocked = false;
	uint8_t height = 0;
	uint16_t tileId = 0;
};

struct ProtocolUnityInventorySlotState {
	uint8_t slotIndex = 0;
	uint16_t itemTypeId = 0;
	std::string name {};
	uint16_t quantity = 0;
	uint16_t stackLimit = 0;
};

struct ProtocolUnityGroundItemState {
	uint32_t itemInstanceId = 0;
	uint16_t itemTypeId = 0;
	std::string name {};
	ProtocolUnityWorldPosition position {};
	uint16_t quantity = 0;
};

struct ProtocolUnitySessionAction {
	std::vector<std::vector<uint8_t>> outboundFrames {};
	bool closeConnection = false;
};

class ProtocolUnitySession {
public:
	using LoginHandler = std::function<ProtocolUnityLoginResponse(std::string_view accountDescriptor, std::string_view secret)>;
	using EnterWorldHandler = std::function<ProtocolUnityEnterWorldResponse(uint32_t accountId, uint32_t characterId)>;
	using MovementHandler = std::function<ProtocolUnitySessionAction(uint32_t actorId, uint8_t direction)>;
	using AttackHandler = std::function<ProtocolUnitySessionAction(uint32_t actorId, uint32_t targetId)>;

	ProtocolUnitySession(
		const ProtocolUnityContract &initContract,
		std::string initServerName,
		uint16_t initAdvertisedPacketLimit,
		uint8_t initSupportedCapabilities = 1,
		LoginHandler initLoginHandler = {},
		EnterWorldHandler initEnterWorldHandler = {},
		MovementHandler initMovementHandler = {},
		AttackHandler initAttackHandler = {}
	);

	[[nodiscard]] ProtocolUnitySessionState getState() const;
	[[nodiscard]] uint32_t getViolationCount() const;
	[[nodiscard]] const std::string &getClientName() const;
	[[nodiscard]] const std::string &getClientVersionLabel() const;
	[[nodiscard]] uint32_t getAuthenticatedAccountId() const;
	[[nodiscard]] const std::vector<ProtocolUnityCharacterSummary> &getCharacters() const;

	[[nodiscard]] ProtocolUnitySessionAction handleFrame(std::span<const uint8_t> frameBytes);

private:
	[[nodiscard]] ProtocolUnitySessionAction handleDecodedFrame(const ProtocolUnityFrameView &frame);
	[[nodiscard]] ProtocolUnitySessionAction handleClientHello(std::span<const uint8_t> payload);
	[[nodiscard]] ProtocolUnitySessionAction handleLoginRequest(std::span<const uint8_t> payload);
	[[nodiscard]] ProtocolUnitySessionAction handleEnterWorldRequest(std::span<const uint8_t> payload);
	[[nodiscard]] ProtocolUnitySessionAction handleMovementRequest(std::span<const uint8_t> payload);
	[[nodiscard]] ProtocolUnitySessionAction handleAttackRequest(std::span<const uint8_t> payload);
	[[nodiscard]] ProtocolUnitySessionAction handlePing(std::span<const uint8_t> payload) const;
	[[nodiscard]] ProtocolUnitySessionAction reject(std::string_view code, std::string_view detail, bool countViolation, bool closeConnection);
	[[nodiscard]] std::vector<uint8_t> buildServerHelloFrame() const;
	[[nodiscard]] std::vector<uint8_t> buildLoginResultFrame(const ProtocolUnityLoginResponse &response) const;
	[[nodiscard]] std::vector<uint8_t> buildCharacterListFrame(std::span<const ProtocolUnityCharacterSummary> characters) const;
	[[nodiscard]] std::vector<uint8_t> buildEnterWorldResultFrame(const ProtocolUnityEnterWorldResponse &response) const;
	[[nodiscard]] std::vector<uint8_t> buildErrorFrame(std::string_view code, std::string_view detail) const;
	[[nodiscard]] std::vector<uint8_t> buildPongFrame(uint64_t timestamp) const;
	void transitionTo(ProtocolUnitySessionState nextState);
	[[nodiscard]] bool shouldDisconnectAfterViolation() const;
	[[nodiscard]] bool hasCharacter(uint32_t characterId) const;

	const ProtocolUnityContract &contract;
	std::string serverName {};
	uint16_t advertisedPacketLimit = 0;
	uint8_t supportedCapabilities = 0;
	ProtocolUnitySessionState state = ProtocolUnitySessionState::Connected;
	uint32_t violationCount = 0;
	std::string clientName {};
	std::string clientVersionLabel {};
	uint8_t clientCapabilities = 0;
	uint32_t authenticatedAccountId = 0;
	std::vector<ProtocolUnityCharacterSummary> characters {};
	LoginHandler loginHandler {};
	EnterWorldHandler enterWorldHandler {};
	MovementHandler movementHandler {};
	AttackHandler attackHandler {};
};

class ProtocolUnity final : public Protocol, public PlayerProtocolObserver {
public:
	enum { SERVER_SENDS_FIRST = true };
	enum { PROTOCOL_IDENTIFIER = 0 };
	enum { USE_CHECKSUM = false };

	static const char* protocol_name() {
		return "protocol unity";
	}

	explicit ProtocolUnity(const Connection_ptr &initConnection);

	void onConnectionAccepted() override;
	void onRecvFirstMessage(NetworkMessage &msg) override;
	void onPlayerCancelWalk(const std::shared_ptr<const Player> &viewer) override;
	void onPlayerCreatureAppear(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature, const Position &pos, bool isLogin) override;
	void onPlayerCreatureMove(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature, const Position &newPos, int32_t newStackPos, const Position &oldPos, int32_t oldStackPos, bool teleport) override;
	void onPlayerCreatureTurn(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) override;
	void onPlayerCreatureHealth(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) override;
	void onPlayerCreatureBecameVisible(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) override;
	void onPlayerCreatureBecameInvisible(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) override;
	void onPlayerCreatureRemovedFromWorld(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) override;
	void onPlayerTileItemAdded(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Tile> &tile, const Position &position, const std::shared_ptr<Item> &item) override;
	void onPlayerTileItemUpdated(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Tile> &tile, const Position &position, const std::shared_ptr<Item> &item) override;
	void onPlayerTileItemRemoved(const std::shared_ptr<const Player> &viewer, const Position &position, const std::shared_ptr<Item> &item) override;
	void onPlayerInventoryUpdated(const std::shared_ptr<const Player> &viewer, uint8_t slotIndex, const std::shared_ptr<Item> &item) override;
	void onPlayerCombatResult(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &attacker, const std::shared_ptr<Creature> &target, int32_t damage, bool targetDied) override;

private:
	struct PendingMovementIntent {
		uint32_t actorId = 0;
		uint8_t direction = 0;
		ProtocolUnityWorldPosition requestedFromPosition {};
		ProtocolUnityWorldPosition expectedToPosition {};
	};

	[[nodiscard]] std::shared_ptr<ProtocolUnity> getThis();
	void parsePacket(NetworkMessage &msg) override;
	void release() override;
	void processFrame(NetworkMessage &msg);
	void sendRawFrame(std::span<const uint8_t> frameBytes) const;
	void cleanupActivePlayer();
	[[nodiscard]] ProtocolUnityEnterWorldResponse enterWorld(uint32_t accountId, uint32_t characterId);
	[[nodiscard]] std::vector<std::vector<uint8_t>> buildPendingWorldBootstrapFrames();
	[[nodiscard]] std::vector<uint8_t> buildCreatureSpawnFrame(const ProtocolUnityActorState &actor) const;
	[[nodiscard]] std::vector<uint8_t> buildCreatureMoveFrame(uint32_t actorId, const ProtocolUnityWorldPosition &fromPosition, const ProtocolUnityWorldPosition &toPosition, uint8_t direction, bool isAuthoritativeCorrection) const;
	[[nodiscard]] std::vector<uint8_t> buildCreatureHealthFrame(uint32_t actorId, uint16_t currentHealth, uint16_t maximumHealth) const;
	[[nodiscard]] std::vector<uint8_t> buildCreatureDeathFrame(uint32_t actorId, uint32_t killerActorId) const;
	[[nodiscard]] std::vector<uint8_t> buildCreatureDespawnFrame(uint32_t actorId, std::string_view reason) const;
	[[nodiscard]] std::vector<uint8_t> buildInventorySnapshotFrame(uint32_t actorId, std::span<const ProtocolUnityInventorySlotState> slots) const;
	[[nodiscard]] std::vector<uint8_t> buildInventoryUpdateFrame(uint32_t actorId, const ProtocolUnityInventorySlotState &slot) const;
	[[nodiscard]] std::vector<uint8_t> buildItemSpawnFrame(const ProtocolUnityGroundItemState &item) const;
	[[nodiscard]] std::vector<uint8_t> buildItemRemoveFrame(uint32_t itemInstanceId, std::string_view reason) const;
	[[nodiscard]] std::vector<uint8_t> buildMapSnapshotFrame(int32_t width, int32_t height, int32_t floor, std::span<const ProtocolUnityTileState> tiles) const;
	[[nodiscard]] std::vector<uint8_t> buildMovementResultFrame(uint32_t actorId, const ProtocolUnityWorldPosition &position, bool accepted, std::string_view reason) const;
	[[nodiscard]] std::vector<uint8_t> buildCombatResultFrame(uint32_t attackerId, uint32_t targetId, int16_t damage, uint16_t targetHealthAfterHit, bool targetDied, bool attackAccepted, std::string_view reason) const;
	[[nodiscard]] ProtocolUnitySessionAction moveActivePlayer(uint32_t actorId, uint8_t direction);
	[[nodiscard]] ProtocolUnitySessionAction attackTarget(uint32_t actorId, uint32_t targetId);
	[[nodiscard]] ProtocolUnityActorState captureActorState(const std::shared_ptr<Creature> &creature) const;
	[[nodiscard]] std::vector<ProtocolUnityInventorySlotState> captureInventorySnapshot() const;
	[[nodiscard]] ProtocolUnityInventorySlotState captureInventorySlotState(uint8_t slotIndex, const std::shared_ptr<Item> &item) const;
	[[nodiscard]] ProtocolUnityGroundItemState captureGroundItemState(const std::shared_ptr<Item> &item, const Position &position);
	[[nodiscard]] std::vector<ProtocolUnityTileState> captureMapSnapshotTiles(int32_t &width, int32_t &height, int32_t &floor);
	[[nodiscard]] ProtocolUnityWorldPosition captureViewportPosition(const Position &position) const;
	[[nodiscard]] uint32_t ensureGroundItemInstanceId(const std::shared_ptr<Item> &item);
	void syncVisibleGroundItems();
	void sendVisibleCreatureSpawn(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature);
	[[nodiscard]] ProtocolUnitySession &getSession();
	[[nodiscard]] static const ProtocolUnityContract &getContract();

	std::optional<ProtocolUnitySession> session {};
	std::shared_ptr<Player> activePlayer = nullptr;
	bool pendingWorldBootstrap = false;
	std::unordered_set<uint32_t> visibleActorIds {};
	std::unordered_set<uint32_t> deadActorIds {};
	std::unordered_map<const Item*, uint32_t> visibleGroundItemIds {};
	ProtocolUnityWorldPosition snapshotOrigin {};
	std::optional<PendingMovementIntent> pendingMovement {};
	uint32_t nextGroundItemInstanceId = 1;
};

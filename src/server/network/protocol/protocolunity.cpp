/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "server/network/protocol/protocolunity.hpp"

#include "account/account.hpp"
#include "account/account_repository.hpp"
#include "config/configmanager.hpp"
#include "core.hpp"
#include "creatures/monsters/monster.hpp"
#include "creatures/players/player.hpp"
#include "creatures/players/management/ban.hpp"
#include "creatures/players/management/waitlist.hpp"
#include "enums/account_group_type.hpp"
#include "enums/account_errors.hpp"
#include "enums/account_type.hpp"
#include "game/game.hpp"
#include "io/functions/iologindata_load_player.hpp"
#include "io/iologindata.hpp"
#include "items/item.hpp"
#include "items/tile.hpp"
#include "map/map_const.hpp"
#include "map/spectators.hpp"
#include "server/network/connection/connection.hpp"
#include "server/network/message/outputmessage.hpp"
#include "server/network/protocol/protocolgame.hpp"
#include "server/network/protocol/transport_codec.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <algorithm>
	#include <filesystem>
	#include <limits>
	#include <memory>

	#include <fmt/format.h>
#endif

namespace {
	constexpr uint8_t PROTOCOL_UNITY_CAPABILITY_STRUCTURED_ERRORS = 1U << 0;
	constexpr uint32_t PROTOCOL_UNITY_DISCONNECT_VIOLATION_THRESHOLD = 3;

	[[nodiscard]] std::string getAdvertisedServerName() {
		const auto configuredName = g_configManager().getString(SERVER_NAME);
		return configuredName.empty() ? "Canary ProtocolUnity" : configuredName;
	}

	[[nodiscard]] const ProtocolUnityContract &getProtocolUnityRuntimeContract() {
		static const auto contract = [] {
			const auto manifestPath = ProtocolUnityContract::locateGeneratedManifest(std::filesystem::current_path());
			if (manifestPath.empty()) {
				throw ProtocolUnityException("Could not locate SharedProtocol/TestVectors/ProtocolUnityContract.json for ProtocolUnity runtime.");
			}
			return ProtocolUnityContract::loadFromGeneratedManifest(manifestPath);
		}();
		return contract;
	}

	[[nodiscard]] ProtocolUnityWorldPosition toProtocolUnityPosition(const Position &position) {
		return ProtocolUnityWorldPosition {
			.x = static_cast<int32_t>(position.x),
			.y = static_cast<int32_t>(position.y),
			.floor = static_cast<int32_t>(position.z),
		};
	}

	[[nodiscard]] uint8_t toProtocolUnityDirection(Direction direction) {
		switch (direction) {
			case DIRECTION_NORTH:
				return 0;
			case DIRECTION_NORTHEAST:
				return 1;
			case DIRECTION_EAST:
				return 2;
			case DIRECTION_SOUTHEAST:
				return 3;
			case DIRECTION_SOUTH:
				return 4;
			case DIRECTION_SOUTHWEST:
				return 5;
			case DIRECTION_WEST:
				return 6;
			case DIRECTION_NORTHWEST:
				return 7;
			default:
				return 4;
		}
	}

	[[nodiscard]] std::optional<Direction> toCanaryDirection(uint8_t direction) {
		switch (direction) {
			case 0:
				return DIRECTION_NORTH;
			case 1:
				return DIRECTION_NORTHEAST;
			case 2:
				return DIRECTION_EAST;
			case 3:
				return DIRECTION_SOUTHEAST;
			case 4:
				return DIRECTION_SOUTH;
			case 5:
				return DIRECTION_SOUTHWEST;
			case 6:
				return DIRECTION_WEST;
			case 7:
				return DIRECTION_NORTHWEST;
			default:
				return std::nullopt;
		}
	}

	template <typename T>
	[[nodiscard]] uint16_t clampToU16(T value) {
		return static_cast<uint16_t>(std::clamp<T>(value, 0, std::numeric_limits<uint16_t>::max()));
	}

	[[nodiscard]] std::optional<ProtocolUnityCharacterSummary> loadProtocolUnityCharacterSummary(const std::string &characterName) {
		auto player = std::make_shared<Player>(std::shared_ptr<ProtocolGame> {});
		player->setName(characterName);
		if (!IOLoginDataLoad::preLoadPlayer(player, characterName)) {
			return std::nullopt;
		}

		if (!IOLoginData::loadPlayerById(player, player->getGUID(), true)) {
			return std::nullopt;
		}

		return ProtocolUnityCharacterSummary {
			.characterId = player->getGUID(),
			.name = player->getName(),
			.position = toProtocolUnityPosition(player->getLoginPosition()),
		};
	}

	[[nodiscard]] ProtocolUnityLoginResponse authenticateProtocolUnityAccount(std::string_view accountDescriptor, std::string_view secret) {
		ProtocolUnityLoginResponse response;
		Account account { std::string(accountDescriptor) };
		account.setProtocolCompat(false);

		if (account.load() != AccountErrors_t::Ok) {
			response.message = "Account could not be loaded.";
			return response;
		}

		const bool useSessionAuthentication = g_configManager().getString(AUTH_TYPE) == "session";
		const bool authenticated = useSessionAuthentication
			? account.authenticate()
			: (!secret.empty() && account.authenticate(std::string(secret)));
		if (!authenticated) {
			response.message = useSessionAuthentication ? "Session is not valid." : "Account or secret is not correct.";
			return response;
		}

		auto [players, result] = account.getAccountPlayers();
		if (result != AccountErrors_t::Ok) {
			response.message = "Character list could not be loaded.";
			return response;
		}

		const auto &contract = getProtocolUnityRuntimeContract();
		response.success = true;
		response.message = "Login accepted.";
		response.accountId = account.getID();
		response.characters.reserve(std::min<size_t>(players.size(), contract.maximumCharacterCount));

		for (const auto &[characterName, deletionTimestamp] : players) {
			if (deletionTimestamp != 0 || response.characters.size() >= contract.maximumCharacterCount) {
				continue;
			}

			const auto character = loadProtocolUnityCharacterSummary(characterName);
			if (character.has_value()) {
				response.characters.emplace_back(*character);
			}
		}

		std::ranges::sort(response.characters, [](const ProtocolUnityCharacterSummary &left, const ProtocolUnityCharacterSummary &right) {
			return left.name < right.name;
		});
		return response;
	}

}

ProtocolUnitySession::ProtocolUnitySession(
	const ProtocolUnityContract &initContract,
	std::string initServerName,
	uint16_t initAdvertisedPacketLimit,
	uint8_t initSupportedCapabilities,
	LoginHandler initLoginHandler,
	EnterWorldHandler initEnterWorldHandler,
	MovementHandler initMovementHandler
) :
	contract(initContract),
	serverName(std::move(initServerName)),
	advertisedPacketLimit(initAdvertisedPacketLimit),
	supportedCapabilities(initSupportedCapabilities),
	loginHandler(std::move(initLoginHandler)),
	enterWorldHandler(std::move(initEnterWorldHandler)),
	movementHandler(std::move(initMovementHandler)) {
	transitionTo(ProtocolUnitySessionState::AwaitingHello);
}

ProtocolUnitySessionState ProtocolUnitySession::getState() const {
	return state;
}

uint32_t ProtocolUnitySession::getViolationCount() const {
	return violationCount;
}

const std::string &ProtocolUnitySession::getClientName() const {
	return clientName;
}

const std::string &ProtocolUnitySession::getClientVersionLabel() const {
	return clientVersionLabel;
}

uint32_t ProtocolUnitySession::getAuthenticatedAccountId() const {
	return authenticatedAccountId;
}

const std::vector<ProtocolUnityCharacterSummary> &ProtocolUnitySession::getCharacters() const {
	return characters;
}

ProtocolUnitySessionAction ProtocolUnitySession::handleFrame(std::span<const uint8_t> frameBytes) {
	try {
		return handleDecodedFrame(ProtocolUnityFrameCodec::decode(frameBytes, contract));
	} catch (const ProtocolUnityUnknownOpcodeException &exception) {
		return reject("unknown_opcode", exception.what(), true, shouldDisconnectAfterViolation());
	} catch (const ProtocolUnityException &exception) {
		return reject("invalid_frame", exception.what(), true, true);
	}
}

ProtocolUnitySessionAction ProtocolUnitySession::handleDecodedFrame(const ProtocolUnityFrameView &frame) {
	switch (frame.opcode) {
		case ProtocolUnityOpcode::ClientHello:
			return handleClientHello(frame.payload);
		case ProtocolUnityOpcode::Ping:
			if (state == ProtocolUnitySessionState::AwaitingHello || state == ProtocolUnitySessionState::Connected) {
				return reject("hello_required", "ClientHello must complete before Ping.", true, false);
			}
			return handlePing(frame.payload);
		case ProtocolUnityOpcode::LoginRequest:
			if (state == ProtocolUnitySessionState::AwaitingHello || state == ProtocolUnitySessionState::Connected) {
				return reject("hello_required", "ClientHello must complete before LoginRequest.", true, false);
			}
			return handleLoginRequest(frame.payload);
		case ProtocolUnityOpcode::EnterWorldRequest:
			if (state == ProtocolUnitySessionState::AwaitingHello || state == ProtocolUnitySessionState::Connected) {
				return reject("hello_required", "ClientHello must complete before gameplay requests.", true, false);
			}
			return handleEnterWorldRequest(frame.payload);
		case ProtocolUnityOpcode::MovementRequest:
			if (state == ProtocolUnitySessionState::AwaitingHello || state == ProtocolUnitySessionState::Connected) {
				return reject("hello_required", "ClientHello must complete before gameplay requests.", true, false);
			}
			return handleMovementRequest(frame.payload);
		case ProtocolUnityOpcode::AttackRequest:
		case ProtocolUnityOpcode::PickupItemRequest:
			if (state == ProtocolUnitySessionState::AwaitingHello || state == ProtocolUnitySessionState::Connected) {
				return reject("hello_required", "ClientHello must complete before gameplay requests.", true, false);
			}
			return reject("invalid_state", "Gameplay requests are blocked until authentication and world entry exist.", true, false);
		default:
			return reject("unsupported_opcode", fmt::format("Opcode '{}' is not implemented yet.", contract.requireOpcode(frame.opcode).name), false, false);
	}
}

ProtocolUnitySessionAction ProtocolUnitySession::handleClientHello(std::span<const uint8_t> payload) {
	if (state != ProtocolUnitySessionState::AwaitingHello && state != ProtocolUnitySessionState::Connected) {
		return reject("duplicate_hello", "ClientHello was already accepted for this connection.", true, false);
	}

	ProtocolUnityPacketReader reader(payload, contract);
	const auto helloClientName = reader.readString();
	const auto helloClientVersion = reader.readString();
	const auto helloCapabilities = reader.readByte();
	reader.expectFullyConsumed();

	if (helloClientName.empty()) {
		return reject("invalid_hello", "ClientHello requires a non-empty client name.", true, false);
	}

	clientName = helloClientName;
	clientVersionLabel = helloClientVersion;
	clientCapabilities = helloCapabilities;
	transitionTo(ProtocolUnitySessionState::HelloAccepted);

	ProtocolUnitySessionAction action;
	action.outboundFrames.emplace_back(buildServerHelloFrame());
	transitionTo(ProtocolUnitySessionState::AwaitingAuthentication);
	return action;
}

ProtocolUnitySessionAction ProtocolUnitySession::handleLoginRequest(std::span<const uint8_t> payload) {
	if (state != ProtocolUnitySessionState::AwaitingAuthentication) {
		return reject("invalid_state", "LoginRequest is only valid while awaiting authentication.", true, false);
	}

	if (!loginHandler) {
		return reject("authentication_unavailable", "ProtocolUnity authentication is not connected yet.", false, false);
	}

	ProtocolUnityPacketReader reader(payload, contract);
	const auto accountDescriptor = reader.readString();
	const auto secret = reader.readString();
	reader.expectFullyConsumed();

	if (accountDescriptor.empty()) {
		return reject("invalid_login", "LoginRequest requires a non-empty account descriptor.", true, false);
	}

	const auto response = loginHandler(accountDescriptor, secret);

	ProtocolUnitySessionAction action;
	action.outboundFrames.emplace_back(buildLoginResultFrame(response));
	if (!response.success) {
		return action;
	}

	authenticatedAccountId = response.accountId;
	characters = response.characters;
	transitionTo(ProtocolUnitySessionState::Authenticated);
	action.outboundFrames.emplace_back(buildCharacterListFrame(characters));
	return action;
}

ProtocolUnitySessionAction ProtocolUnitySession::handleEnterWorldRequest(std::span<const uint8_t> payload) {
	if (state != ProtocolUnitySessionState::Authenticated && state != ProtocolUnitySessionState::CharacterSelected) {
		return reject("invalid_state", "EnterWorldRequest is only valid after a successful login.", true, false);
	}

	if (!enterWorldHandler) {
		return reject("world_entry_unavailable", "ProtocolUnity world entry is not connected yet.", false, false);
	}

	ProtocolUnityPacketReader reader(payload, contract);
	const auto characterId = reader.readU32();
	reader.expectFullyConsumed();

	if (!hasCharacter(characterId)) {
		return reject("character_not_found", "Character is not available for the authenticated account.", true, false);
	}

	auto response = enterWorldHandler(authenticatedAccountId, characterId);
	if (response.selectionAccepted) {
		transitionTo(response.success ? ProtocolUnitySessionState::InWorld : ProtocolUnitySessionState::CharacterSelected);
	}

	ProtocolUnitySessionAction action;
	action.outboundFrames.emplace_back(buildEnterWorldResultFrame(response));
	return action;
}

ProtocolUnitySessionAction ProtocolUnitySession::handleMovementRequest(std::span<const uint8_t> payload) {
	if (state != ProtocolUnitySessionState::InWorld) {
		return reject("invalid_state", "MovementRequest is only valid after world entry succeeds.", true, false);
	}

	if (!movementHandler) {
		return reject("movement_unavailable", "ProtocolUnity movement is not connected yet.", false, false);
	}

	ProtocolUnityPacketReader reader(payload, contract);
	const auto actorId = reader.readU32();
	const auto direction = reader.readByte();
	reader.expectFullyConsumed();
	return movementHandler(actorId, direction);
}

ProtocolUnitySessionAction ProtocolUnitySession::handlePing(std::span<const uint8_t> payload) const {
	ProtocolUnityPacketReader reader(payload, contract);
	const auto timestamp = reader.readU64();
	reader.expectFullyConsumed();

	ProtocolUnitySessionAction action;
	action.outboundFrames.emplace_back(buildPongFrame(timestamp));
	return action;
}

ProtocolUnitySessionAction ProtocolUnitySession::reject(std::string_view code, std::string_view detail, bool countViolation, bool closeConnection) {
	if (countViolation) {
		++violationCount;
	}

	ProtocolUnitySessionAction action;
	action.outboundFrames.emplace_back(buildErrorFrame(code, detail));
	action.closeConnection = closeConnection || shouldDisconnectAfterViolation();
	if (action.closeConnection) {
		transitionTo(ProtocolUnitySessionState::Closing);
	}
	return action;
}

std::vector<uint8_t> ProtocolUnitySession::buildServerHelloFrame() const {
	ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::ServerHello);
	writer.writeString(serverName);
	writer.writeU16(contract.protocolVersion);
	writer.writeU16(advertisedPacketLimit);
	writer.writeByte(supportedCapabilities);
	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnitySession::buildLoginResultFrame(const ProtocolUnityLoginResponse &response) const {
	ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::LoginResult);
	writer.writeByte(response.success ? 1 : 0);
	writer.writeString(response.message);
	writer.writeU32(response.accountId);
	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnitySession::buildCharacterListFrame(std::span<const ProtocolUnityCharacterSummary> charactersToEncode) const {
	ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::CharacterList);
	writer.writeByte(static_cast<uint8_t>(std::min<size_t>(charactersToEncode.size(), contract.maximumCharacterCount)));

	for (size_t index = 0; index < charactersToEncode.size() && index < contract.maximumCharacterCount; ++index) {
		const auto &character = charactersToEncode[index];
		writer.writeU32(character.characterId);
		writer.writeString(character.name);
		writer.writeI32(character.position.x);
		writer.writeI32(character.position.y);
		writer.writeI32(character.position.floor);
	}

	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnitySession::buildEnterWorldResultFrame(const ProtocolUnityEnterWorldResponse &response) const {
	ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::EnterWorldResult);
	writer.writeByte(response.success ? 1 : 0);
	writer.writeString(response.message);
	writer.writeU32(response.actorId);
	writer.writeI32(response.spawnPosition.x);
	writer.writeI32(response.spawnPosition.y);
	writer.writeI32(response.spawnPosition.floor);
	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnitySession::buildErrorFrame(std::string_view code, std::string_view detail) const {
	ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::ErrorMessage);
	writer.writeString(code);
	writer.writeString(detail);
	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnitySession::buildPongFrame(uint64_t timestamp) const {
	ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::Pong);
	writer.writeU64(timestamp);
	return writer.finalize();
}

void ProtocolUnitySession::transitionTo(ProtocolUnitySessionState nextState) {
	state = nextState;
}

bool ProtocolUnitySession::shouldDisconnectAfterViolation() const {
	return violationCount >= PROTOCOL_UNITY_DISCONNECT_VIOLATION_THRESHOLD;
}

bool ProtocolUnitySession::hasCharacter(uint32_t characterId) const {
	return std::ranges::any_of(characters, [characterId](const ProtocolUnityCharacterSummary &character) {
		return character.characterId == characterId;
	});
}

ProtocolUnity::ProtocolUnity(const Connection_ptr &initConnection) :
	Protocol(initConnection) {
	setRawMessages(true);
	if (initConnection) {
		initConnection->setTransportCodec(TransportCodecs::protocolUnity(), InitialTransportState::ResolvedFromPrelude);
	}
}

std::shared_ptr<ProtocolUnity> ProtocolUnity::getThis() {
	return std::static_pointer_cast<ProtocolUnity>(shared_from_this());
}

void ProtocolUnity::onConnectionAccepted() {
	if (const auto connection = getConnection()) {
		connection->setTransportCodec(TransportCodecs::protocolUnity(), InitialTransportState::ResolvedFromPrelude);
	}
}

void ProtocolUnity::onRecvFirstMessage(NetworkMessage &msg) {
	processFrame(msg);
}

void ProtocolUnity::parsePacket(NetworkMessage &msg) {
	processFrame(msg);
}

void ProtocolUnity::release() {
	cleanupActivePlayer();
	Protocol::release();
}

void ProtocolUnity::onPlayerCancelWalk(const std::shared_ptr<const Player> &viewer) {
	if (!activePlayer || !viewer || viewer != activePlayer || !pendingMovement.has_value()) {
		return;
	}

	sendRawFrame(buildMovementResultFrame(pendingMovement->actorId, captureViewportPosition(activePlayer->getPosition()), false, "walk_cancelled"));
	pendingMovement.reset();
}

void ProtocolUnity::onPlayerCreatureAppear(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature, const Position &, bool) {
	sendVisibleCreatureSpawn(viewer, creature);
}

void ProtocolUnity::onPlayerCreatureMove(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature, const Position &newPos, int32_t, const Position &oldPos, int32_t, bool) {
	if (!activePlayer || !viewer || !creature || viewer != activePlayer) {
		return;
	}

	if (creature != activePlayer) {
		sendVisibleCreatureSpawn(viewer, creature);
		if (!visibleActorIds.contains(creature->getID())) {
			return;
		}
	}

	const auto fromPosition = captureViewportPosition(oldPos);
	const auto toPosition = captureViewportPosition(newPos);
	const auto direction = toProtocolUnityDirection(creature->getDirection());

	if (creature == activePlayer && pendingMovement.has_value() && pendingMovement->actorId == creature->getID()) {
		sendRawFrame(buildMovementResultFrame(creature->getID(), toPosition, true, ""));
		pendingMovement.reset();
	}

	sendRawFrame(buildCreatureMoveFrame(creature->getID(), fromPosition, toPosition, direction, false));
}

void ProtocolUnity::onPlayerCreatureTurn(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) {
	if (!activePlayer || !viewer || !creature || viewer != activePlayer) {
		return;
	}

	if (creature != activePlayer) {
		sendVisibleCreatureSpawn(viewer, creature);
		if (!visibleActorIds.contains(creature->getID())) {
			return;
		}
	}

	const auto position = captureViewportPosition(creature->getPosition());
	sendRawFrame(buildCreatureMoveFrame(creature->getID(), position, position, toProtocolUnityDirection(creature->getDirection()), false));
}

void ProtocolUnity::onPlayerCreatureHealth(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) {
	if (!activePlayer || !viewer || !creature || viewer != activePlayer) {
		return;
	}

	if (creature != activePlayer && !visibleActorIds.contains(creature->getID())) {
		return;
	}

	sendRawFrame(buildCreatureHealthFrame(creature->getID(), clampToU16(creature->getHealth()), clampToU16(creature->getMaxHealth())));
}

void ProtocolUnity::onPlayerCreatureBecameVisible(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) {
	sendVisibleCreatureSpawn(viewer, creature);
}

void ProtocolUnity::onPlayerCreatureBecameInvisible(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) {
	if (!activePlayer || !viewer || !creature || viewer != activePlayer || creature == activePlayer) {
		return;
	}

	if (!creature->isRemoved() && viewer->canSeeCreature(creature)) {
		return;
	}

	if (visibleActorIds.erase(creature->getID()) > 0) {
		sendRawFrame(buildCreatureDespawnFrame(creature->getID(), creature->isRemoved() ? "removed" : "out_of_view"));
	}
}

void ProtocolUnity::processFrame(NetworkMessage &msg) {
	try {
		auto &activeSession = getSession();
		auto action = activeSession.handleFrame(std::span<const uint8_t>(msg.getBuffer(), msg.getLength()));
		for (const auto &frame : action.outboundFrames) {
			sendRawFrame(frame);
		}
		for (const auto &frame : buildPendingWorldBootstrapFrames()) {
			sendRawFrame(frame);
		}
		if (action.closeConnection) {
			disconnect();
		}
	} catch (const std::exception &exception) {
		g_logger().error("[ProtocolUnity::processFrame] - {}", exception.what());
		disconnect();
	}
}

void ProtocolUnity::sendRawFrame(std::span<const uint8_t> frameBytes) const {
	auto output = OutputMessagePool::getOutputMessage();
	output->addBytes(reinterpret_cast<const char*>(frameBytes.data()), frameBytes.size());
	send(output);
}

void ProtocolUnity::cleanupActivePlayer() {
	pendingWorldBootstrap = false;
	visibleActorIds.clear();
	pendingMovement.reset();

	if (!activePlayer) {
		return;
	}

	activePlayer->clearProtocolObserver();

	if (!activePlayer->isRemoved()) {
		g_game().removeCreature(activePlayer, true);
	}

	activePlayer.reset();
}

ProtocolUnityEnterWorldResponse ProtocolUnity::enterWorld(uint32_t accountId, uint32_t characterId) {
	ProtocolUnityEnterWorldResponse response;
	if (accountId == 0 || characterId == 0) {
		response.message = "Character selection is invalid.";
		return response;
	}

	if (activePlayer) {
		response.selectionAccepted = true;
		response.message = "Character is already in the world.";
		return response;
	}

	const auto characterName = IOLoginData::getNameByGuid(characterId);
	if (characterName.empty()) {
		response.message = "Character was not found.";
		return response;
	}

	if (!g_accountRepository().getCharacterByAccountIdAndName(accountId, characterName)) {
		response.message = "Character does not belong to the authenticated account.";
		return response;
	}

	if (g_game().getPlayerByName(characterName)) {
		response.selectionAccepted = true;
		response.message = "Character is already online.";
		return response;
	}

	auto player = std::make_shared<Player>(std::shared_ptr<ProtocolGame> {});
	player->setName(characterName);
	player->setID();

	if (!IOLoginDataLoad::preLoadPlayer(player, characterName)) {
		response.message = "Character could not be loaded.";
		return response;
	}

	if (IOBan::isPlayerNamelocked(player->getGUID())) {
		response.selectionAccepted = true;
		response.message = "Your character has been namelocked.";
		return response;
	}

	if (g_game().getGameState() == GAME_STATE_CLOSING && !player->hasFlag(PlayerFlags_t::CanAlwaysLogin)) {
		response.selectionAccepted = true;
		response.message = "The game is just going down. Please try again later.";
		return response;
	}

	if (g_game().getGameState() == GAME_STATE_CLOSED && !player->hasFlag(PlayerFlags_t::CanAlwaysLogin)) {
		const auto maintainMessage = g_configManager().getString(MAINTAIN_MODE_MESSAGE);
		response.selectionAccepted = true;
		response.message = maintainMessage.empty() ? "Server is currently closed. Please try again later." : maintainMessage;
		return response;
	}

	if (g_configManager().getBoolean(ONLY_PREMIUM_ACCOUNT) && !player->isPremium() && (player->getGroup()->id < GROUP_TYPE_GAMEMASTER || player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER)) {
		response.selectionAccepted = true;
		response.message = "Your premium time for this account is out.";
		return response;
	}

	const auto onlineCount = g_game().getPlayersByAccount(player->getAccount()).size();
	const auto maxOnline = g_configManager().getNumber(MAX_PLAYERS_PER_ACCOUNT);
	if (player->getAccountType() < ACCOUNT_TYPE_GAMEMASTER && onlineCount >= maxOnline) {
		response.selectionAccepted = true;
		response.message = fmt::format("You may only login with {} character{} of your account at the same time.", maxOnline, maxOnline > 1 ? "s" : "");
		return response;
	}

	if (!player->hasFlag(PlayerFlags_t::CannotBeBanned)) {
		BanInfo banInfo;
		if (IOBan::isAccountBanned(accountId, banInfo)) {
			response.selectionAccepted = true;
			response.message = "The authenticated account is banned.";
			return response;
		}
	}

	if (!WaitingList::getInstance().clientLogin(player)) {
		response.selectionAccepted = true;
		response.message = "Too many players are online right now.";
		return response;
	}

	if (!IOLoginData::loadPlayerById(player, characterId, false)) {
		response.message = "Character could not be loaded.";
		return response;
	}

	const bool placedCreature = g_game().placeCreature(player, player->getLoginPosition()) || g_game().placeCreature(player, player->getTemplePosition(), false, true);
	if (!placedCreature) {
		response.selectionAccepted = true;
		response.message = "Temple position is wrong. Please contact an administrator.";
		return response;
	}

	player->setLoginProtection(g_configManager().getNumber(LOGIN_PROTECTION_TIME));
	player->setProtocolObserver(getThis());

	activePlayer = player;
	pendingWorldBootstrap = true;
	visibleActorIds.clear();
	snapshotOrigin = {
		.x = static_cast<int32_t>(player->getPosition().x) - MAP_MAX_CLIENT_VIEW_PORT_X,
		.y = static_cast<int32_t>(player->getPosition().y) - MAP_MAX_CLIENT_VIEW_PORT_Y,
		.floor = static_cast<int32_t>(player->getPosition().z),
	};

	response.success = true;
	response.selectionAccepted = true;
	response.actorId = player->getID();
	response.spawnPosition = captureViewportPosition(player->getPosition());
	response.message = "Entered world.";
	return response;
}

std::vector<std::vector<uint8_t>> ProtocolUnity::buildPendingWorldBootstrapFrames() {
	if (!pendingWorldBootstrap || !activePlayer || activePlayer->isRemoved()) {
		return {};
	}

	pendingWorldBootstrap = false;

	int32_t width = 0;
	int32_t height = 0;
	int32_t floor = 0;
	const auto tiles = captureMapSnapshotTiles(width, height, floor);
	const auto inventory = captureInventorySnapshot();

	std::vector<std::vector<uint8_t>> frames;
	frames.emplace_back(buildMapSnapshotFrame(width, height, floor, tiles));
	frames.emplace_back(buildInventorySnapshotFrame(activePlayer->getID(), inventory));
	frames.emplace_back(buildCreatureSpawnFrame(captureActorState(activePlayer)));

	std::unordered_set<uint32_t> nextVisibleActorIds;
	for (const auto &creature : Spectators().find<Creature>(activePlayer->getPosition(), false, MAP_MAX_CLIENT_VIEW_PORT_X, MAP_MAX_CLIENT_VIEW_PORT_X, MAP_MAX_CLIENT_VIEW_PORT_Y, MAP_MAX_CLIENT_VIEW_PORT_Y)) {
		if (!creature || creature == activePlayer || creature->isRemoved()) {
			continue;
		}

		nextVisibleActorIds.insert(creature->getID());
		frames.emplace_back(buildCreatureSpawnFrame(captureActorState(creature)));
	}

	for (const auto actorId : visibleActorIds) {
		if (!nextVisibleActorIds.contains(actorId)) {
			frames.emplace_back(buildCreatureDespawnFrame(actorId, "out_of_view"));
		}
	}

	visibleActorIds = std::move(nextVisibleActorIds);
	return frames;
}

std::vector<uint8_t> ProtocolUnity::buildCreatureSpawnFrame(const ProtocolUnityActorState &actor) const {
	ProtocolUnityPacketWriter writer(getContract(), ProtocolUnityOpcode::CreatureSpawn);
	writer.writeU32(actor.actorId);
	writer.writeString(actor.name);
	writer.writeByte(static_cast<uint8_t>(actor.kind));
	writer.writeI32(actor.position.x);
	writer.writeI32(actor.position.y);
	writer.writeI32(actor.position.floor);
	writer.writeByte(actor.direction);
	writer.writeU16(actor.health);
	writer.writeU16(actor.maxHealth);
	writer.writeU16(actor.mana);
	writer.writeU16(actor.maxMana);
	writer.writeByte(static_cast<uint8_t>(actor.disposition));
	writer.writeByte(actor.isDead ? 1 : 0);
	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnity::buildCreatureMoveFrame(uint32_t actorId, const ProtocolUnityWorldPosition &fromPosition, const ProtocolUnityWorldPosition &toPosition, uint8_t direction, bool isAuthoritativeCorrection) const {
	ProtocolUnityPacketWriter writer(getContract(), ProtocolUnityOpcode::CreatureMove);
	writer.writeU32(actorId);
	writer.writeI32(fromPosition.x);
	writer.writeI32(fromPosition.y);
	writer.writeI32(fromPosition.floor);
	writer.writeI32(toPosition.x);
	writer.writeI32(toPosition.y);
	writer.writeI32(toPosition.floor);
	writer.writeByte(direction);
	writer.writeByte(isAuthoritativeCorrection ? 1 : 0);
	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnity::buildCreatureHealthFrame(uint32_t actorId, uint16_t currentHealth, uint16_t maximumHealth) const {
	ProtocolUnityPacketWriter writer(getContract(), ProtocolUnityOpcode::CreatureHealth);
	writer.writeU32(actorId);
	writer.writeU16(currentHealth);
	writer.writeU16(maximumHealth);
	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnity::buildCreatureDespawnFrame(uint32_t actorId, std::string_view reason) const {
	ProtocolUnityPacketWriter writer(getContract(), ProtocolUnityOpcode::CreatureDespawn);
	writer.writeU32(actorId);
	writer.writeString(reason);
	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnity::buildInventorySnapshotFrame(uint32_t actorId, std::span<const ProtocolUnityInventorySlotState> slots) const {
	ProtocolUnityPacketWriter writer(getContract(), ProtocolUnityOpcode::InventorySnapshot);
	writer.writeU32(actorId);
	writer.writeByte(static_cast<uint8_t>(std::min<size_t>(slots.size(), getContract().maximumInventorySlots)));

	for (size_t index = 0; index < slots.size() && index < getContract().maximumInventorySlots; ++index) {
		const auto &slot = slots[index];
		writer.writeByte(slot.slotIndex);
		writer.writeU16(slot.itemTypeId);
		writer.writeString(slot.name);
		writer.writeU16(slot.quantity);
		writer.writeU16(slot.stackLimit);
	}

	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnity::buildMapSnapshotFrame(int32_t width, int32_t height, int32_t floor, std::span<const ProtocolUnityTileState> tiles) const {
	ProtocolUnityPacketWriter writer(getContract(), ProtocolUnityOpcode::MapSnapshot);
	writer.writeI32(width);
	writer.writeI32(height);
	writer.writeI32(floor);
	writer.writeU16(static_cast<uint16_t>(std::min<size_t>(tiles.size(), std::numeric_limits<uint16_t>::max())));

	for (size_t index = 0; index < tiles.size() && index < std::numeric_limits<uint16_t>::max(); ++index) {
		const auto &tile = tiles[index];
		writer.writeI16(tile.localX);
		writer.writeI16(tile.localY);
		writer.writeByte(tile.blocked ? 1 : 0);
		writer.writeByte(tile.height);
		writer.writeU16(tile.tileId);
	}

	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnity::buildMovementResultFrame(uint32_t actorId, const ProtocolUnityWorldPosition &position, bool accepted, std::string_view reason) const {
	ProtocolUnityPacketWriter writer(getContract(), ProtocolUnityOpcode::MovementResult);
	writer.writeU32(actorId);
	writer.writeI32(position.x);
	writer.writeI32(position.y);
	writer.writeI32(position.floor);
	writer.writeByte(accepted ? 1 : 0);
	writer.writeString(reason);
	return writer.finalize();
}

ProtocolUnitySessionAction ProtocolUnity::moveActivePlayer(uint32_t actorId, uint8_t direction) {
	ProtocolUnitySessionAction action;
	if (!activePlayer || activePlayer->isRemoved()) {
		action.outboundFrames.emplace_back(buildMovementResultFrame(actorId, {}, false, "player_unavailable"));
		return action;
	}

	if (activePlayer->getID() != actorId) {
		action.outboundFrames.emplace_back(buildMovementResultFrame(actorId, captureViewportPosition(activePlayer->getPosition()), false, "actor_mismatch"));
		return action;
	}

	const auto canaryDirection = toCanaryDirection(direction);
	if (!canaryDirection.has_value()) {
		action.outboundFrames.emplace_back(buildMovementResultFrame(actorId, captureViewportPosition(activePlayer->getPosition()), false, "invalid_direction"));
		return action;
	}

	if (pendingMovement.has_value()) {
		action.outboundFrames.emplace_back(buildMovementResultFrame(actorId, captureViewportPosition(activePlayer->getPosition()), false, "movement_pending"));
		return action;
	}

	pendingMovement = PendingMovementIntent {
		.actorId = actorId,
		.direction = direction,
	};
	g_game().playerMove(activePlayer->getID(), *canaryDirection);
	return action;
}

ProtocolUnityActorState ProtocolUnity::captureActorState(const std::shared_ptr<Creature> &creature) const {
	ProtocolUnityActorState actor;
	if (!creature) {
		return actor;
	}

	actor.actorId = creature->getID();
	actor.name = creature->getName();
	actor.kind = creature->getMonster() ? ProtocolUnityActorKind::Monster : ProtocolUnityActorKind::Player;
	actor.position = captureViewportPosition(creature->getPosition());
	actor.direction = toProtocolUnityDirection(creature->getDirection());
	actor.health = clampToU16(creature->getHealth());
	actor.maxHealth = clampToU16(creature->getMaxHealth());
	if (const auto &player = creature->getPlayer()) {
		actor.mana = clampToU16(player->getMana());
		actor.maxMana = clampToU16(player->getMaxMana());
		actor.disposition = ProtocolUnityActorDisposition::Friendly;
	} else {
		actor.mana = 0;
		actor.maxMana = 0;
		actor.disposition = creature->getMonster() ? ProtocolUnityActorDisposition::Hostile : ProtocolUnityActorDisposition::Neutral;
	}
	actor.isDead = creature->getHealth() <= 0;
	return actor;
}

std::vector<ProtocolUnityInventorySlotState> ProtocolUnity::captureInventorySnapshot() const {
	std::vector<ProtocolUnityInventorySlotState> slots;
	if (!activePlayer || activePlayer->isRemoved()) {
		return slots;
	}

	slots.reserve(CONST_SLOT_LAST - CONST_SLOT_FIRST + 1);
	for (uint8_t slot = CONST_SLOT_FIRST; slot <= CONST_SLOT_LAST; ++slot) {
		const auto &item = activePlayer->getInventoryItem(static_cast<Slots_t>(slot));
		if (!item) {
			continue;
		}

		slots.push_back(ProtocolUnityInventorySlotState {
			.slotIndex = slot,
			.itemTypeId = item->getID(),
			.name = item->getName(),
			.quantity = static_cast<uint16_t>(item->isStackable() ? item->getItemCount() : 1),
			.stackLimit = item->getStackSize(),
		});
	}

	return slots;
}

std::vector<ProtocolUnityTileState> ProtocolUnity::captureMapSnapshotTiles(int32_t &width, int32_t &height, int32_t &floor) {
	std::vector<ProtocolUnityTileState> tiles;
	if (!activePlayer || activePlayer->isRemoved()) {
		width = 0;
		height = 0;
		floor = 0;
		return tiles;
	}

	width = (MAP_MAX_CLIENT_VIEW_PORT_X + 1) * 2;
	height = (MAP_MAX_CLIENT_VIEW_PORT_Y + 1) * 2;
	floor = activePlayer->getPosition().z;

	const auto originX = static_cast<int32_t>(activePlayer->getPosition().x) - MAP_MAX_CLIENT_VIEW_PORT_X;
	const auto originY = static_cast<int32_t>(activePlayer->getPosition().y) - MAP_MAX_CLIENT_VIEW_PORT_Y;
	snapshotOrigin = { .x = originX, .y = originY, .floor = floor };

	tiles.reserve(static_cast<size_t>(width * height));
	for (int16_t localY = 0; localY < height; ++localY) {
		for (int16_t localX = 0; localX < width; ++localX) {
			const auto absoluteX = originX + localX;
			const auto absoluteY = originY + localY;

			std::shared_ptr<Tile> tile = nullptr;
			if (absoluteX >= 0 && absoluteY >= 0 && absoluteX <= std::numeric_limits<uint16_t>::max() && absoluteY <= std::numeric_limits<uint16_t>::max()) {
				tile = g_game().map.getTile(static_cast<uint16_t>(absoluteX), static_cast<uint16_t>(absoluteY), static_cast<uint8_t>(floor));
			}

			const auto blocked = !tile || tile->hasFlag(TILESTATE_BLOCKSOLID | TILESTATE_BLOCKPATH);
			const auto tileId = tile && tile->getGround() ? tile->getGround()->getID() : 0;
			const auto tileHeight = tile && tile->hasFlag(TILESTATE_HASHEIGHT) ? 1U : 0U;
			tiles.push_back(ProtocolUnityTileState {
				.localX = localX,
				.localY = localY,
				.blocked = blocked,
				.height = static_cast<uint8_t>(tileHeight),
				.tileId = static_cast<uint16_t>(tileId),
			});
		}
	}

	return tiles;
}

ProtocolUnityWorldPosition ProtocolUnity::captureViewportPosition(const Position &position) const {
	return ProtocolUnityWorldPosition {
		.x = static_cast<int32_t>(position.x) - snapshotOrigin.x,
		.y = static_cast<int32_t>(position.y) - snapshotOrigin.y,
		.floor = static_cast<int32_t>(position.z),
	};
}

void ProtocolUnity::sendVisibleCreatureSpawn(const std::shared_ptr<const Player> &viewer, const std::shared_ptr<Creature> &creature) {
	if (!activePlayer || !viewer || !creature || viewer != activePlayer || creature == activePlayer || creature->isRemoved()) {
		return;
	}

	if (!viewer->canSeeCreature(creature)) {
		return;
	}

	if (visibleActorIds.insert(creature->getID()).second) {
		sendRawFrame(buildCreatureSpawnFrame(captureActorState(creature)));
	}
}

ProtocolUnitySession &ProtocolUnity::getSession() {
	if (!session.has_value()) {
		const auto &contract = getContract();
		const auto advertisedLimit = static_cast<uint16_t>(std::min<uint32_t>(contract.maximumPacketSize, INPUTMESSAGE_MAXSIZE));
		session.emplace(
			contract,
			getAdvertisedServerName(),
			advertisedLimit,
			PROTOCOL_UNITY_CAPABILITY_STRUCTURED_ERRORS,
			[](std::string_view accountDescriptor, std::string_view secret) {
				return authenticateProtocolUnityAccount(accountDescriptor, secret);
			},
			[this](uint32_t accountId, uint32_t characterId) {
				return enterWorld(accountId, characterId);
			},
			[this](uint32_t actorId, uint8_t direction) {
				return moveActivePlayer(actorId, direction);
			}
		);
	}

	return *session;
}

const ProtocolUnityContract &ProtocolUnity::getContract() {
	return getProtocolUnityRuntimeContract();
}

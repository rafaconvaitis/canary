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
#include "creatures/players/player.hpp"
#include "enums/account_errors.hpp"
#include "io/functions/iologindata_load_player.hpp"
#include "io/iologindata.hpp"
#include "server/network/connection/connection.hpp"
#include "server/network/message/outputmessage.hpp"
#include "server/network/protocol/protocolgame.hpp"
#include "server/network/protocol/transport_codec.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <algorithm>
	#include <filesystem>
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
			.x = position.x,
			.y = position.y,
			.floor = position.z,
		};
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

	[[nodiscard]] ProtocolUnityEnterWorldResponse prepareProtocolUnityWorldEntry(uint32_t accountId, uint32_t characterId) {
		ProtocolUnityEnterWorldResponse response;
		if (accountId == 0 || characterId == 0) {
			response.message = "Character selection is invalid.";
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

		auto player = std::make_shared<Player>(std::shared_ptr<ProtocolGame> {});
		if (!IOLoginData::loadPlayerById(player, characterId, true)) {
			response.message = "Character could not be loaded.";
			return response;
		}

		response.selectionAccepted = true;
		response.actorId = characterId;
		response.spawnPosition = toProtocolUnityPosition(player->getLoginPosition());
		response.message = "ProtocolUnity world entry is not connected to a live Player session yet.";
		return response;
	}
}

ProtocolUnitySession::ProtocolUnitySession(
	const ProtocolUnityContract &initContract,
	std::string initServerName,
	uint16_t initAdvertisedPacketLimit,
	uint8_t initSupportedCapabilities,
	LoginHandler initLoginHandler,
	EnterWorldHandler initEnterWorldHandler
) :
	contract(initContract),
	serverName(std::move(initServerName)),
	advertisedPacketLimit(initAdvertisedPacketLimit),
	supportedCapabilities(initSupportedCapabilities),
	loginHandler(std::move(initLoginHandler)),
	enterWorldHandler(std::move(initEnterWorldHandler)) {
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
	writer.writeU16(static_cast<uint16_t>(std::min<size_t>(charactersToEncode.size(), contract.maximumCharacterCount)));

	for (size_t index = 0; index < charactersToEncode.size() && index < contract.maximumCharacterCount; ++index) {
		const auto &character = charactersToEncode[index];
		writer.writeU32(character.characterId);
		writer.writeString(character.name);
		writer.writeU32(character.position.x);
		writer.writeU32(character.position.y);
		writer.writeU32(character.position.floor);
	}

	return writer.finalize();
}

std::vector<uint8_t> ProtocolUnitySession::buildEnterWorldResultFrame(const ProtocolUnityEnterWorldResponse &response) const {
	ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::EnterWorldResult);
	writer.writeByte(response.success ? 1 : 0);
	writer.writeString(response.message);
	writer.writeU32(response.actorId);
	writer.writeU32(response.spawnPosition.x);
	writer.writeU32(response.spawnPosition.y);
	writer.writeU32(response.spawnPosition.floor);
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

void ProtocolUnity::processFrame(NetworkMessage &msg) {
	try {
		auto &activeSession = getSession();
		auto action = activeSession.handleFrame(std::span<const uint8_t>(msg.getBuffer(), msg.getLength()));
		for (const auto &frame : action.outboundFrames) {
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
			[](uint32_t accountId, uint32_t characterId) {
				return prepareProtocolUnityWorldEntry(accountId, characterId);
			}
		);
	}

	return *session;
}

const ProtocolUnityContract &ProtocolUnity::getContract() {
	return getProtocolUnityRuntimeContract();
}

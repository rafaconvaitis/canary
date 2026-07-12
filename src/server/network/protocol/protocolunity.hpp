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

#ifndef USE_PRECOMPILED_HEADERS
	#include <optional>
	#include <functional>
	#include <span>
	#include <string>
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
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t floor = 0;
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

struct ProtocolUnitySessionAction {
	std::vector<std::vector<uint8_t>> outboundFrames {};
	bool closeConnection = false;
};

class ProtocolUnitySession {
public:
	using LoginHandler = std::function<ProtocolUnityLoginResponse(std::string_view accountDescriptor, std::string_view secret)>;
	using EnterWorldHandler = std::function<ProtocolUnityEnterWorldResponse(uint32_t accountId, uint32_t characterId)>;

	ProtocolUnitySession(
		const ProtocolUnityContract &initContract,
		std::string initServerName,
		uint16_t initAdvertisedPacketLimit,
		uint8_t initSupportedCapabilities = 1,
		LoginHandler initLoginHandler = {},
		EnterWorldHandler initEnterWorldHandler = {}
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
};

class ProtocolUnity final : public Protocol {
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

private:
	void parsePacket(NetworkMessage &msg) override;
	void processFrame(NetworkMessage &msg);
	void sendRawFrame(std::span<const uint8_t> frameBytes) const;
	[[nodiscard]] ProtocolUnitySession &getSession();
	[[nodiscard]] static const ProtocolUnityContract &getContract();

	std::optional<ProtocolUnitySession> session {};
};

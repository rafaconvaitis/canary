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
	#include <span>
	#include <string>
	#include <vector>
#endif

enum class ProtocolUnitySessionState : uint8_t {
	Connected,
	AwaitingHello,
	HelloAccepted,
	AwaitingAuthentication,
	Closing,
	Closed,
};

struct ProtocolUnitySessionAction {
	std::vector<std::vector<uint8_t>> outboundFrames {};
	bool closeConnection = false;
};

class ProtocolUnitySession {
public:
	ProtocolUnitySession(
		const ProtocolUnityContract &initContract,
		std::string initServerName,
		uint16_t initAdvertisedPacketLimit,
		uint8_t initSupportedCapabilities = 1
	);

	[[nodiscard]] ProtocolUnitySessionState getState() const;
	[[nodiscard]] uint32_t getViolationCount() const;
	[[nodiscard]] const std::string &getClientName() const;
	[[nodiscard]] const std::string &getClientVersionLabel() const;

	[[nodiscard]] ProtocolUnitySessionAction handleFrame(std::span<const uint8_t> frameBytes);

private:
	[[nodiscard]] ProtocolUnitySessionAction handleDecodedFrame(const ProtocolUnityFrameView &frame);
	[[nodiscard]] ProtocolUnitySessionAction handleClientHello(std::span<const uint8_t> payload);
	[[nodiscard]] ProtocolUnitySessionAction handlePing(std::span<const uint8_t> payload) const;
	[[nodiscard]] ProtocolUnitySessionAction reject(std::string_view code, std::string_view detail, bool countViolation, bool closeConnection);
	[[nodiscard]] std::vector<uint8_t> buildServerHelloFrame() const;
	[[nodiscard]] std::vector<uint8_t> buildErrorFrame(std::string_view code, std::string_view detail) const;
	[[nodiscard]] std::vector<uint8_t> buildPongFrame(uint64_t timestamp) const;
	void transitionTo(ProtocolUnitySessionState nextState);
	[[nodiscard]] bool shouldDisconnectAfterViolation() const;

	const ProtocolUnityContract &contract;
	std::string serverName {};
	uint16_t advertisedPacketLimit = 0;
	uint8_t supportedCapabilities = 0;
	ProtocolUnitySessionState state = ProtocolUnitySessionState::Connected;
	uint32_t violationCount = 0;
	std::string clientName {};
	std::string clientVersionLabel {};
	uint8_t clientCapabilities = 0;
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

/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "server/network/protocol/protocolunity.hpp"

#include "config/configmanager.hpp"
#include "server/network/connection/connection.hpp"
#include "server/network/message/outputmessage.hpp"
#include "server/network/protocol/transport_codec.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <algorithm>
	#include <filesystem>

	#include <fmt/format.h>
#endif

namespace {
	constexpr uint8_t PROTOCOL_UNITY_CAPABILITY_STRUCTURED_ERRORS = 1U << 0;
	constexpr uint32_t PROTOCOL_UNITY_DISCONNECT_VIOLATION_THRESHOLD = 3;

	[[nodiscard]] std::string getAdvertisedServerName() {
		const auto configuredName = g_configManager().getString(SERVER_NAME);
		return configuredName.empty() ? "Canary ProtocolUnity" : configuredName;
	}
}

ProtocolUnitySession::ProtocolUnitySession(
	const ProtocolUnityContract &initContract,
	std::string initServerName,
	uint16_t initAdvertisedPacketLimit,
	uint8_t initSupportedCapabilities
) :
	contract(initContract),
	serverName(std::move(initServerName)),
	advertisedPacketLimit(initAdvertisedPacketLimit),
	supportedCapabilities(initSupportedCapabilities) {
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
			return reject("authentication_unavailable", "ProtocolUnity authentication is not connected yet.", false, false);
		case ProtocolUnityOpcode::MovementRequest:
		case ProtocolUnityOpcode::AttackRequest:
		case ProtocolUnityOpcode::PickupItemRequest:
		case ProtocolUnityOpcode::EnterWorldRequest:
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
		session.emplace(contract, getAdvertisedServerName(), advertisedLimit, PROTOCOL_UNITY_CAPABILITY_STRUCTURED_ERRORS);
	}

	return *session;
}

const ProtocolUnityContract &ProtocolUnity::getContract() {
	static const auto contract = [] {
		const auto manifestPath = ProtocolUnityContract::locateGeneratedManifest(std::filesystem::current_path());
		if (manifestPath.empty()) {
			throw ProtocolUnityException("Could not locate SharedProtocol/TestVectors/ProtocolUnityContract.json for ProtocolUnity runtime.");
		}
		return ProtocolUnityContract::loadFromGeneratedManifest(manifestPath);
	}();
	return contract;
}

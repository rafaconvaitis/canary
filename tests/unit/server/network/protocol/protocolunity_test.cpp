/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "server/network/protocol/protocolunity.hpp"

#ifndef USE_PRECOMPILED_HEADERS
	#include <ranges>

	#include <fmt/format.h>
	#include <gtest/gtest.h>
#endif

namespace {
	[[nodiscard]] const ProtocolUnityContract &getProtocolUnityContract() {
		static const auto manifestPath = ProtocolUnityContract::locateGeneratedManifest(std::filesystem::path(TESTS_SOURCE_DIR));
		if (manifestPath.empty()) {
			throw ProtocolUnityException("Could not locate SharedProtocol/TestVectors/ProtocolUnityContract.json from the Canary test root.");
		}

		static const auto contract = ProtocolUnityContract::loadFromGeneratedManifest(manifestPath);
		return contract;
	}

	[[nodiscard]] const ProtocolUnityVector &requireVector(std::string_view vectorName) {
		const auto &contract = getProtocolUnityContract();
		const auto iterator = std::ranges::find_if(contract.vectors, [vectorName](const ProtocolUnityVector &vector) {
			return vector.name == vectorName;
		});

		if (iterator == contract.vectors.end()) {
			throw ProtocolUnityException(fmt::format("ProtocolUnity vector '{}' was not found in the manifest.", vectorName));
		}

		return *iterator;
	}

	[[nodiscard]] std::vector<uint8_t> requireNegativeVector(std::string_view vectorName) {
		const auto &contract = getProtocolUnityContract();
		const auto iterator = std::ranges::find_if(contract.negativeVectors, [vectorName](const ProtocolUnityNegativeVector &vector) {
			return vector.name == vectorName;
		});

		if (iterator == contract.negativeVectors.end()) {
			throw ProtocolUnityException(fmt::format("ProtocolUnity negative vector '{}' was not found in the manifest.", vectorName));
		}

		return ProtocolUnityContract::decodeHex(iterator->frameHex);
	}

	[[nodiscard]] ProtocolUnitySession makeSession() {
		const auto &contract = getProtocolUnityContract();
		return ProtocolUnitySession(contract, "Canary ProtocolUnity", 4096, 1);
	}

	[[nodiscard]] ProtocolUnityFrameView decodeResponse(const std::vector<uint8_t> &frameBytes) {
		return ProtocolUnityFrameCodec::decode(frameBytes, getProtocolUnityContract());
	}
}

TEST(ProtocolUnitySessionTest, ClientHelloTransitionsToAwaitingAuthenticationAndRespondsWithServerHello) {
	auto session = makeSession();
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);

	const auto action = session.handleFrame(helloFrame);

	ASSERT_EQ(ProtocolUnitySessionState::AwaitingAuthentication, session.getState());
	ASSERT_EQ(0U, session.getViolationCount());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());
	EXPECT_EQ("TibiaUnity", session.getClientName());
	EXPECT_EQ("0.3.0", session.getClientVersionLabel());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::ServerHello, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ("Canary ProtocolUnity", reader.readString());
	EXPECT_EQ(2, reader.readU16());
	EXPECT_EQ(4096, reader.readU16());
	EXPECT_EQ(1, reader.readByte());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, PingBeforeHelloReturnsStructuredError) {
	auto session = makeSession();
	const auto pingFrame = ProtocolUnityContract::decodeHex(requireVector("ping_ticks").frameHex);

	const auto action = session.handleFrame(pingFrame);

	ASSERT_EQ(ProtocolUnitySessionState::AwaitingHello, session.getState());
	ASSERT_EQ(1U, session.getViolationCount());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::ErrorMessage, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ("hello_required", reader.readString());
	EXPECT_EQ("ClientHello must complete before Ping.", reader.readString());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, PingAfterHelloEchoesTimestampInPong) {
	auto session = makeSession();
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);
	const auto pingFrame = ProtocolUnityContract::decodeHex(requireVector("ping_ticks").frameHex);

	(void)session.handleFrame(helloFrame);
	const auto action = session.handleFrame(pingFrame);

	ASSERT_EQ(ProtocolUnitySessionState::AwaitingAuthentication, session.getState());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::Pong, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ(123456789ULL, reader.readU64());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, DuplicateHelloIsRejectedWithoutClosingImmediately) {
	auto session = makeSession();
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);

	(void)session.handleFrame(helloFrame);
	const auto action = session.handleFrame(helloFrame);

	ASSERT_EQ(ProtocolUnitySessionState::AwaitingAuthentication, session.getState());
	ASSERT_EQ(1U, session.getViolationCount());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::ErrorMessage, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ("duplicate_hello", reader.readString());
	EXPECT_EQ("ClientHello was already accepted for this connection.", reader.readString());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, ThirdViolationRequestsDisconnect) {
	auto session = makeSession();
	const auto pingFrame = ProtocolUnityContract::decodeHex(requireVector("ping_ticks").frameHex);

	(void)session.handleFrame(pingFrame);
	(void)session.handleFrame(pingFrame);
	const auto action = session.handleFrame(pingFrame);

	EXPECT_EQ(3U, session.getViolationCount());
	EXPECT_TRUE(action.closeConnection);
	EXPECT_EQ(ProtocolUnitySessionState::Closing, session.getState());
}

TEST(ProtocolUnitySessionTest, UnknownOpcodeReturnsStructuredErrorWithoutImmediateDisconnect) {
	auto session = makeSession();
	const auto unknownOpcodeFrame = requireNegativeVector("ping_unknown_opcode");

	const auto action = session.handleFrame(unknownOpcodeFrame);

	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(ProtocolUnitySessionState::AwaitingHello, session.getState());
	ASSERT_EQ(1U, session.getViolationCount());
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::ErrorMessage, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ("unknown_opcode", reader.readString());
	EXPECT_EQ("ProtocolUnity opcode 0xAAAA is not mapped.", reader.readString());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

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

	[[nodiscard]] ProtocolUnitySession makeSession(
		ProtocolUnitySession::LoginHandler loginHandler,
		ProtocolUnitySession::EnterWorldHandler enterWorldHandler = {},
		ProtocolUnitySession::MovementHandler movementHandler = {},
		ProtocolUnitySession::AttackHandler attackHandler = {}
	) {
		const auto &contract = getProtocolUnityContract();
		return ProtocolUnitySession(contract, "Canary ProtocolUnity", 4096, 1, std::move(loginHandler), std::move(enterWorldHandler), std::move(movementHandler), std::move(attackHandler));
	}

	[[nodiscard]] std::vector<uint8_t> buildLoginRequestFrame(std::string_view accountDescriptor, std::string_view secret) {
		ProtocolUnityPacketWriter writer(getProtocolUnityContract(), ProtocolUnityOpcode::LoginRequest);
		writer.writeString(accountDescriptor);
		writer.writeString(secret);
		return writer.finalize();
	}

	[[nodiscard]] std::vector<uint8_t> buildEnterWorldRequestFrame(uint32_t characterId) {
		ProtocolUnityPacketWriter writer(getProtocolUnityContract(), ProtocolUnityOpcode::EnterWorldRequest);
		writer.writeU32(characterId);
		return writer.finalize();
	}

	[[nodiscard]] std::vector<uint8_t> buildMovementRequestFrame(uint32_t actorId, uint8_t direction) {
		ProtocolUnityPacketWriter writer(getProtocolUnityContract(), ProtocolUnityOpcode::MovementRequest);
		writer.writeU32(actorId);
		writer.writeByte(direction);
		return writer.finalize();
	}

	[[nodiscard]] std::vector<uint8_t> buildAttackRequestFrame(uint32_t actorId, uint32_t targetId) {
		ProtocolUnityPacketWriter writer(getProtocolUnityContract(), ProtocolUnityOpcode::AttackRequest);
		writer.writeU32(actorId);
		writer.writeU32(targetId);
		return writer.finalize();
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

TEST(ProtocolUnitySessionTest, LoginRequestAfterHelloReturnsLoginResultAndCharacterList) {
	auto session = makeSession(
		[](std::string_view accountDescriptor, std::string_view secret) {
			EXPECT_EQ("dev.alpha", accountDescriptor);
			EXPECT_EQ("plain-secret", secret);

			ProtocolUnityLoginResponse response;
			response.success = true;
			response.message = "Login accepted.";
			response.accountId = 77;
			response.characters = {
				ProtocolUnityCharacterSummary { .characterId = 11, .name = "Knight", .position = { .x = 100, .y = 200, .floor = 7 } },
				ProtocolUnityCharacterSummary { .characterId = 22, .name = "Sorcerer", .position = { .x = 300, .y = 400, .floor = 6 } },
			};
			return response;
		}
	);
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);
	const auto loginFrame = buildLoginRequestFrame("dev.alpha", "plain-secret");

	(void)session.handleFrame(helloFrame);
	const auto action = session.handleFrame(loginFrame);

	ASSERT_EQ(ProtocolUnitySessionState::Authenticated, session.getState());
	ASSERT_EQ(77U, session.getAuthenticatedAccountId());
	ASSERT_EQ(2U, session.getCharacters().size());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(2U, action.outboundFrames.size());

	const auto loginResult = decodeResponse(action.outboundFrames[0]);
	EXPECT_EQ(ProtocolUnityOpcode::LoginResult, loginResult.opcode);
	ProtocolUnityPacketReader loginReader(loginResult.payload, getProtocolUnityContract());
	EXPECT_EQ(1, loginReader.readByte());
	EXPECT_EQ("Login accepted.", loginReader.readString());
	EXPECT_EQ(77U, loginReader.readU32());
	EXPECT_NO_THROW(loginReader.expectFullyConsumed());

	const auto characterList = decodeResponse(action.outboundFrames[1]);
	EXPECT_EQ(ProtocolUnityOpcode::CharacterList, characterList.opcode);
	ProtocolUnityPacketReader characterReader(characterList.payload, getProtocolUnityContract());
	EXPECT_EQ(2, characterReader.readByte());
	EXPECT_EQ(11U, characterReader.readU32());
	EXPECT_EQ("Knight", characterReader.readString());
	EXPECT_EQ(100, characterReader.readI32());
	EXPECT_EQ(200, characterReader.readI32());
	EXPECT_EQ(7, characterReader.readI32());
	EXPECT_EQ(22U, characterReader.readU32());
	EXPECT_EQ("Sorcerer", characterReader.readString());
	EXPECT_EQ(300, characterReader.readI32());
	EXPECT_EQ(400, characterReader.readI32());
	EXPECT_EQ(6, characterReader.readI32());
	EXPECT_NO_THROW(characterReader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, FailedLoginReturnsOnlyLoginResultAndKeepsAuthenticationState) {
	auto session = makeSession(
		[](std::string_view accountDescriptor, std::string_view secret) {
			EXPECT_EQ("dev.alpha", accountDescriptor);
			EXPECT_EQ("bad-secret", secret);

			ProtocolUnityLoginResponse response;
			response.success = false;
			response.message = "Account or secret is not correct.";
			return response;
		}
	);
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);
	const auto loginFrame = buildLoginRequestFrame("dev.alpha", "bad-secret");

	(void)session.handleFrame(helloFrame);
	const auto action = session.handleFrame(loginFrame);

	ASSERT_EQ(ProtocolUnitySessionState::AwaitingAuthentication, session.getState());
	ASSERT_EQ(0U, session.getAuthenticatedAccountId());
	ASSERT_EQ(0U, session.getCharacters().size());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto loginResult = decodeResponse(action.outboundFrames[0]);
	EXPECT_EQ(ProtocolUnityOpcode::LoginResult, loginResult.opcode);
	ProtocolUnityPacketReader reader(loginResult.payload, getProtocolUnityContract());
	EXPECT_EQ(0, reader.readByte());
	EXPECT_EQ("Account or secret is not correct.", reader.readString());
	EXPECT_EQ(0U, reader.readU32());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, EnterWorldAfterAuthenticatedLoginValidatesSelectionAndReturnsStructuredResult) {
	auto session = makeSession(
		[](std::string_view, std::string_view) {
			ProtocolUnityLoginResponse response;
			response.success = true;
			response.message = "Login accepted.";
			response.accountId = 77;
			response.characters = {
				ProtocolUnityCharacterSummary { .characterId = 11, .name = "Knight", .position = { .x = 100, .y = 200, .floor = 7 } },
			};
			return response;
		},
		[](uint32_t accountId, uint32_t characterId) {
			EXPECT_EQ(77U, accountId);
			EXPECT_EQ(11U, characterId);

			ProtocolUnityEnterWorldResponse response;
			response.success = true;
			response.selectionAccepted = true;
			response.message = "Entered world.";
			response.actorId = 1100;
			response.spawnPosition = { .x = 8, .y = 6, .floor = 7 };
			return response;
		}
	);
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);
	const auto loginFrame = buildLoginRequestFrame("dev.alpha", "plain-secret");
	const auto enterWorldFrame = buildEnterWorldRequestFrame(11);

	(void)session.handleFrame(helloFrame);
	(void)session.handleFrame(loginFrame);
	const auto action = session.handleFrame(enterWorldFrame);

	ASSERT_EQ(ProtocolUnitySessionState::InWorld, session.getState());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto result = decodeResponse(action.outboundFrames[0]);
	EXPECT_EQ(ProtocolUnityOpcode::EnterWorldResult, result.opcode);
	ProtocolUnityPacketReader reader(result.payload, getProtocolUnityContract());
	EXPECT_EQ(1, reader.readByte());
	EXPECT_EQ("Entered world.", reader.readString());
	EXPECT_EQ(1100U, reader.readU32());
	EXPECT_EQ(8, reader.readI32());
	EXPECT_EQ(6, reader.readI32());
	EXPECT_EQ(7, reader.readI32());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, MovementRequestBeforeWorldEntryReturnsStructuredError) {
	auto session = makeSession();
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);
	const auto movementFrame = buildMovementRequestFrame(1100, 2);

	(void)session.handleFrame(helloFrame);
	const auto action = session.handleFrame(movementFrame);

	ASSERT_EQ(ProtocolUnitySessionState::AwaitingAuthentication, session.getState());
	ASSERT_EQ(1U, session.getViolationCount());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::ErrorMessage, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ("invalid_state", reader.readString());
	EXPECT_EQ("MovementRequest is only valid after world entry succeeds.", reader.readString());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, MovementRequestInWorldDispatchesMovementHandler) {
	auto session = makeSession(
		[](std::string_view, std::string_view) {
			ProtocolUnityLoginResponse response;
			response.success = true;
			response.message = "Login accepted.";
			response.accountId = 77;
			response.characters = {
				ProtocolUnityCharacterSummary { .characterId = 11, .name = "Knight", .position = { .x = 100, .y = 200, .floor = 7 } },
			};
			return response;
		},
		[](uint32_t, uint32_t) {
			ProtocolUnityEnterWorldResponse response;
			response.success = true;
			response.selectionAccepted = true;
			response.message = "Entered world.";
			response.actorId = 1100;
			response.spawnPosition = { .x = 8, .y = 6, .floor = 7 };
			return response;
		},
		[](uint32_t actorId, uint8_t direction) {
			EXPECT_EQ(1100U, actorId);
			EXPECT_EQ(2, direction);

			ProtocolUnitySessionAction action;
			ProtocolUnityPacketWriter writer(getProtocolUnityContract(), ProtocolUnityOpcode::MovementResult);
			writer.writeU32(actorId);
			writer.writeI32(9);
			writer.writeI32(6);
			writer.writeI32(7);
			writer.writeByte(1);
			writer.writeString("");
			action.outboundFrames.emplace_back(writer.finalize());
			return action;
		}
	);
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);
	const auto loginFrame = buildLoginRequestFrame("dev.alpha", "plain-secret");
	const auto enterWorldFrame = buildEnterWorldRequestFrame(11);
	const auto movementFrame = buildMovementRequestFrame(1100, 2);

	(void)session.handleFrame(helloFrame);
	(void)session.handleFrame(loginFrame);
	(void)session.handleFrame(enterWorldFrame);
	const auto action = session.handleFrame(movementFrame);

	ASSERT_EQ(ProtocolUnitySessionState::InWorld, session.getState());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::MovementResult, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ(1100U, reader.readU32());
	EXPECT_EQ(9, reader.readI32());
	EXPECT_EQ(6, reader.readI32());
	EXPECT_EQ(7, reader.readI32());
	EXPECT_EQ(1, reader.readByte());
	EXPECT_EQ("", reader.readString());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnityMovementTest, ImmediateCancelRequiresNoQueuedWalkSteps) {
	const ProtocolUnityWorldPosition requestedFrom { .x = 8, .y = 6, .floor = 7 };
	const ProtocolUnityWorldPosition unchangedPosition { .x = 8, .y = 6, .floor = 7 };

	EXPECT_TRUE(shouldSynthesizeImmediateMovementCancel(requestedFrom, unchangedPosition, 0));
	EXPECT_FALSE(shouldSynthesizeImmediateMovementCancel(requestedFrom, unchangedPosition, 1));
}

TEST(ProtocolUnityMovementTest, ImmediateCancelIsSkippedAfterAuthoritativePositionChanges) {
	const ProtocolUnityWorldPosition requestedFrom { .x = 8, .y = 6, .floor = 7 };
	const ProtocolUnityWorldPosition movedPosition { .x = 7, .y = 6, .floor = 7 };

	EXPECT_FALSE(shouldSynthesizeImmediateMovementCancel(requestedFrom, movedPosition, 0));
}

TEST(ProtocolUnitySessionTest, AttackRequestBeforeWorldEntryReturnsStructuredError) {
	auto session = makeSession();
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);
	const auto attackFrame = buildAttackRequestFrame(1100, 2200);

	(void)session.handleFrame(helloFrame);
	const auto action = session.handleFrame(attackFrame);

	ASSERT_EQ(ProtocolUnitySessionState::AwaitingAuthentication, session.getState());
	ASSERT_EQ(1U, session.getViolationCount());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::ErrorMessage, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ("invalid_state", reader.readString());
	EXPECT_EQ("AttackRequest is only valid after world entry succeeds.", reader.readString());
	EXPECT_NO_THROW(reader.expectFullyConsumed());
}

TEST(ProtocolUnitySessionTest, AttackRequestInWorldDispatchesAttackHandler) {
	auto session = makeSession(
		[](std::string_view, std::string_view) {
			ProtocolUnityLoginResponse response;
			response.success = true;
			response.message = "Login accepted.";
			response.accountId = 77;
			response.characters = {
				ProtocolUnityCharacterSummary { .characterId = 11, .name = "Knight", .position = { .x = 100, .y = 200, .floor = 7 } },
			};
			return response;
		},
		[](uint32_t, uint32_t) {
			ProtocolUnityEnterWorldResponse response;
			response.success = true;
			response.selectionAccepted = true;
			response.message = "Entered world.";
			response.actorId = 1100;
			response.spawnPosition = { .x = 8, .y = 6, .floor = 7 };
			return response;
		},
		{},
		[](uint32_t actorId, uint32_t targetId) {
			EXPECT_EQ(1100U, actorId);
			EXPECT_EQ(2200U, targetId);

			ProtocolUnitySessionAction action;
			ProtocolUnityPacketWriter writer(getProtocolUnityContract(), ProtocolUnityOpcode::CombatResult);
			writer.writeU32(actorId);
			writer.writeU32(targetId);
			writer.writeI16(12);
			writer.writeU16(18);
			writer.writeByte(0);
			writer.writeByte(1);
			writer.writeString("hit");
			action.outboundFrames.emplace_back(writer.finalize());
			return action;
		}
	);
	const auto helloFrame = ProtocolUnityContract::decodeHex(requireVector("client_hello_development").frameHex);
	const auto loginFrame = buildLoginRequestFrame("dev.alpha", "plain-secret");
	const auto enterWorldFrame = buildEnterWorldRequestFrame(11);
	const auto attackFrame = buildAttackRequestFrame(1100, 2200);

	(void)session.handleFrame(helloFrame);
	(void)session.handleFrame(loginFrame);
	(void)session.handleFrame(enterWorldFrame);
	const auto action = session.handleFrame(attackFrame);

	ASSERT_EQ(ProtocolUnitySessionState::InWorld, session.getState());
	ASSERT_FALSE(action.closeConnection);
	ASSERT_EQ(1U, action.outboundFrames.size());

	const auto response = decodeResponse(action.outboundFrames.front());
	EXPECT_EQ(ProtocolUnityOpcode::CombatResult, response.opcode);

	ProtocolUnityPacketReader reader(response.payload, getProtocolUnityContract());
	EXPECT_EQ(1100U, reader.readU32());
	EXPECT_EQ(2200U, reader.readU32());
	EXPECT_EQ(12, reader.readI16());
	EXPECT_EQ(18, reader.readU16());
	EXPECT_EQ(0, reader.readByte());
	EXPECT_EQ(1, reader.readByte());
	EXPECT_EQ("hit", reader.readString());
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

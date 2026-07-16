/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "server/network/protocol/protocolunity_contract.hpp"

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

	[[nodiscard]] std::string bytesToHex(std::span<const uint8_t> bytes) {
		std::string hex;
		hex.reserve(bytes.size() * 2);
		for (const auto value : bytes) {
			hex += fmt::format("{:02X}", value);
		}
		return hex;
	}
}

TEST(ProtocolUnityContractTest, LoadsCanonicalManifestAndOpcodeRegistry) {
	const auto &contract = getProtocolUnityContract();

	EXPECT_EQ(1u, contract.schemaVersion);
	EXPECT_EQ(2, contract.protocolVersion);
	EXPECT_EQ(4u, contract.framePrefixBytes);
	EXPECT_EQ(65535u, contract.maximumPacketSize);
	EXPECT_EQ(1024, contract.maximumStringLength);
	EXPECT_EQ(36u, contract.opcodes.size());
	EXPECT_EQ(10u, contract.vectors.size());
	EXPECT_EQ(2u, contract.negativeVectors.size());

	const auto &clientHello = contract.requireOpcode(ProtocolUnityOpcode::ClientHello);
	EXPECT_EQ("ClientHello", clientHello.name);
	EXPECT_EQ(ProtocolUnityDirection::ClientToServer, clientHello.direction);

	const auto &errorMessage = contract.requireOpcode(0x00FF);
	EXPECT_EQ(ProtocolUnityOpcode::ErrorMessage, errorMessage.opcode);
	EXPECT_EQ("ErrorMessage", errorMessage.name);

	ASSERT_TRUE(contract.tryParseOpcode("Ping").has_value());
	EXPECT_EQ(ProtocolUnityOpcode::Ping, *contract.tryParseOpcode("Ping"));
	ASSERT_TRUE(contract.tryParseOpcode("ReconnectRequest").has_value());
	EXPECT_EQ(ProtocolUnityOpcode::ReconnectRequest, *contract.tryParseOpcode("ReconnectRequest"));
	EXPECT_EQ(ProtocolUnityOpcode::SessionToken, requireVector("session_token_issue").opcode);
	EXPECT_EQ(ProtocolUnityOpcode::ReconnectRequest, requireVector("reconnect_request_resume").opcode);
	EXPECT_FALSE(contract.tryParseOpcode("NotRealOpcode").has_value());
}

TEST(ProtocolUnityFrameCodecTest, PositiveVectorsDecodeWithCanonicalHeaders) {
	const auto &contract = getProtocolUnityContract();

	for (const auto &vector : contract.vectors) {
		SCOPED_TRACE(vector.name);
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		EXPECT_EQ(contract.protocolVersion, frame.version);
		EXPECT_EQ(vector.opcode, frame.opcode);
	}
}

TEST(ProtocolUnityPacketReaderTest, ReadsDeterministicPositiveVectors) {
	const auto &contract = getProtocolUnityContract();

	{
		const auto &vector = requireVector("client_hello_development");
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		ProtocolUnityPacketReader reader(frame.payload, contract);
		EXPECT_EQ("TibiaUnity", reader.readString());
		EXPECT_EQ("0.3.0", reader.readString());
		EXPECT_EQ(1, reader.readByte());
		EXPECT_NO_THROW(reader.expectFullyConsumed());
	}

	{
		const auto &vector = requireVector("server_hello_default");
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		ProtocolUnityPacketReader reader(frame.payload, contract);
		EXPECT_EQ("TibiaUnity DevServer", reader.readString());
		EXPECT_EQ(2, reader.readU16());
		EXPECT_EQ(65535, reader.readU16());
		EXPECT_EQ(1, reader.readByte());
		EXPECT_NO_THROW(reader.expectFullyConsumed());
	}

	{
		const auto &vector = requireVector("login_request_hash");
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		ProtocolUnityPacketReader reader(frame.payload, contract);
		EXPECT_EQ("dev.alpha", reader.readString());
		EXPECT_EQ("sha256:abc123", reader.readString());
		EXPECT_NO_THROW(reader.expectFullyConsumed());
	}

	{
		const auto &vector = requireVector("movement_request_east");
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		ProtocolUnityPacketReader reader(frame.payload, contract);
		EXPECT_EQ(42u, reader.readU32());
		EXPECT_EQ(2, reader.readByte());
		EXPECT_NO_THROW(reader.expectFullyConsumed());
	}

	{
		const auto &vector = requireVector("defend_request_enter");
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		ProtocolUnityPacketReader reader(frame.payload, contract);
		EXPECT_EQ(42u, reader.readU32());
		EXPECT_EQ(1, reader.readByte());
		EXPECT_NO_THROW(reader.expectFullyConsumed());
	}

	{
		const auto &vector = requireVector("defense_result_blocked");
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		ProtocolUnityPacketReader reader(frame.payload, contract);
		EXPECT_EQ(42u, reader.readU32());
		EXPECT_EQ(77u, reader.readU32());
		EXPECT_EQ(4, reader.readByte());
		EXPECT_EQ(24, reader.readI16());
		EXPECT_EQ(0, reader.readI16());
		EXPECT_EQ(1, reader.readByte());
		EXPECT_EQ(1, reader.readByte());
		EXPECT_EQ("blocked", reader.readString());
		EXPECT_NO_THROW(reader.expectFullyConsumed());
	}

	{
		const auto &vector = requireVector("ping_ticks");
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		ProtocolUnityPacketReader reader(frame.payload, contract);
		EXPECT_EQ(123456789ULL, reader.readU64());
		EXPECT_NO_THROW(reader.expectFullyConsumed());
	}

	{
		const auto &vector = requireVector("error_message_blocked");
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);
		const auto frame = ProtocolUnityFrameCodec::decode(bytes, contract);
		ProtocolUnityPacketReader reader(frame.payload, contract);
		EXPECT_EQ("movement_blocked", reader.readString());
		EXPECT_EQ("Tile is blocked", reader.readString());
		EXPECT_NO_THROW(reader.expectFullyConsumed());
	}
}

TEST(ProtocolUnityPacketWriterTest, RebuildsDeterministicPositiveVectors) {
	const auto &contract = getProtocolUnityContract();

	{
		const auto &vector = requireVector("client_hello_development");
		ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::ClientHello);
		writer.writeString("TibiaUnity");
		writer.writeString("0.3.0");
		writer.writeByte(1);
		EXPECT_EQ(vector.frameHex, bytesToHex(writer.finalize()));
	}

	{
		const auto &vector = requireVector("server_hello_default");
		ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::ServerHello);
		writer.writeString("TibiaUnity DevServer");
		writer.writeU16(2);
		writer.writeU16(65535);
		writer.writeByte(1);
		EXPECT_EQ(vector.frameHex, bytesToHex(writer.finalize()));
	}

	{
		const auto &vector = requireVector("login_request_hash");
		ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::LoginRequest);
		writer.writeString("dev.alpha");
		writer.writeString("sha256:abc123");
		EXPECT_EQ(vector.frameHex, bytesToHex(writer.finalize()));
	}

	{
		const auto &vector = requireVector("movement_request_east");
		ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::MovementRequest);
		writer.writeU32(42);
		writer.writeByte(2);
		EXPECT_EQ(vector.frameHex, bytesToHex(writer.finalize()));
	}

	{
		const auto &vector = requireVector("defend_request_enter");
		ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::DefendRequest);
		writer.writeU32(42);
		writer.writeByte(1);
		EXPECT_EQ(vector.frameHex, bytesToHex(writer.finalize()));
	}

	{
		const auto &vector = requireVector("defense_result_blocked");
		ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::DefenseResult);
		writer.writeU32(42);
		writer.writeU32(77);
		writer.writeByte(4);
		writer.writeI16(24);
		writer.writeI16(0);
		writer.writeByte(1);
		writer.writeByte(1);
		writer.writeString("blocked");
		EXPECT_EQ(vector.frameHex, bytesToHex(writer.finalize()));
	}

	{
		const auto &vector = requireVector("ping_ticks");
		ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::Ping);
		writer.writeU64(123456789ULL);
		EXPECT_EQ(vector.frameHex, bytesToHex(writer.finalize()));
	}

	{
		const auto &vector = requireVector("error_message_blocked");
		ProtocolUnityPacketWriter writer(contract, ProtocolUnityOpcode::ErrorMessage);
		writer.writeString("movement_blocked");
		writer.writeString("Tile is blocked");
		EXPECT_EQ(vector.frameHex, bytesToHex(writer.finalize()));
	}
}

TEST(ProtocolUnityFrameCodecTest, NegativeVectorsRaiseTypedExceptions) {
	const auto &contract = getProtocolUnityContract();

	for (const auto &vector : contract.negativeVectors) {
		SCOPED_TRACE(vector.name);
		const auto bytes = ProtocolUnityContract::decodeHex(vector.frameHex);

		if (vector.expectedException == "UnknownOpcodeException") {
			EXPECT_THROW(
				{
					(void)ProtocolUnityFrameCodec::decode(bytes, contract);
				},
				ProtocolUnityUnknownOpcodeException
			);
			continue;
		}

		EXPECT_THROW(
			{
				(void)ProtocolUnityFrameCodec::decode(bytes, contract);
			},
			ProtocolUnityException
		);
	}
}

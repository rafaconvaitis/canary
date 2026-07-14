/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019–present OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#pragma once

#ifndef USE_PRECOMPILED_HEADERS
	#include <cstdint>
	#include <filesystem>
	#include <optional>
	#include <span>
	#include <stdexcept>
	#include <string>
	#include <string_view>
	#include <unordered_map>
	#include <vector>
#endif

enum class ProtocolUnityOpcode : uint16_t {
	ClientHello = 0x0001,
	ServerHello = 0x0002,
	LoginRequest = 0x0010,
	LoginResult = 0x0011,
	CharacterList = 0x0012,
	EnterWorldRequest = 0x0013,
	EnterWorldResult = 0x0014,
	SessionToken = 0x0015,
	ReconnectRequest = 0x0016,
	ReconnectResult = 0x0017,
	SessionResyncBegin = 0x0018,
	SessionResyncEnd = 0x0019,
	SessionExpired = 0x001A,
	MapSnapshot = 0x0020,
	MapChunk = 0x0021,
	CreatureSpawn = 0x0022,
	CreatureMove = 0x0023,
	CreatureHealth = 0x0024,
	CreatureDeath = 0x0025,
	CreatureDespawn = 0x0026,
	MovementRequest = 0x0030,
	MovementResult = 0x0031,
	AttackRequest = 0x0032,
	CombatResult = 0x0033,
	ItemSpawn = 0x0040,
	ItemRemove = 0x0041,
	PickupItemRequest = 0x0042,
	PickupItemResult = 0x0043,
	InventorySnapshot = 0x0044,
	InventoryUpdate = 0x0045,
	SystemMessage = 0x0050,
	Ping = 0x00F0,
	Pong = 0x00F1,
	ErrorMessage = 0x00FF,
};

enum class ProtocolUnityDirection : uint8_t {
	ClientToServer,
	ServerToClient,
};

class ProtocolUnityException : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

class ProtocolUnityUnknownOpcodeException final : public ProtocolUnityException {
public:
	using ProtocolUnityException::ProtocolUnityException;
};

struct ProtocolUnityOpcodeMetadata {
	ProtocolUnityOpcode opcode = ProtocolUnityOpcode::ClientHello;
	std::string name {};
	ProtocolUnityDirection direction = ProtocolUnityDirection::ClientToServer;
	std::string notes {};
};

struct ProtocolUnityVector {
	std::string name {};
	ProtocolUnityOpcode opcode = ProtocolUnityOpcode::ClientHello;
	std::string opcodeName {};
	std::string description {};
	std::string frameHex {};
};

struct ProtocolUnityNegativeVector {
	std::string name {};
	std::string description {};
	std::string frameHex {};
	std::string expectedException {};
};

struct ProtocolUnityFrameView {
	uint16_t version = 0;
	ProtocolUnityOpcode opcode = ProtocolUnityOpcode::ClientHello;
	std::span<const uint8_t> payload {};
};

class ProtocolUnityContract {
public:
	[[nodiscard]] static ProtocolUnityContract loadFromGeneratedManifest(const std::filesystem::path &manifestPath);
	[[nodiscard]] static std::filesystem::path locateGeneratedManifest(const std::filesystem::path &startDirectory);
	[[nodiscard]] static std::vector<uint8_t> decodeHex(std::string_view hex);

	[[nodiscard]] const ProtocolUnityOpcodeMetadata &requireOpcode(uint16_t rawOpcode) const;
	[[nodiscard]] const ProtocolUnityOpcodeMetadata &requireOpcode(ProtocolUnityOpcode opcode) const;
	[[nodiscard]] const ProtocolUnityOpcodeMetadata &requireOpcode(std::string_view opcodeName) const;
	[[nodiscard]] std::optional<ProtocolUnityOpcode> tryParseOpcode(std::string_view opcodeName) const;

	uint32_t schemaVersion = 0;
	uint16_t protocolVersion = 0;
	uint32_t framePrefixBytes = 0;
	uint32_t maximumPacketSize = 0;
	uint16_t maximumStringLength = 0;
	uint16_t maximumCharacterCount = 0;
	uint16_t maximumInventorySlots = 0;
	uint16_t defaultChunkSize = 0;
	uint32_t defaultTimeoutMilliseconds = 0;
	std::vector<ProtocolUnityOpcodeMetadata> opcodes {};
	std::vector<ProtocolUnityVector> vectors {};
	std::vector<ProtocolUnityNegativeVector> negativeVectors {};

private:
	std::unordered_map<uint16_t, size_t> opcodeIndexByValue {};
	std::unordered_map<std::string, size_t> opcodeIndexByName {};
};

class ProtocolUnityPacketReader {
public:
	ProtocolUnityPacketReader(std::span<const uint8_t> initPayload, const ProtocolUnityContract &initContract);

	[[nodiscard]] bool canRead(size_t size) const;
	[[nodiscard]] size_t remaining() const;

	[[nodiscard]] uint8_t readByte();
	[[nodiscard]] int16_t readI16();
	[[nodiscard]] uint16_t readU16();
	[[nodiscard]] int32_t readI32();
	[[nodiscard]] uint32_t readU32();
	[[nodiscard]] int64_t readI64();
	[[nodiscard]] uint64_t readU64();
	[[nodiscard]] std::string readString();
	void expectFullyConsumed() const;

private:
	template <typename T>
	[[nodiscard]] T readLittleEndian();

	const ProtocolUnityContract &contract;
	std::span<const uint8_t> payload;
	size_t position = 0;
};

class ProtocolUnityFrameCodec {
public:
	[[nodiscard]] static ProtocolUnityFrameView decode(std::span<const uint8_t> frameBytes, const ProtocolUnityContract &contract);
	[[nodiscard]] static std::vector<uint8_t> encode(ProtocolUnityOpcode opcode, std::span<const uint8_t> payload, const ProtocolUnityContract &contract);
};

class ProtocolUnityPacketWriter {
public:
	ProtocolUnityPacketWriter(const ProtocolUnityContract &initContract, ProtocolUnityOpcode initOpcode);

	void writeByte(uint8_t value);
	void writeI16(int16_t value);
	void writeU16(uint16_t value);
	void writeI32(int32_t value);
	void writeU32(uint32_t value);
	void writeI64(int64_t value);
	void writeU64(uint64_t value);
	void writeString(std::string_view value);

	[[nodiscard]] std::vector<uint8_t> finalize() const;

private:
	template <typename T>
	void writeLittleEndian(T value);

	void ensurePayloadCapacity(size_t additionalBytes) const;

	const ProtocolUnityContract &contract;
	ProtocolUnityOpcode opcode;
	std::vector<uint8_t> payload {};
};

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
	#include <fstream>

	#include <fmt/format.h>
	#include <magic_enum/magic_enum.hpp>
	#include <nlohmann/json.hpp>
#endif

namespace {
	using json = nlohmann::json;

	constexpr uint32_t protocolUnityHeaderSize = sizeof(uint16_t) + sizeof(uint16_t);
	constexpr uint32_t protocolUnityPrefixSize = sizeof(uint32_t);

	[[nodiscard]] uint8_t decodeHexNibble(char value) {
		if (value >= '0' && value <= '9') {
			return static_cast<uint8_t>(value - '0');
		}

		if (value >= 'A' && value <= 'F') {
			return static_cast<uint8_t>(10 + value - 'A');
		}

		if (value >= 'a' && value <= 'f') {
			return static_cast<uint8_t>(10 + value - 'a');
		}

		throw ProtocolUnityException(fmt::format("Invalid hex digit '{}'.", value));
	}

	[[nodiscard]] uint16_t parseHexU16(std::string_view value) {
		if (value.starts_with("0x") || value.starts_with("0X")) {
			value.remove_prefix(2);
		}

		if (value.empty() || value.size() > 4) {
			throw ProtocolUnityException(fmt::format("Invalid uint16 hex literal '{}'.", value));
		}

		uint16_t result = 0;
		for (char digit : value) {
			result = static_cast<uint16_t>((result << 4) | decodeHexNibble(digit));
		}
		return result;
	}

	[[nodiscard]] ProtocolUnityDirection parseDirection(std::string_view value) {
		if (value == "client_to_server") {
			return ProtocolUnityDirection::ClientToServer;
		}

		if (value == "server_to_client") {
			return ProtocolUnityDirection::ServerToClient;
		}

		throw ProtocolUnityException(fmt::format("Unsupported ProtocolUnity direction '{}'.", value));
	}

	[[nodiscard]] ProtocolUnityOpcode parseOpcodeName(std::string_view opcodeName) {
		if (opcodeName == "DefendRequest") {
			return ProtocolUnityOpcode::DefendRequest;
		}
		if (opcodeName == "DefenseResult") {
			return ProtocolUnityOpcode::DefenseResult;
		}
		if (opcodeName == "TurnRequest") {
			return ProtocolUnityOpcode::TurnRequest;
		}
		if (opcodeName == "TurnResult") {
			return ProtocolUnityOpcode::TurnResult;
		}
		if (opcodeName == "FollowRequest") {
			return ProtocolUnityOpcode::FollowRequest;
		}
		if (opcodeName == "FollowResult") {
			return ProtocolUnityOpcode::FollowResult;
		}
		if (opcodeName == "FightModeRequest") {
			return ProtocolUnityOpcode::FightModeRequest;
		}
		if (opcodeName == "FightModeResult") {
			return ProtocolUnityOpcode::FightModeResult;
		}
		if (opcodeName == "InteractionRequest") {
			return ProtocolUnityOpcode::InteractionRequest;
		}
		if (opcodeName == "InteractionResult") {
			return ProtocolUnityOpcode::InteractionResult;
		}

		const auto opcode = magic_enum::enum_cast<ProtocolUnityOpcode>(opcodeName);
		if (!opcode.has_value()) {
			throw ProtocolUnityException(fmt::format("Unknown ProtocolUnity opcode name '{}'.", opcodeName));
		}

		return *opcode;
	}

	template <typename T>
	[[nodiscard]] T readLittleEndian(std::span<const uint8_t> bytes, size_t offset) {
		T value = 0;
		for (size_t index = 0; index < sizeof(T); ++index) {
			value |= static_cast<T>(bytes[offset + index]) << (index * 8);
		}
		return value;
	}

	template <typename T>
	void writeLittleEndian(std::vector<uint8_t> &buffer, T value) {
		for (size_t index = 0; index < sizeof(T); ++index) {
			buffer.push_back(static_cast<uint8_t>((value >> (index * 8)) & 0xFF));
		}
	}
}

ProtocolUnityContract ProtocolUnityContract::loadFromGeneratedManifest(const std::filesystem::path &manifestPath) {
	std::ifstream stream(manifestPath);
	if (!stream.is_open()) {
		throw ProtocolUnityException(fmt::format("Could not open ProtocolUnity manifest '{}'.", manifestPath.string()));
	}

	json manifest;
	try {
		stream >> manifest;
	} catch (const json::exception &exception) {
		throw ProtocolUnityException(fmt::format("Failed to parse ProtocolUnity manifest '{}': {}", manifestPath.string(), exception.what()));
	}

	ProtocolUnityContract contract;
	contract.schemaVersion = manifest.at("SchemaVersion").get<uint32_t>();
	contract.protocolVersion = manifest.at("ProtocolVersion").get<uint16_t>();
	if (contract.schemaVersion != 1) {
		throw ProtocolUnityException(fmt::format(
			"Unsupported ProtocolUnity schema version {} in '{}'.",
			contract.schemaVersion,
			manifestPath.string()
		));
	}

	const auto &framing = manifest.at("Framing");
	contract.framePrefixBytes = framing.at("FramePrefixBytes").get<uint32_t>();
	if (contract.framePrefixBytes != protocolUnityPrefixSize) {
		throw ProtocolUnityException(fmt::format(
			"Unsupported ProtocolUnity frame prefix size {} in '{}'.",
			contract.framePrefixBytes,
			manifestPath.string()
		));
	}

	if (framing.at("PrefixEndian").get<std::string>() != "little") {
		throw ProtocolUnityException("ProtocolUnity only supports little-endian frame prefixes.");
	}

	const auto &headerFields = framing.at("HeaderFields");
	if (headerFields.size() != 2) {
		throw ProtocolUnityException("ProtocolUnity header must contain version and opcode fields.");
	}

	if (headerFields.at(0).at("Name").get<std::string>() != "version" || headerFields.at(0).at("Type").get<std::string>() != "uint16") {
		throw ProtocolUnityException("ProtocolUnity manifest version header is not uint16.");
	}

	if (headerFields.at(1).at("Name").get<std::string>() != "opcode" || headerFields.at(1).at("Type").get<std::string>() != "uint16") {
		throw ProtocolUnityException("ProtocolUnity manifest opcode header is not uint16.");
	}

	if (framing.at("StringLengthType").get<std::string>() != "uint16") {
		throw ProtocolUnityException("ProtocolUnity string length prefix must be uint16.");
	}

	if (framing.at("StringEncoding").get<std::string>() != "utf-8") {
		throw ProtocolUnityException("ProtocolUnity only supports UTF-8 strings.");
	}

	const auto &limits = manifest.at("Limits");
	contract.maximumPacketSize = limits.at("DefaultMaximumPacketSize").get<uint32_t>();
	contract.maximumStringLength = limits.at("MaximumStringLength").get<uint16_t>();
	contract.maximumCharacterCount = limits.at("MaximumCharacterCount").get<uint16_t>();
	contract.maximumInventorySlots = limits.at("MaximumInventorySlots").get<uint16_t>();
	contract.defaultChunkSize = limits.at("DefaultChunkSize").get<uint16_t>();
	contract.defaultTimeoutMilliseconds = limits.at("DefaultTimeoutMilliseconds").get<uint32_t>();

	for (const auto &opcodeNode : manifest.at("Opcodes")) {
		ProtocolUnityOpcodeMetadata metadata;
		metadata.name = opcodeNode.at("Name").get<std::string>();
		metadata.opcode = parseOpcodeName(metadata.name);
		metadata.direction = parseDirection(opcodeNode.at("Direction").get<std::string>());
		metadata.notes = opcodeNode.value("Notes", std::string {});

		const auto manifestValue = parseHexU16(opcodeNode.at("ValueHex").get<std::string>());
		if (manifestValue != magic_enum::enum_integer(metadata.opcode)) {
			throw ProtocolUnityException(fmt::format(
				"Opcode '{}' uses manifest value 0x{:04X}, expected 0x{:04X}.",
				metadata.name,
				manifestValue,
				magic_enum::enum_integer(metadata.opcode)
			));
		}

		const auto index = contract.opcodes.size();
		contract.opcodes.push_back(std::move(metadata));
		contract.opcodeIndexByValue.emplace(manifestValue, index);
		contract.opcodeIndexByName.emplace(contract.opcodes.back().name, index);
	}

	for (const auto &vectorNode : manifest.at("Vectors")) {
		ProtocolUnityVector vector;
		vector.name = vectorNode.at("Name").get<std::string>();
		vector.opcodeName = vectorNode.at("OpcodeName").get<std::string>();
		vector.opcode = parseOpcodeName(vector.opcodeName);
		vector.description = vectorNode.value("Description", std::string {});
		vector.frameHex = vectorNode.at("FrameHex").get<std::string>();

		if (!contract.tryParseOpcode(vector.opcodeName).has_value()) {
			throw ProtocolUnityException(fmt::format("Vector '{}' references unknown opcode '{}'.", vector.name, vector.opcodeName));
		}

		contract.vectors.push_back(std::move(vector));
	}

	for (const auto &vectorNode : manifest.at("NegativeVectors")) {
		ProtocolUnityNegativeVector vector;
		vector.name = vectorNode.at("Name").get<std::string>();
		vector.description = vectorNode.value("Description", std::string {});
		vector.frameHex = vectorNode.at("FrameHex").get<std::string>();
		vector.expectedException = vectorNode.at("ExpectedException").get<std::string>();
		contract.negativeVectors.push_back(std::move(vector));
	}

	return contract;
}

std::filesystem::path ProtocolUnityContract::locateGeneratedManifest(const std::filesystem::path &startDirectory) {
	auto current = std::filesystem::absolute(startDirectory);
	const auto relativeManifestPath = std::filesystem::path("SharedProtocol") / "TestVectors" / "ProtocolUnityContract.json";

	while (!current.empty()) {
		std::error_code errorCode;
		const auto candidate = current / relativeManifestPath;
		if (std::filesystem::exists(candidate, errorCode) && !errorCode) {
			return candidate;
		}

		if (!current.has_parent_path() || current.parent_path() == current) {
			break;
		}

		current = current.parent_path();
	}

	return {};
}

std::vector<uint8_t> ProtocolUnityContract::decodeHex(std::string_view hex) {
	if ((hex.size() % 2) != 0) {
		throw ProtocolUnityException(fmt::format("Hex payload '{}' must contain an even number of characters.", hex));
	}

	std::vector<uint8_t> bytes;
	bytes.reserve(hex.size() / 2);

	for (size_t index = 0; index < hex.size(); index += 2) {
		const auto upper = decodeHexNibble(hex[index]);
		const auto lower = decodeHexNibble(hex[index + 1]);
		bytes.push_back(static_cast<uint8_t>((upper << 4) | lower));
	}

	return bytes;
}

const ProtocolUnityOpcodeMetadata &ProtocolUnityContract::requireOpcode(uint16_t rawOpcode) const {
	const auto iterator = opcodeIndexByValue.find(rawOpcode);
	if (iterator == opcodeIndexByValue.end()) {
		throw ProtocolUnityUnknownOpcodeException(fmt::format("ProtocolUnity opcode 0x{:04X} is not mapped.", rawOpcode));
	}

	return opcodes[iterator->second];
}

const ProtocolUnityOpcodeMetadata &ProtocolUnityContract::requireOpcode(ProtocolUnityOpcode opcode) const {
	return requireOpcode(magic_enum::enum_integer(opcode));
}

const ProtocolUnityOpcodeMetadata &ProtocolUnityContract::requireOpcode(std::string_view opcodeName) const {
	const auto iterator = opcodeIndexByName.find(std::string(opcodeName));
	if (iterator == opcodeIndexByName.end()) {
		throw ProtocolUnityUnknownOpcodeException(fmt::format("ProtocolUnity opcode '{}' is not mapped.", opcodeName));
	}

	return opcodes[iterator->second];
}

std::optional<ProtocolUnityOpcode> ProtocolUnityContract::tryParseOpcode(std::string_view opcodeName) const {
	const auto iterator = opcodeIndexByName.find(std::string(opcodeName));
	if (iterator == opcodeIndexByName.end()) {
		return std::nullopt;
	}

	return opcodes[iterator->second].opcode;
}

ProtocolUnityPacketReader::ProtocolUnityPacketReader(std::span<const uint8_t> initPayload, const ProtocolUnityContract &initContract) :
	contract(initContract),
	payload(initPayload) { }

bool ProtocolUnityPacketReader::canRead(size_t size) const {
	return position + size <= payload.size();
}

size_t ProtocolUnityPacketReader::remaining() const {
	return payload.size() - position;
}

uint8_t ProtocolUnityPacketReader::readByte() {
	return readLittleEndian<uint8_t>();
}

int16_t ProtocolUnityPacketReader::readI16() {
	return readLittleEndian<int16_t>();
}

uint16_t ProtocolUnityPacketReader::readU16() {
	return readLittleEndian<uint16_t>();
}

int32_t ProtocolUnityPacketReader::readI32() {
	return readLittleEndian<int32_t>();
}

uint32_t ProtocolUnityPacketReader::readU32() {
	return readLittleEndian<uint32_t>();
}

int64_t ProtocolUnityPacketReader::readI64() {
	return readLittleEndian<int64_t>();
}

uint64_t ProtocolUnityPacketReader::readU64() {
	return readLittleEndian<uint64_t>();
}

std::string ProtocolUnityPacketReader::readString() {
	const auto stringLength = readU16();
	if (stringLength > contract.maximumStringLength) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity string length {} exceeds maximum {}.",
			stringLength,
			contract.maximumStringLength
		));
	}

	if (!canRead(stringLength)) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity string requires {} bytes but only {} remain.",
			stringLength,
			remaining()
		));
	}

	std::string value(payload.begin() + static_cast<std::ptrdiff_t>(position), payload.begin() + static_cast<std::ptrdiff_t>(position + stringLength));
	position += stringLength;
	return value;
}

void ProtocolUnityPacketReader::expectFullyConsumed() const {
	if (remaining() != 0) {
		throw ProtocolUnityException(fmt::format("ProtocolUnity payload has {} trailing bytes.", remaining()));
	}
}

ProtocolUnityFrameView ProtocolUnityFrameCodec::decode(std::span<const uint8_t> frameBytes, const ProtocolUnityContract &contract) {
	if (frameBytes.size() < protocolUnityPrefixSize) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity frame is too short for the {}-byte prefix.",
			protocolUnityPrefixSize
		));
	}

	const auto bodyLength = readLittleEndian<uint32_t>(frameBytes, 0);
	if (bodyLength > contract.maximumPacketSize) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity body size {} exceeds maximum {}.",
			bodyLength,
			contract.maximumPacketSize
		));
	}

	if (bodyLength < protocolUnityHeaderSize) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity body size {} is smaller than the {}-byte header.",
			bodyLength,
			protocolUnityHeaderSize
		));
	}

	const auto expectedFrameSize = static_cast<size_t>(contract.framePrefixBytes + bodyLength);
	if (frameBytes.size() < expectedFrameSize) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity frame is incomplete: expected {} bytes, received {}.",
			expectedFrameSize,
			frameBytes.size()
		));
	}

	if (frameBytes.size() > expectedFrameSize) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity frame has {} trailing bytes after the declared body.",
			frameBytes.size() - expectedFrameSize
		));
	}

	const auto version = readLittleEndian<uint16_t>(frameBytes, protocolUnityPrefixSize);
	if (version != contract.protocolVersion) {
		throw ProtocolUnityException(fmt::format(
			"Unsupported ProtocolUnity version {}. Expected {}.",
			version,
			contract.protocolVersion
		));
	}

	const auto rawOpcode = readLittleEndian<uint16_t>(frameBytes, protocolUnityPrefixSize + sizeof(uint16_t));
	const auto &opcode = contract.requireOpcode(rawOpcode);
	const auto payloadOffset = protocolUnityPrefixSize + protocolUnityHeaderSize;
	const auto payloadSize = static_cast<size_t>(bodyLength - protocolUnityHeaderSize);

	return ProtocolUnityFrameView {
		.version = version,
		.opcode = opcode.opcode,
		.payload = frameBytes.subspan(payloadOffset, payloadSize),
	};
}

std::vector<uint8_t> ProtocolUnityFrameCodec::encode(ProtocolUnityOpcode opcode, std::span<const uint8_t> payload, const ProtocolUnityContract &contract) {
	const auto bodySize = static_cast<uint32_t>(protocolUnityHeaderSize + payload.size());
	if (bodySize > contract.maximumPacketSize) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity body size {} exceeds maximum {}.",
			bodySize,
			contract.maximumPacketSize
		));
	}

	(void)contract.requireOpcode(opcode);

	std::vector<uint8_t> frame;
	frame.reserve(contract.framePrefixBytes + bodySize);
	writeLittleEndian<uint32_t>(frame, bodySize);
	writeLittleEndian<uint16_t>(frame, contract.protocolVersion);
	writeLittleEndian<uint16_t>(frame, magic_enum::enum_integer(opcode));
	frame.insert(frame.end(), payload.begin(), payload.end());
	return frame;
}

ProtocolUnityPacketWriter::ProtocolUnityPacketWriter(const ProtocolUnityContract &initContract, ProtocolUnityOpcode initOpcode) :
	contract(initContract),
	opcode(initOpcode) {
	(void)contract.requireOpcode(opcode);
}

void ProtocolUnityPacketWriter::writeByte(uint8_t value) {
	ensurePayloadCapacity(sizeof(value));
	payload.push_back(value);
}

void ProtocolUnityPacketWriter::writeI16(int16_t value) {
	ensurePayloadCapacity(sizeof(value));
	writeLittleEndian(value);
}

void ProtocolUnityPacketWriter::writeU16(uint16_t value) {
	ensurePayloadCapacity(sizeof(value));
	writeLittleEndian(value);
}

void ProtocolUnityPacketWriter::writeI32(int32_t value) {
	ensurePayloadCapacity(sizeof(value));
	writeLittleEndian(value);
}

void ProtocolUnityPacketWriter::writeU32(uint32_t value) {
	ensurePayloadCapacity(sizeof(value));
	writeLittleEndian(value);
}

void ProtocolUnityPacketWriter::writeI64(int64_t value) {
	ensurePayloadCapacity(sizeof(value));
	writeLittleEndian(value);
}

void ProtocolUnityPacketWriter::writeU64(uint64_t value) {
	ensurePayloadCapacity(sizeof(value));
	writeLittleEndian(value);
}

void ProtocolUnityPacketWriter::writeString(std::string_view value) {
	if (value.size() > contract.maximumStringLength) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity string length {} exceeds maximum {}.",
			value.size(),
			contract.maximumStringLength
		));
	}

	ensurePayloadCapacity(sizeof(uint16_t) + value.size());
	writeLittleEndian<uint16_t>(static_cast<uint16_t>(value.size()));
	payload.insert(payload.end(), value.begin(), value.end());
}

std::vector<uint8_t> ProtocolUnityPacketWriter::finalize() const {
	return ProtocolUnityFrameCodec::encode(opcode, payload, contract);
}

void ProtocolUnityPacketWriter::ensurePayloadCapacity(size_t additionalBytes) const {
	const auto nextBodySize = protocolUnityHeaderSize + payload.size() + additionalBytes;
	if (nextBodySize > contract.maximumPacketSize) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity body size {} exceeds maximum {}.",
			nextBodySize,
			contract.maximumPacketSize
		));
	}
}

template <typename T>
T ProtocolUnityPacketReader::readLittleEndian() {
	if (!canRead(sizeof(T))) {
		throw ProtocolUnityException(fmt::format(
			"ProtocolUnity reader needs {} bytes but only {} remain.",
			sizeof(T),
			remaining()
		));
	}

	const auto value = ::readLittleEndian<T>(payload, position);
	position += sizeof(T);
	return value;
}

template <typename T>
void ProtocolUnityPacketWriter::writeLittleEndian(T value) {
	::writeLittleEndian(payload, value);
}

template uint8_t ProtocolUnityPacketReader::readLittleEndian<uint8_t>();
template int16_t ProtocolUnityPacketReader::readLittleEndian<int16_t>();
template uint16_t ProtocolUnityPacketReader::readLittleEndian<uint16_t>();
template int32_t ProtocolUnityPacketReader::readLittleEndian<int32_t>();
template uint32_t ProtocolUnityPacketReader::readLittleEndian<uint32_t>();
template int64_t ProtocolUnityPacketReader::readLittleEndian<int64_t>();
template uint64_t ProtocolUnityPacketReader::readLittleEndian<uint64_t>();
template void ProtocolUnityPacketWriter::writeLittleEndian<uint16_t>(uint16_t value);
template void ProtocolUnityPacketWriter::writeLittleEndian<uint32_t>(uint32_t value);
template void ProtocolUnityPacketWriter::writeLittleEndian<uint64_t>(uint64_t value);

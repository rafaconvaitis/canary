local scanRuntimePath = "protocolunity-golden-zone-scan.lua"

local function fileExists(path)
	local file = io.open(path, "r")
	if not file then
		return false
	end

	file:close()
	return true
end

local function tryLoadScanConfig()
	if os.getenv("TIBIAUNITY_PROTOCOLUNITY_GOLDEN_ZONE_SCAN") ~= "1" then
		return nil
	end

	if not fileExists(scanRuntimePath) then
		return nil
	end

	local ok, configOrError = pcall(dofile, scanRuntimePath)
	if not ok then
		logger.error("[ProtocolUnityGoldenZoneScan] Failed to load {}: {}", scanRuntimePath, configOrError)
		return nil
	end

	if type(configOrError) ~= "table" or type(configOrError.candidates) ~= "table" then
		return nil
	end

	return configOrError
end

local function jsonEscape(value)
	value = tostring(value or "")
	value = value:gsub("\\", "\\\\")
	value = value:gsub("\"", "\\\"")
	value = value:gsub("\n", "\\n")
	value = value:gsub("\r", "\\r")
	value = value:gsub("\t", "\\t")
	return value
end

local function isArray(value)
	if type(value) ~= "table" then
		return false
	end

	local count = 0
	local maxIndex = 0
	for key in pairs(value) do
		if type(key) ~= "number" then
			return false
		end

		count = count + 1
		if key > maxIndex then
			maxIndex = key
		end
	end

	return count == maxIndex
end

local function encodeJson(value)
	local valueType = type(value)
	if valueType == "nil" then
		return "null"
	end

	if valueType == "string" then
		return "\"" .. jsonEscape(value) .. "\""
	end

	if valueType == "number" or valueType == "boolean" then
		return tostring(value)
	end

	if valueType ~= "table" then
		return "\"" .. jsonEscape(tostring(value)) .. "\""
	end

	if isArray(value) then
		local entries = {}
		for index = 1, #value do
			entries[#entries + 1] = encodeJson(value[index])
		end

		return "[" .. table.concat(entries, ",") .. "]"
	end

	local entries = {}
	for key, nestedValue in pairs(value) do
		entries[#entries + 1] = "\"" .. jsonEscape(key) .. "\":" .. encodeJson(nestedValue)
	end

	table.sort(entries)
	return "{" .. table.concat(entries, ",") .. "}"
end

local function toPosition(value)
	return Position(value.x, value.y, value.z)
end

local function isStructurallyWalkable(tile)
	return tile and tile:isWalkable(false, false, true, true, true)
end

local function countTileMetrics(tile, metrics)
	if not tile then
		metrics.missingTiles = metrics.missingTiles + 1
		return
	end

	metrics.existingTiles = metrics.existingTiles + 1
	if isStructurallyWalkable(tile) then
		metrics.walkableTiles = metrics.walkableTiles + 1
	else
		metrics.blockedTiles = metrics.blockedTiles + 1
	end

	if tile:hasFlag(TILESTATE_PROTECTIONZONE) then
		metrics.protectionZoneTiles = metrics.protectionZoneTiles + 1
	end

	if tile:hasFlag(TILESTATE_FLOORCHANGE) then
		metrics.floorChangeTiles = metrics.floorChangeTiles + 1
	end

	if tile:hasFlag(TILESTATE_HOUSE) then
		metrics.houseTiles = metrics.houseTiles + 1
	end

	local ground = tile:getGround()
	if ground then
		local groundName = ground:getName():lower()
		if groundName:find("water", 1, true) or groundName:find("swamp", 1, true) then
			metrics.waterTiles = metrics.waterTiles + 1
		end
	end

	local creature = tile:getTopCreature()
	if creature then
		metrics.creatureCount = metrics.creatureCount + 1
		if creature:isPlayer() then
			metrics.playerCount = metrics.playerCount + 1
		elseif creature:isNpc() then
			metrics.npcCount = metrics.npcCount + 1
		else
			metrics.monsterCount = metrics.monsterCount + 1
		end
	end

	local items = tile:getItems()
	if items then
		for index = 1, tile:getItemCount() do
			local item = items[index]
			if item then
				if item:isTeleport() then
					metrics.teleportTiles = metrics.teleportTiles + 1
				end

				if item:getType():isMovable() then
					metrics.movableItems = metrics.movableItems + 1
				end
			end
		end
	end
end

local function countContiguousWalkable(center, radius)
	local startTile = Tile(center)
	if not isStructurallyWalkable(startTile) then
		return 0
	end

	local visited = {}
	local queue = { center }
	local head = 1
	local total = 0

	local function key(position)
		return position.x .. ":" .. position.y .. ":" .. position.z
	end

	visited[key(center)] = true

	while head <= #queue do
		local current = queue[head]
		head = head + 1
		total = total + 1

		local offsets = {
			{ x = -1, y = 0 },
			{ x = 1, y = 0 },
			{ x = 0, y = -1 },
			{ x = 0, y = 1 },
		}

		for _, offset in ipairs(offsets) do
			local candidate = Position(current.x + offset.x, current.y + offset.y, current.z)
			if math.abs(candidate.x - center.x) <= radius and math.abs(candidate.y - center.y) <= radius then
				local candidateKey = key(candidate)
				if not visited[candidateKey] then
					visited[candidateKey] = true
					local tile = Tile(candidate)
					if isStructurallyWalkable(tile) then
						queue[#queue + 1] = candidate
					end
				end
			end
		end
	end

	return total
end

local function evaluateCandidate(candidate, radius)
	local center = toPosition(candidate.center)
	local metrics = {
		id = candidate.id or "unknown",
		label = candidate.label or candidate.id or "Unknown",
		center = { x = center.x, y = center.y, z = center.z },
		radius = radius,
		totalTiles = (radius * 2 + 1) * (radius * 2 + 1),
		existingTiles = 0,
		missingTiles = 0,
		walkableTiles = 0,
		blockedTiles = 0,
		protectionZoneTiles = 0,
		floorChangeTiles = 0,
		houseTiles = 0,
		waterTiles = 0,
		teleportTiles = 0,
		movableItems = 0,
		creatureCount = 0,
		playerCount = 0,
		npcCount = 0,
		monsterCount = 0,
	}

	for x = center.x - radius, center.x + radius do
		for y = center.y - radius, center.y + radius do
			countTileMetrics(Tile(Position(x, y, center.z)), metrics)
		end
	end

	metrics.centerWalkable = isStructurallyWalkable(Tile(center))
	metrics.contiguousWalkableTiles = countContiguousWalkable(center, radius)
	metrics.walkableCoverage = metrics.totalTiles > 0 and (metrics.contiguousWalkableTiles / metrics.totalTiles) or 0
	metrics.score = metrics.contiguousWalkableTiles
		- (metrics.blockedTiles * 0.5)
		- (metrics.monsterCount * 35)
		- (metrics.npcCount * 45)
		- (metrics.playerCount * 200)
		- (metrics.protectionZoneTiles * 4)
		- (metrics.floorChangeTiles * 6)
		- (metrics.teleportTiles * 40)
		- (metrics.houseTiles * 3)
		- (metrics.movableItems * 0.25)

	if not metrics.centerWalkable then
		metrics.score = metrics.score - 400
	end

	metrics.summary = string.format(
		"walkable=%d contiguous=%d monsters=%d npcs=%d players=%d teleports=%d pz=%d",
		metrics.walkableTiles,
		metrics.contiguousWalkableTiles,
		metrics.monsterCount,
		metrics.npcCount,
		metrics.playerCount,
		metrics.teleportTiles,
		metrics.protectionZoneTiles
	)

	return metrics
end

local function writeScanReport(report)
	local file = io.open(report.outputPath, "w")
	if not file then
		logger.error("[ProtocolUnityGoldenZoneScan] Could not open {}", report.outputPath)
		return false
	end

	file:write(encodeJson(report))
	file:close()
	return true
end

local function runScan()
	local config = tryLoadScanConfig()
	if not config then
		return true
	end

	local candidates = {}
	for _, candidate in ipairs(config.candidates) do
		candidates[#candidates + 1] = evaluateCandidate(candidate, config.radius or 15)
	end

	table.sort(candidates, function(left, right)
		return left.score > right.score
	end)

	local report = {
		schemaVersion = 1,
		generatedAtUtc = os.date("!%Y-%m-%dT%H:%M:%SZ"),
		radius = config.radius or 15,
		outputPath = config.outputPath,
		selectedCandidateId = candidates[1] and candidates[1].id or "",
		selectedCandidateLabel = candidates[1] and candidates[1].label or "",
		candidatesEvaluated = #candidates,
		candidates = candidates,
	}

	if writeScanReport(report) then
		logger.info(
			"[ProtocolUnityGoldenZoneScan] Wrote {} candidates to {}; best={} score={}",
			#candidates,
			config.outputPath,
			report.selectedCandidateId,
			candidates[1] and candidates[1].score or 0
		)
	end

	return true
end

local protocolUnityGoldenZoneScan = GlobalEvent("ProtocolUnityGoldenZoneScan")

function protocolUnityGoldenZoneScan.onStartup()
	return runScan()
end

protocolUnityGoldenZoneScan:register()

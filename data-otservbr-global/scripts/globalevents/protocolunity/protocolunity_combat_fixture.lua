local fixtureRuntimePath = "protocolunity-combat-fixture.lua"
local pickupProbeItemId = 3091
local loginRefreshGeneration = 0
local dedicatedAreaReady = false
local protocolUnityViewRadiusX = 8
local protocolUnityViewRadiusY = 6
local protocolUnityViewPositiveOffsetX = protocolUnityViewRadiusX + 1
local protocolUnityViewPositiveOffsetY = protocolUnityViewRadiusY + 1
local formationPatterns = {
	{ alpha = { x = -1, y = 0 }, beta = { x = 1, y = 0 } },
	{ alpha = { x = 1, y = 0 }, beta = { x = -1, y = 0 } },
	{ alpha = { x = 0, y = -1 }, beta = { x = 0, y = 1 } },
	{ alpha = { x = 0, y = 1 }, beta = { x = 0, y = -1 } },
}

local function tryLoadFixture()
	local fixtureEnabledFlag = os.getenv("TIBIAUNITY_PROTOCOLUNITY_FIXTURE")
	local file = io.open(fixtureRuntimePath, "r")
	if not file then
		return nil
	end

	file:close()
	local ok, fixtureOrError = pcall(dofile, fixtureRuntimePath)
	if not ok then
		logger.error("[ProtocolUnityCombatFixture] Failed to load {}: {}", fixtureRuntimePath, fixtureOrError)
		return nil
	end

	if type(fixtureOrError) ~= "table" or not fixtureOrError.enabled then
		return nil
	end

	if fixtureOrError.developmentOnly and fixtureEnabledFlag ~= "1" then
		logger.info("[ProtocolUnityCombatFixture] Ignored {} because development-only mode is disabled", fixtureRuntimePath)
		return nil
	end

	return fixtureOrError
end

local function toPosition(value)
	return Position(value.x, value.y, value.z)
end

local function isInsideConfiguredBox(x, y, box)
	return box
		and box.from
		and box.to
		and x >= box.from.x and x <= box.to.x
		and y >= box.from.y and y <= box.to.y
end

local function resolveDevelopmentGroundId(area, x, y)
	if isInsideConfiguredBox(x, y, area.bridge) then
		return area.dirtGroundId
	end

	local center = area.center
	local diagonal = (x - center.x) - (y - center.y) - (area.riverOffset or 0)
	if math.abs(diagonal) <= (area.riverHalfWidth or 0) then
		return area.waterGroundId
	end

	local pathDistance = math.abs((x - center.x) + math.floor((y - center.y) * 0.55))
	if pathDistance <= 1 and math.abs(y - center.y) <= 15 then
		return area.dirtGroundId
	end

	return area.grassGroundId
end

local function ensureDedicatedDevelopmentArea(fixture)
	local area = fixture.developmentArea
	if dedicatedAreaReady or not area or not area.enabled then
		return true
	end

	if not fixture.developmentOnly or os.getenv("TIBIAUNITY_PROTOCOLUNITY_FIXTURE") ~= "1" then
		logger.error("[ProtocolUnityCombatFixture] Refused to create dedicated area outside development-only fixture mode")
		return false
	end

	local createdTiles = 0
	local createdGrounds = 0
	for x = area.bounds.from.x, area.bounds.to.x do
		for y = area.bounds.from.y, area.bounds.to.y do
			local position = Position(x, y, area.bounds.from.z)
			local tile = Tile(position) or Game.createTile(position, true)
			if tile then
				createdTiles = createdTiles + 1
				if not tile:getGround() then
					local groundId = resolveDevelopmentGroundId(area, x, y)
					if Game.createItem(groundId, 1, position) then
						createdGrounds = createdGrounds + 1
					end
				end
			end
		end
	end

	dedicatedAreaReady = createdTiles > 0 and createdGrounds > 0
	logger.info(
		"[ProtocolUnityCombatFixture] Prepared development-only Golden Clearing area with {} tiles and {} grounds",
		createdTiles,
		createdGrounds
	)
	return dedicatedAreaReady
end

local function hasBlockingItem(tile)
	local items = tile:getItems()
	if not items then
		return false
	end

	for index = 1, tile:getItemCount() do
		local item = items[index]
		local itemType = item:getType()
		if itemType:getType() ~= ITEM_TYPE_MAGICFIELD and not itemType:isMovable() and item:hasProperty(CONST_PROP_BLOCKSOLID) then
			return true
		end
	end

	return false
end

local function removeCreatureIfNeeded(creature)
	if creature and not creature:isPlayer() then
		creature:remove()
	end
end

local function canUseFixtureTile(position, allowedPlayerName)
	local tile = Tile(position)
	if not tile then
		return false
	end

	if tile:hasFlag(TILESTATE_PROTECTIONZONE) then
		return false
	end

	local ground = tile:getGround()
	if not ground or ground:hasProperty(CONST_PROP_BLOCKSOLID) then
		return false
	end

	local creature = tile:getTopCreature()
	if creature then
		if not creature:isPlayer() then
			return false
		end

		if not allowedPlayerName or creature:getName() ~= allowedPlayerName then
			return false
		end
	end

	return not hasBlockingItem(tile)
end

local function isSpawnableTargetTile(position)
	local tile = Tile(position)
	if not tile then
		return false
	end

	local creature = tile:getTopCreature()
	if creature then
		return false
	end

	return canUseFixtureTile(position, nil)
end

local function buildFixtureArea(alphaPosition, betaPosition, targetPosition, padding)
	local margin = padding or 3
	return {
		from = {
			x = math.min(alphaPosition.x, betaPosition.x, targetPosition.x) - margin,
			y = math.min(alphaPosition.y, betaPosition.y, targetPosition.y) - margin,
			z = targetPosition.z,
		},
		to = {
			x = math.max(alphaPosition.x, betaPosition.x, targetPosition.x) + margin,
			y = math.max(alphaPosition.y, betaPosition.y, targetPosition.y) + margin,
			z = targetPosition.z,
		},
	}
end

local function buildProtocolUnityViewportArea(anchorPosition, padding)
	local margin = padding or 0
	return {
		from = {
			x = anchorPosition.x - protocolUnityViewRadiusX - margin,
			y = anchorPosition.y - protocolUnityViewRadiusY - margin,
			z = anchorPosition.z,
		},
		to = {
			x = anchorPosition.x + protocolUnityViewPositiveOffsetX + margin,
			y = anchorPosition.y + protocolUnityViewPositiveOffsetY + margin,
			z = anchorPosition.z,
		},
	}
end

local function countFormationVisualNoise(fixture, formation)
	local alphaName = fixture.players and fixture.players.alpha and fixture.players.alpha.name
	local betaName = fixture.players and fixture.players.beta and fixture.players.beta.name
	local targetName = fixture.target and fixture.target.name
	local noise = 0
	local viewportArea = buildProtocolUnityViewportArea(formation.alpha, 0)

	for x = viewportArea.from.x, viewportArea.to.x do
		for y = viewportArea.from.y, viewportArea.to.y do
			local tile = Tile(Position(x, y, formation.alpha.z))
			if tile then
				local creature = tile:getTopCreature()
				if creature then
					if creature:isPlayer() then
						local playerName = creature:getName()
						if playerName ~= alphaName and playerName ~= betaName then
							noise = noise + 6
						end
					elseif creature:getName() ~= targetName then
						noise = noise + 10
					end
				end

				local items = tile:getItems()
				if items then
					for index = 1, tile:getItemCount() do
						local item = items[index]
						if item and item:getId() ~= pickupProbeItemId and item:getType():isMovable() then
							noise = noise + 1
						end
					end
				end
			end
		end
	end

	return noise
end

local function collectSearchPositions(origin, radius)
	local positions = {}
	for distance = 0, radius do
		for offsetX = -distance, distance do
			local absOffsetY = distance - math.abs(offsetX)
			local offsetYs = absOffsetY == 0 and { 0 } or { -absOffsetY, absOffsetY }
			for _, offsetY in ipairs(offsetYs) do
				positions[#positions + 1] = Position(origin.x + offsetX, origin.y + offsetY, origin.z)
			end
		end
	end

	return positions
end

local function offsetPosition(position, offset)
	return Position(position.x + offset.x, position.y + offset.y, position.z)
end

local function isSamePosition(left, right)
	return left and right and left.x == right.x and left.y == right.y and left.z == right.z
end

local function resolvePickupProbePosition(formation)
	for offsetY = -2, 2 do
		for offsetX = -2, 2 do
			local candidate = Position(formation.target.x + offsetX, formation.target.y + offsetY, formation.target.z)
			local alphaInRange = math.abs(candidate.x - formation.alpha.x) <= 1 and math.abs(candidate.y - formation.alpha.y) <= 1
			local betaInRange = math.abs(candidate.x - formation.beta.x) <= 1 and math.abs(candidate.y - formation.beta.y) <= 1
			if alphaInRange
				and betaInRange
				and not isSamePosition(candidate, formation.alpha)
				and not isSamePosition(candidate, formation.beta)
				and not isSamePosition(candidate, formation.target)
				and canUseFixtureTile(candidate, nil) then
				return candidate
			end
		end
	end

	return nil
end

local function resolvePickupProbeFromFixture(fixture, formation)
	local pickupProbe = fixture.formation and fixture.formation.pickupProbe
	if pickupProbe then
		return Position(pickupProbe.x, pickupProbe.y, pickupProbe.z)
	end

	return resolvePickupProbePosition(formation)
end

local function hasClearSight(fromPosition, toPosition)
	if not fromPosition or not toPosition then
		return false
	end

	return fromPosition:isSightClear(toPosition, true)
end

local function cleanupArea(area)
	for x = area.from.x, area.to.x do
		for y = area.from.y, area.to.y do
			local position = Position(x, y, area.from.z)
			local tile = Tile(position)
			if tile then
				removeCreatureIfNeeded(tile:getTopCreature())
				local items = tile:getItems()
				if items then
					-- Unity owns scenery in this isolated development area; clear corpse containers too.
					for index = #items, 1, -1 do
						local item = items[index]
						if item then
							item:remove()
						end
					end
				end
			end
		end
	end
end

local function isInsideArea(position, area)
	return area
		and area.from
		and area.to
		and position.z == area.from.z
		and position.x >= area.from.x and position.x <= area.to.x
		and position.y >= area.from.y and position.y <= area.to.y
end

local function resolveFixturePlayerKey(fixture, player)
	if not fixture.players then
		return nil
	end

	local playerName = player:getName()
	for slotKey, slot in pairs(fixture.players) do
		if slot and slot.name == playerName then
			return slotKey
		end
	end

	return nil
end

local function resolveStaticFormation(fixture)
	if not fixture.formation or not fixture.formation.alpha or not fixture.formation.beta or not fixture.formation.target then
		return nil
	end

	local alphaPosition = toPosition(fixture.formation.alpha)
	local betaPosition = toPosition(fixture.formation.beta)
	local targetPosition = toPosition(fixture.formation.target)
	local cleanupRadius = fixture.search and fixture.search.cleanupRadius or (fixture.search and fixture.search.areaPadding) or 3

	if not canUseFixtureTile(alphaPosition, fixture.players and fixture.players.alpha and fixture.players.alpha.name) then
		return nil
	end

	if not canUseFixtureTile(betaPosition, fixture.players and fixture.players.beta and fixture.players.beta.name) then
		return nil
	end

	local targetTile = Tile(targetPosition)
	if not targetTile or not targetTile:getGround() or targetTile:hasFlag(TILESTATE_PROTECTIONZONE) or hasBlockingItem(targetTile) then
		return nil
	end

	return {
		alpha = alphaPosition,
		beta = betaPosition,
		target = targetPosition,
		area = buildFixtureArea(alphaPosition, betaPosition, targetPosition, cleanupRadius),
		viewportArea = buildProtocolUnityViewportArea(alphaPosition, 1),
	}
end

local function resolveFixtureFormation(fixture)
	local staticFormation = resolveStaticFormation(fixture)
	if staticFormation then
		return staticFormation
	end

	local search = fixture.search or {}
	local originValue = search.origin
	if not originValue then
		return nil
	end

	local alphaSlot = fixture.players and fixture.players.alpha
	local betaSlot = fixture.players and fixture.players.beta
	if not alphaSlot or not betaSlot then
		return nil
	end

	local origin = toPosition(originValue)
	local searchRadius = search.radius or 18
	local areaPadding = search.areaPadding or 3
	local bestFormation = nil
	local bestNoise = math.huge

	for _, targetPosition in ipairs(collectSearchPositions(origin, searchRadius)) do
		if isSpawnableTargetTile(targetPosition) then
			for _, pattern in ipairs(formationPatterns) do
				local alphaPosition = offsetPosition(targetPosition, pattern.alpha)
				local betaPosition = offsetPosition(targetPosition, pattern.beta)
				if canUseFixtureTile(alphaPosition, alphaSlot.name)
					and canUseFixtureTile(betaPosition, betaSlot.name)
					and hasClearSight(alphaPosition, targetPosition)
					and hasClearSight(betaPosition, targetPosition) then
					local formation = {
						alpha = alphaPosition,
						beta = betaPosition,
						target = targetPosition,
						area = buildFixtureArea(alphaPosition, betaPosition, targetPosition, areaPadding),
						viewportArea = buildProtocolUnityViewportArea(alphaPosition, 1),
					}
					local noise = countFormationVisualNoise(fixture, formation)
					if noise < bestNoise then
						bestFormation = formation
						bestNoise = noise
						if bestNoise == 0 then
							return bestFormation
						end
					end
				end
			end
		end
	end

	return bestFormation
end

local function teleportFixturePlayers(fixture, formation)
	for slotKey, slot in pairs(fixture.players or {}) do
		local player = slot and slot.name and Player(slot.name) or nil
		local destination = formation[slotKey]
		if player and destination then
			player:removeCondition(CONDITION_PARALYZE)
			player:teleportTo(destination)
			player:getPosition():sendMagicEffect(CONST_ME_TELEPORT)
		end
	end
end

local function spawnFixtureTarget(fixture, formation)
	local monster = Game.createMonster(fixture.target.name, formation.target, false, false)
	if not monster then
		monster = Game.createMonster(fixture.target.name, formation.target, true, false)
	end

	if monster then
		monster:registerEvent("ProtocolUnityCombatFixtureDeath")
		logger.info(
			"[ProtocolUnityCombatFixture] Spawned {} at {} with alpha {} and beta {}",
			fixture.target.name,
			monster:getPosition():toString(),
			formation.alpha:toString(),
			formation.beta:toString()
		)
		return true
	end

	return false
end

local function spawnAuxiliaryCreatures(fixture)
	local anchorName = fixture.players and fixture.players.alpha and fixture.players.alpha.name
	local anchorPlayer = anchorName and Player(anchorName) or nil
	local keepAllPassiveForSoak = os.getenv("TIBIAUNITY_PROTOCOLUNITY_SOAK") == "1"
	for _, definition in ipairs(fixture.auxiliaryCreatures or {}) do
		local position = toPosition(definition.position)
		local tile = Tile(position)
		if tile and not tile:getTopCreature() then
			local creature = Game.createMonster(definition.name, position, false, false)
			if not creature then
				creature = Game.createMonster(definition.name, position, true, false)
			end

			if creature then
				local keepPassive = keepAllPassiveForSoak or definition.name ~= "Golden Moss Beast"
				if definition.name == "Golden Moss Beast" then
					creature:setMoveLocked(true)
				end
				if keepPassive then
					if keepAllPassiveForSoak and anchorPlayer then
						creature:setMaster(anchorPlayer)
					end
					creature:setTarget(nil)
					creature:setFollowCreature(nil)
					creature:setMoveLocked(true)
				end
				logger.info("[ProtocolUnityCombatFixture] Spawned auxiliary {} at {}", definition.name, position:toString())
			else
				logger.warn("[ProtocolUnityCombatFixture] Could not spawn auxiliary {} at {}", definition.name, position:toString())
			end
		end
	end
end

local function refreshFixtureTarget(fixture, reason)
	local formation = resolveFixtureFormation(fixture)
	if not formation then
		logger.error(
			"[ProtocolUnityCombatFixture] Could not resolve a combat formation near {} within radius {} for fixture {} after {}",
			toPosition(fixture.search.origin):toString(),
			fixture.search.radius or 18,
			fixture.fixtureName or "(unnamed)",
			reason or "refresh"
		)
		return false
	end

	cleanupArea(formation.area)
	cleanupArea(formation.viewportArea)
	teleportFixturePlayers(fixture, formation)

	if not spawnFixtureTarget(fixture, formation) then
		logger.error("[ProtocolUnityCombatFixture] Could not place {} in fixture {} after {}", fixture.target.name, fixture.fixtureName or "(unnamed)", reason or "refresh")
		return false
	end

	spawnAuxiliaryCreatures(fixture)

	local pickupProbePosition = resolvePickupProbeFromFixture(fixture, formation)
	if pickupProbePosition then
		spawnPickupProbe(pickupProbePosition)
	else
		logger.warn("[ProtocolUnityCombatFixture] Could not resolve a shared pickup probe tile near {}", formation.target:toString())
	end

	return true, formation
end

local function areFixturePlayersOnline(fixture)
	local alphaName = fixture.players and fixture.players.alpha and fixture.players.alpha.name
	local betaName = fixture.players and fixture.players.beta and fixture.players.beta.name
	return alphaName and betaName and Player(alphaName) and Player(betaName)
end

local function cleanupResolvedFormation(fixture)
	local formation = resolveFixtureFormation(fixture)
	if formation then
		cleanupArea(formation.area)
		cleanupArea(formation.viewportArea)
	else
		logger.warn("[ProtocolUnityCombatFixture] Startup cleanup could not resolve a combat formation for fixture {}", fixture.fixtureName or "(unnamed)")
	end
end

local function getFixtureDestinationForPlayer(fixture, player)
	local slotKey = resolveFixturePlayerKey(fixture, player)
	if not slotKey then
		return nil
	end

	local formation = resolveFixtureFormation(fixture)
	if not formation then
		return nil
	end

	return formation[slotKey], formation
end

local function resetLoginPlayer(player, fixture)
	local destination, formation = getFixtureDestinationForPlayer(fixture, player)
	if not destination or not formation then
		logger.error("[ProtocolUnityCombatFixture] Could not resolve a destination for {}", player:getName())
		return false
	end

	cleanupArea(formation.area)
	cleanupArea(formation.viewportArea)
	teleportFixturePlayers(fixture, formation)

	if not isInsideArea(player:getPosition(), formation.area) then
		logger.error("[ProtocolUnityCombatFixture] Player {} did not reach fixture {} after teleport", player:getName(), fixture.fixtureName or "(unnamed)")
		return false
	end

	if not spawnFixtureTarget(fixture, formation) then
		logger.error("[ProtocolUnityCombatFixture] Could not respawn {} for {}", fixture.target.name, player:getName())
		return false
	end

	spawnAuxiliaryCreatures(fixture)

	return true
end

local function spawnPickupProbe(position)
	local tile = Tile(position)
	if not tile then
		logger.warn("[ProtocolUnityCombatFixture] Could not resolve tile for pickup probe at {}", position:toString())
		return
	end

	if tile:getItemById(pickupProbeItemId) then
		logger.info("[ProtocolUnityCombatFixture] Pickup probe already exists at {}", position:toString())
		return
	end

	local item = Game.createItem(pickupProbeItemId, 1, position)
	if item then
		logger.info("[ProtocolUnityCombatFixture] Spawned pickup probe {} at {}", item:getName(), position:toString())
	else
		logger.error("[ProtocolUnityCombatFixture] Failed to create pickup probe item {} at {}", pickupProbeItemId, position:toString())
	end
end

local function resolveLoginFixture(fixture, player)
	local slotKey = resolveFixturePlayerKey(fixture, player)
	if not slotKey then
		return nil
	end

	return fixture.players[slotKey]
end

local function ensurePlayerLoginPosition(slot)
	if not slot or not slot.login then
		return nil
	end

	return toPosition(slot.login)
end

local function stageFixturePlayer(player, slot)
	local loginPosition = ensurePlayerLoginPosition(slot)
	if not loginPosition then
		return false
	end

	player:teleportTo(loginPosition)
	player:getPosition():sendMagicEffect(CONST_ME_TELEPORT)
	return true
end

local function refreshFixtureForPlayer(player, fixture)
	local slot = resolveLoginFixture(fixture, player)
	if not slot then
		return true
	end

	if not stageFixturePlayer(player, slot) then
		return false
	end

	if not areFixturePlayersOnline(fixture) then
		return true
	end

	return resetLoginPlayer(player, fixture)
end

local function prepareFixtureOnStartup(fixture)
	if areFixturePlayersOnline(fixture) then
		local ok = refreshFixtureTarget(fixture, "startup")
		if not ok then
			logger.warn("[ProtocolUnityCombatFixture] Startup spawn deferred until a fixture player logs in")
		end
	end
end

local function initializeFixture(fixture)
	if not ensureDedicatedDevelopmentArea(fixture) then
		logger.error("[ProtocolUnityCombatFixture] Dedicated Golden Clearing area preparation failed")
		return
	end

	if fixture.search and fixture.search.origin then
		cleanupResolvedFormation(fixture)
		return
	end

	if fixture.area then
		cleanupArea(fixture.area)
	end
end

local function resolveLiveFixture()
	local fixture = tryLoadFixture()
	if not fixture then
		return nil
	end

	return fixture
end

local function refreshAfterLogin(playerId)
	local loginPlayer = Player(playerId)
	if not loginPlayer then
		return
	end

	local fixture = resolveLiveFixture()
	if not fixture then
		return
	end

	refreshFixtureForPlayer(loginPlayer, fixture)
end

local function scheduleLoginRefresh(playerId)
	loginRefreshGeneration = loginRefreshGeneration + 1
	local generation = loginRefreshGeneration

	addEvent(function()
		if generation ~= loginRefreshGeneration then
			return
		end

		refreshAfterLogin(playerId)
	end, 400)
end

local function cancelScheduledLoginRefresh()
	loginRefreshGeneration = loginRefreshGeneration + 1
end

local function onFixtureStartup()
	local fixture = tryLoadFixture()
	if not fixture then
		return true
	end

	initializeFixture(fixture)
	prepareFixtureOnStartup(fixture)
	return true
end

local function onFixtureLogin(player)
	local fixture = resolveLiveFixture()
	if not fixture then
		return true
	end

	local slot = resolveLoginFixture(fixture, player)
	if not slot then
		return true
	end

	local refreshed = refreshFixtureForPlayer(player, fixture)
	if not refreshed then
		logger.warn("[ProtocolUnityCombatFixture] Immediate login refresh failed for {}; scheduling retry", player:getName())
		scheduleLoginRefresh(player:getId())
		return true
	end

	if not areFixturePlayersOnline(fixture) then
		scheduleLoginRefresh(player:getId())
	else
		cancelScheduledLoginRefresh()
	end

	return true
end

local protocolUnityCombatFixture = GlobalEvent("ProtocolUnityCombatFixture")

function protocolUnityCombatFixture.onStartup()
	return onFixtureStartup()
end

protocolUnityCombatFixture:register()

local fixtureLogin = CreatureEvent("ProtocolUnityCombatFixtureLogin")

function fixtureLogin.onLogin(player)
	return onFixtureLogin(player)
end

fixtureLogin:register()

local fixtureDeath = CreatureEvent("ProtocolUnityCombatFixtureDeath")

function fixtureDeath.onDeath(creature)
	local fixture = resolveLiveFixture()
	if not fixture or not creature or creature:isPlayer() or creature:getName() ~= fixture.target.name then
		return true
	end

	local deathPosition = creature:getPosition()
	logger.info("[ProtocolUnityCombatFixture] {} died at {}; scheduling pickup probe", creature:getName(), deathPosition:toString())
	addEvent(function()
		spawnPickupProbe(deathPosition)
	end, 100)

	return true
end

fixtureDeath:register()

local mType = Game.createMonsterType("Golden Moss Beast")
local monster = {}

monster.description = "a golden moss beast"
monster.experience = 120
monster.outfit = {
	lookType = 21,
}

monster.health = 180
monster.maxHealth = 180
monster.race = "venom"
monster.corpse = 5966
monster.speed = 110

monster.changeTarget = {
	interval = 2000,
	chance = 0,
}

monster.flags = {
	summonable = false,
	attackable = true,
	hostile = true,
	convinceable = false,
	illusionable = false,
	pushable = false,
	rewardBoss = false,
	canPushItems = false,
	canPushCreatures = false,
	staticAttackChance = 90,
	targetDistance = 1,
	runHealth = 0,
	healthHidden = false,
	isBlockable = true,
	canWalkOnEnergy = false,
	canWalkOnFire = false,
	canWalkOnPoison = true,
}

monster.light = {
	level = 2,
	color = 194,
}

monster.voices = {}

monster.loot = {
	{ name = "meat", chance = 65000, maxCount = 1 },
}

monster.attacks = {
	{ name = "melee", interval = 1800, chance = 100, minDamage = 0, maxDamage = -12 },
}

monster.defenses = {
	defense = 8,
	armor = 6,
	mitigation = 0.1,
}

monster.elements = {
	{ type = COMBAT_PHYSICALDAMAGE, percent = 5 },
	{ type = COMBAT_EARTHDAMAGE, percent = 35 },
	{ type = COMBAT_FIREDAMAGE, percent = -15 },
}

monster.immunities = {
	{ type = "paralyze", condition = false },
	{ type = "outfit", condition = false },
	{ type = "invisible", condition = false },
	{ type = "bleed", condition = false },
}

mType:register(monster)

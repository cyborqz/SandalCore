// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

//go:build aoc_standin

package citynode

// The stand-in for constants_gen.go in the data-free build (go build, go vet and go test with -tags aoc_standin), which the
// continuous-integration job uses on a machine without game data. constants_gen.go is made from the player's own game install
// by scripts/extract_game_data.ps1 and is not in the repository; this file declares the same names with placeholder values.
// The level model is this package's own (LevelTown, LevelVillage); every other value is a placeholder, never the game's.
// A test that needs the real values skips itself with standin.Skip.
//
// With constants_gen.go present the compiler reports every name below as redeclared: never use the tag on a machine that has
// game data. When tools/capture/gen_node_constants.py gains a constant, add its name here with a placeholder value.

// ExpToLevel: one placeholder per level, rising by one.
var ExpToLevel = [...]int64{0, 1, 2, 3, 4, 5, 6}

// ReputationToLevel: a placeholder.
var ReputationToLevel = [...]int64{0}

// MaxNodeLevel and MinimumOperationalNodeLevel: the level model of this package.
const MaxNodeLevel = LevelTown
const MinimumOperationalNodeLevel = LevelVillage

const NodeCitizenshipPlayerLevelRequirement = 0
const NodePassiveIncomeSeconds = 0
const MaxNumberNodeNotifications = 0

var MayoralCommissionHandRefreshSeconds = [...]int64{0}

const VassalshipSystemEnabled = false
const VassalshipProtectionEnabled = false

const VassalshipDefinitionGuid int64 = 0
const BuyOrderConstantsGuid int64 = 0
const ElectionConstantsGuid int64 = 0
const WarConstantsGuid int64 = 0
const CitizenshipRewardGuid int64 = 0
const NodeCurrencyGuid int64 = 0
const NodeAlignmentGuid int64 = 0
const NodeWarDeclaringAlignmentGuid int64 = 0
const NodeWarTargetAlignmentGuid int64 = 0
const NodeSiegeDeclaringAlignmentGuid int64 = 0
const NodeSiegeTargetAlignmentGuid int64 = 0
const NodeLevelUpCountdownToastGuid int64 = 0
const NodeCitizenshipGainToastGuid int64 = 0

const ServiceBuildingDefaultRepairSeconds = 0
const ServiceBuildingDefaultDemolitionSeconds = 0
const ServiceBuildingDemolitionVotingEnabled = false
const ServiceBuildingMaintenanceFrequencyWeeks = 0
const ServiceBuildingMaintenanceDayOfWeek = ""
const ZoiPlayerCountRefreshSeconds = 0
const MayorMessageCooldownSeconds = 0
const MaxMayorMessageLength = 0
const StaticHouseUpgradeSeconds = 0
const SiegeJoinRequestCooldownSeconds = 0

const SiegeInactiveTag = ""
const SiegePrepTag = ""
const SiegeActiveTag = ""
const SiegePillageTag = ""

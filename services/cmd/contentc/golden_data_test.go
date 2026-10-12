// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

// Golden data for the class attributes: what the six Player_Base_Attrib_* StatCurve
// rows and the 48 formulas of the Player_Base_<Class> StatInitializerLists hold,
// read from CacheDB.dbc by the Python prototype of the format survey (usmap.py and
// dbc.py in the survey's scratchpad: c1/statcurves.json and c1/tab_StatInit.json),
// a decoder independent of package designdb. The tests compare contentc's Go
// decode, the formulas run by package aocexpr and class_stat_base with an
// arithmetic done here from these numbers alone. The curves were checked to be six
// copies of the same 50 keys (Time = the level 1-50), so one list stands for all.

// goldenCurveValues are the value of every attribute curve at levels 1-50.
var goldenCurveValues = [50]float64{
	30, 68, 118, 152, 170, 186, 201, 215, 228, 240,
	251, 262, 272, 282, 292, 301, 310, 319, 327, 335,
	343, 351, 358, 366, 373, 380, 387, 393, 400, 406,
	413, 419, 425, 431, 437, 442, 448, 454, 459, 465,
	470, 475, 481, 486, 491, 496, 501, 506, 511, 516,
}

// goldenCurves are the six curves: Guid and name.
var goldenCurves = []struct {
	guid int64
	name string
}{
	{6064630328054775808, "Player_Base_Attrib_Constitution"},
	{6064630328334811136, "Player_Base_Attrib_Strength"},
	{6064630328334811137, "Player_Base_Attrib_Dexterity"},
	{6064630328334811138, "Player_Base_Attrib_Mentality"},
	{6064630328334811139, "Player_Base_Attrib_Intelligence"},
	{6064630328334811140, "Player_Base_Attrib_Wisdom"},
}

// ~ C:Y:B - CoA ~

// goldenFormulas are the 48 formulas, each "const v = EvalCurve(<curve>, Float(level));
// Round(v * <multiplier>)", in class and attribute order.
var goldenFormulas = []struct {
	class, stat string
	curve       int64
	multiplier  string
}{
	{"Bard", "Strength", 6064630328334811136, "0.8"},
	{"Bard", "Dexterity", 6064630328334811137, "0.8"},
	{"Bard", "Constitution", 6064630328054775808, "1.0"},
	{"Bard", "Intelligence", 6064630328334811139, "1.3"},
	{"Bard", "Wisdom", 6064630328334811140, "1.2"},
	{"Bard", "Mentality", 6064630328334811138, "1.0"},
	{"Cleric", "Strength", 6064630328334811136, "0.8"},
	{"Cleric", "Dexterity", 6064630328334811137, "0.8"},
	{"Cleric", "Constitution", 6064630328054775808, "0.9"},
	{"Cleric", "Intelligence", 6064630328334811139, "1.2"},
	{"Cleric", "Wisdom", 6064630328334811140, "1.3"},
	{"Cleric", "Mentality", 6064630328334811138, "1.1"},
	{"Fighter", "Strength", 6064630328334811136, "1.3"},
	{"Fighter", "Dexterity", 6064630328334811137, "1.0"},
	{"Fighter", "Constitution", 6064630328054775808, "1.2"},
	{"Fighter", "Intelligence", 6064630328334811139, "0.8"},
	{"Fighter", "Wisdom", 6064630328334811140, "0.8"},
	{"Fighter", "Mentality", 6064630328334811138, "1.0"},
	{"Mage", "Strength", 6064630328334811136, "0.8"},
	{"Mage", "Dexterity", 6064630328334811137, "0.8"},
	{"Mage", "Constitution", 6064630328054775808, "0.8"},
	{"Mage", "Intelligence", 6064630328334811139, "1.3"},
	{"Mage", "Wisdom", 6064630328334811140, "1.1"},
	{"Mage", "Mentality", 6064630328334811138, "1.3"},
	{"Ranger", "Strength", 6064630328334811136, "1.1"},
	{"Ranger", "Dexterity", 6064630328334811137, "1.3"},
	{"Ranger", "Constitution", 6064630328054775808, "1.1"},
	{"Ranger", "Intelligence", 6064630328334811139, "0.8"},
	{"Ranger", "Wisdom", 6064630328334811140, "0.8"},
	{"Ranger", "Mentality", 6064630328334811138, "1.0"},
	{"Rogue", "Strength", 6064630328334811136, "1.2"},
	{"Rogue", "Dexterity", 6064630328334811137, "1.2"},
	{"Rogue", "Constitution", 6064630328054775808, "1.1"},
	{"Rogue", "Intelligence", 6064630328334811139, "0.8"},
	{"Rogue", "Wisdom", 6064630328334811140, "0.8"},
	{"Rogue", "Mentality", 6064630328334811138, "1.0"},
	{"Summoner", "Strength", 6064630328334811136, "0.8"},
	{"Summoner", "Dexterity", 6064630328334811137, "0.8"},
	{"Summoner", "Constitution", 6064630328054775808, "1.0"},
	{"Summoner", "Intelligence", 6064630328334811139, "1.2"},
	{"Summoner", "Wisdom", 6064630328334811140, "1.1"},
	{"Summoner", "Mentality", 6064630328334811138, "1.2"},
	{"Tank", "Strength", 6064630328334811136, "1.1"},
	{"Tank", "Dexterity", 6064630328334811137, "1.0"},
	{"Tank", "Constitution", 6064630328054775808, "1.3"},
	{"Tank", "Intelligence", 6064630328334811139, "0.8"},
	{"Tank", "Wisdom", 6064630328334811140, "0.8"},
	{"Tank", "Mentality", 6064630328334811138, "1.1"},
}

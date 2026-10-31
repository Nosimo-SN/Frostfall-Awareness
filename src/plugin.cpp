// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nosimo-SN. Part of Frostfall Awareness.

// Frostfall Awareness: turns Frostfall's player conditions into SkyrimNet narration and rescue memories.
//  - Exposure (Chilly .. Freezing to Death) and frostbite (head / hands / feet / body) are narrated when
//    they start and repeated at a per-state interval while they last. Only debuff states are narrated.
//    Nothing is posted in combat (Frostfall pauses exposure there too), shortly after combat, or while
//    SkyrimNet dialogue is recent, so the speech queue is not crowded.
//  - A Frostfall rescue (max exposure in rescue mode) is confirmed by Frostfall's own rescue message box,
//    then one LLM call writes first-person memories for the rescuers and a hazy one for the player.
// No Papyrus. A worker thread paces the poll and makes SkyrimNet calls; all game reads happen in tasks
// on the game thread, or in the UI event sink.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "SkyrimNet/PublicAPI.h"

namespace
{
	constexpr auto kFrostfallPlugin = "Frostfall.esp"sv;
	constexpr auto kUpdatePlugin = "Update.esm"sv;
	constexpr auto kANDPlugin = "Advanced Nudity Detection.esp"sv;
	constexpr auto kConfigName = "Frostfall_Awareness";
	constexpr auto kRescuePrompt = "frostfall_rescue_memory";
	constexpr auto kPollInterval = 1s;
	constexpr int  kReloadEveryPolls = 10;  // settings reload every ~10 s
	constexpr auto kRescueWindow = 180s;    // a rescue message must follow the collapse within this
	constexpr float kCompanionRange = 4000.0f;
	constexpr auto  kRestartGrace = 120s;  // a state back within this of ending continues, it does not start again

	using Clock = std::chrono::steady_clock;

	// --- narrated states -------------------------------------------------------------------------------

	enum class Kind
	{
		kExposure,
		kFrostbite
	};

	struct StateDef
	{
		Kind        kind;
		int         level;           // exposure level, or unused for frostbite
		const char* keywordEditorID; // frostbite keyword on the magic effect
		RE::FormID  keywordLocalID;  // regular-build fallback
		const char* key;             // settings key
		int         defaultRepeat;   // seconds, 0 = only when it starts
		const char* defaultText;
	};

	// Priority order: the first eligible state is the one narrated on a poll.
	constexpr std::array kStates{
		StateDef{ Kind::kExposure, 5, nullptr, 0, "freezing_to_death", 60, "{name} stumbles through the cold, lips blue, barely able to move." },
		StateDef{ Kind::kFrostbite, 0, "_Frost_FrostbiteHeadKW", 0x069156, "frostbite_head", 60, "{name}'s face and ears are raw and white with frostbite." },
		StateDef{ Kind::kExposure, 4, nullptr, 0, "freezing", 120, "{name} is shaking uncontrollably, limbs stiff and slow with cold." },
		StateDef{ Kind::kFrostbite, 0, "_Frost_FrostbiteBodyKW", 0x069153, "frostbite_body", 120, "Frostbite has bitten deep into {name}'s body." },
		StateDef{ Kind::kFrostbite, 0, "_Frost_FrostbiteHandsKW", 0x069154, "frostbite_hands", 240, "{name}'s fingers are numb and swollen with frostbite." },
		StateDef{ Kind::kFrostbite, 0, "_Frost_FrostbiteFeetKW", 0x069155, "frostbite_feet", 240, "{name} walks stiffly on frostbitten feet." },
		StateDef{ Kind::kExposure, 3, nullptr, 0, "very_cold", 240, "{name} is shivering hard, fingers clumsy with cold." },
		StateDef{ Kind::kExposure, 2, nullptr, 0, "chilly", 480, "{name} shivers as the cold begins to sap {their} strength." },
	};
	constexpr std::size_t kStateCount = kStates.size();

	// --- rescue messages -------------------------------------------------------------------------------

	enum class RescueKind
	{
		kCompanion,  // F_*: a follower carried, dragged or camped the player to safety
		kAnimal,     // D_*: an animal companion
		kAlone,      // A_*: a hunter or passerby found the player
		kFailsafe,   // "a distant tavern"
		kUnknown     // confirmed by Frostfall's rescue signature, message not read
	};

	struct RescueMessageDef
	{
		const char* editorID;
		RE::FormID  localID;  // regular-build fallback
		RescueKind  kind;
	};

	constexpr std::array kRescueMessages{
		RescueMessageDef{ "_Frost_RescueMsg_F_1", 0x062FF2, RescueKind::kCompanion },
		RescueMessageDef{ "_Frost_RescueMsg_F_2", 0x062FF3, RescueKind::kCompanion },
		RescueMessageDef{ "_Frost_RescueMsg_F_3", 0x062FF4, RescueKind::kCompanion },
		RescueMessageDef{ "_Frost_RescueMsg_F_4", 0x062FF5, RescueKind::kCompanion },
		RescueMessageDef{ "_Frost_RescueMsg_F_5", 0x062FF6, RescueKind::kCompanion },
		RescueMessageDef{ "_Frost_RescueMsg_D_1", 0x062FF7, RescueKind::kAnimal },
		RescueMessageDef{ "_Frost_RescueMsg_D_2", 0x062FF8, RescueKind::kAnimal },
		RescueMessageDef{ "_Frost_RescueMsg_D_3", 0x062FF9, RescueKind::kAnimal },
		RescueMessageDef{ "_Frost_RescueMsg_D_4", 0x062FFA, RescueKind::kAnimal },
		RescueMessageDef{ "_Frost_RescueMsg_A_1", 0x062FF0, RescueKind::kAlone },
		RescueMessageDef{ "_Frost_RescueMsg_A_2", 0x062FF1, RescueKind::kAlone },
		RescueMessageDef{ "_Frost_RescueMsg_Failsafe", 0x062FFC, RescueKind::kFailsafe },
	};

	const char* RescueKindName(RescueKind a_kind)
	{
		switch (a_kind) {
		case RescueKind::kCompanion:
			return "companion";
		case RescueKind::kAnimal:
			return "animal_companion";
		case RescueKind::kAlone:
			return "stranger";
		case RescueKind::kFailsafe:
			return "unknown_rescuer";
		default:
			return "unknown";
		}
	}

	// --- settings --------------------------------------------------------------------------------------

	// Defaults mirror settings/Frostfall_Awareness.yaml; used until (or if never) SkyrimNet answers.
	struct Settings
	{
		bool                                 enabled{ true };
		bool                                 forceReaction{ true };
		bool                                 playerThought{ true };
		int                                  minGap{ 60 };
		int                                  dialogueQuiet{ 15 };
		int                                  combatGrace{ 10 };
		std::array<bool, kStateCount>        narrate{};
		std::array<int, kStateCount>         repeat{};
		std::array<std::string, kStateCount> texts{};
		bool                                 rescueMemories{ true };
		bool                                 playerMemory{ true };
		bool                                 rescueEvent{ true };
		bool                                 wakeLine{ true };
		bool                                 arrivalLine{ true };
		int                                  rescueQuiet{ 180 };
		bool                                 purgeDialogue{ true };
		bool                                 huddleEnabled{ true };
		bool                                 huddleHug{ true };
		bool                                 warmOthers{ true };
		bool                                 huddleGlow{ true };
		bool                                 npcFrost{ true };
		int                                  coldShift{ 0 };  // NPC cold, in whole steps: -2 hardier .. +2 feel it sooner
		bool                                 vampiresImmune{ true };
		bool                                 undeadImmune{ true };
		bool                                 heatCopies{ true };  // read at startup only
		bool                                 sceneWarmth{ true };
		float                                sceneRate{ 0.1f };
		bool                                 logAll{ false };

		Settings()
		{
			narrate.fill(true);
			for (std::size_t i = 0; i < kStateCount; ++i) {
				repeat[i] = kStates[i].defaultRepeat;
				texts[i] = kStates[i].defaultText;
			}
		}
	};

	std::mutex g_settingsLock;
	Settings   g_settings;

	std::string ConfigValue(const std::string& a_path, const std::string& a_default)
	{
		if (!PublicGetPluginConfigValue) {
			return a_default;
		}
		return PublicGetPluginConfigValue(kConfigName, a_path.c_str(), a_default.c_str());
	}

	bool ConfigBool(const std::string& a_path, bool a_default)
	{
		auto value = ConfigValue(a_path, a_default ? "true" : "false");
		std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return value == "true" || value == "1" || value == "yes" || value == "on";
	}

	// Seconds fields are plain text boxes while the defaults are tuned; anything unparsable keeps the default.
	int ConfigSeconds(const std::string& a_path, int a_default, int a_max = 86400)
	{
		const auto value = ConfigValue(a_path, std::to_string(a_default));
		double     parsed = a_default;
		try {
			parsed = std::stod(value);
		} catch (...) {
			parsed = a_default;
		}
		return std::clamp(static_cast<int>(parsed), 0, a_max);
	}

	float ConfigFloat(const std::string& a_path, float a_default, float a_min, float a_max)
	{
		const auto value = ConfigValue(a_path, std::to_string(a_default));
		double     parsed = a_default;
		try {
			parsed = std::stod(value);
		} catch (...) {
			parsed = a_default;
		}
		return std::clamp(static_cast<float>(parsed), a_min, a_max);
	}

	void ReloadSettings()
	{
		Settings s;
		if (PublicGetPluginConfigValue) {
			s.enabled = ConfigBool("general.enabled", s.enabled);
			s.forceReaction = ConfigBool("general.force_reaction", s.forceReaction);
			s.playerThought = ConfigBool("general.player_thought", s.playerThought);
			s.minGap = ConfigSeconds("general.min_gap", s.minGap);
			s.dialogueQuiet = ConfigSeconds("general.dialogue_quiet", s.dialogueQuiet);
			s.combatGrace = ConfigSeconds("general.combat_grace", s.combatGrace);
			for (std::size_t i = 0; i < kStateCount; ++i) {
				const std::string key = kStates[i].key;
				s.narrate[i] = ConfigBool("narrate." + key, true);
				s.repeat[i] = ConfigSeconds("repeat." + key, kStates[i].defaultRepeat);
				s.texts[i] = ConfigValue("text." + key, kStates[i].defaultText);
				if (s.texts[i].empty()) {
					s.texts[i] = kStates[i].defaultText;
				}
			}
			s.rescueMemories = ConfigBool("rescue.memories", s.rescueMemories);
			s.playerMemory = ConfigBool("rescue.player_memory", s.playerMemory);
			s.rescueEvent = ConfigBool("rescue.event", s.rescueEvent);
			s.wakeLine = ConfigBool("rescue.wake_line", s.wakeLine);
			s.arrivalLine = ConfigBool("rescue.arrival_line", s.arrivalLine);
			s.rescueQuiet = ConfigSeconds("rescue.quiet", s.rescueQuiet);
			s.purgeDialogue = ConfigBool("rescue.purge_dialogue", s.purgeDialogue);
			s.huddleEnabled = ConfigBool("huddle.enabled", s.huddleEnabled);
			s.huddleHug = ConfigBool("huddle.hug", s.huddleHug);
			s.warmOthers = ConfigBool("huddle.npc_enabled", s.warmOthers);
			s.huddleGlow = ConfigBool("huddle.glow", s.huddleGlow);
			s.npcFrost = ConfigBool("frost.npc_shader", s.npcFrost);
			s.coldShift = static_cast<int>(std::lround(ConfigFloat("npc.cold_shift", static_cast<float>(s.coldShift), -2.0f, 2.0f)));
			s.vampiresImmune = ConfigBool("npc.vampires_immune", s.vampiresImmune);
			s.undeadImmune = ConfigBool("npc.undead_immune", s.undeadImmune);
			s.heatCopies = ConfigBool("heat.match_copies", s.heatCopies);
			s.sceneWarmth = ConfigBool("scene.enabled", s.sceneWarmth);
			s.sceneRate = ConfigFloat("scene.rate", s.sceneRate, 0.0f, 3.0f);
			s.logAll = ConfigBool("debug.log_all", s.logAll);
		}
		std::scoped_lock lock(g_settingsLock);
		g_settings = std::move(s);
	}

	Settings SnapshotSettings()
	{
		std::scoped_lock lock(g_settingsLock);
		return g_settings;
	}

	// --- shared state ----------------------------------------------------------------------------------

	struct Forms
	{
		RE::TESGlobal*                          running{ nullptr };        // FrostfallRunning: 2 running
		RE::TESGlobal*                          exposureLevel{ nullptr };  // -1 .. 6
		RE::TESGlobal*                          exposure{ nullptr };       // 0 .. 120
		RE::TESGlobal*                          maxExposureMode{ nullptr };  // 1 nothing, 2 rescue, 3 death
		RE::TESGlobal*                          temperature{ nullptr };    // Weathersense air where the player is
		RE::TESEffectShader*                    frostShader{ nullptr };    // Frostfall's Freezing to Death look
		RE::TESEffectShader*                    frostShaderLower{ nullptr };  // Frostfall's Freezing look
		std::vector<RE::TESFaction*>            andRevealing;              // A.N.D. showing factions, rank 1 = showing
		RE::TESIdleForm*                        hugIdle{ nullptr };        // vanilla paired hug
		RE::TESEffectShader*                    warmGlow{ nullptr };       // vanilla resist-fire potion glow
		std::vector<RE::TESFaction*>            vampireFactions;           // VampireFaction, DLC1VampireFaction
		RE::TESNPC*                             serana{ nullptr };         // a plain Nord in the records; a vampire until cured
		RE::TESNPC*                             valerica{ nullptr };       // a plain Nord in the records
		RE::TESQuest*                           seranaCure{ nullptr };     // DLC1SeranaCureSelfQuest, stage 100 = cured
		std::array<RE::BGSKeyword*, kStateCount> frostbite{};
		std::vector<std::pair<std::string, RescueKind>> rescueTexts;  // normalised message text
	};
	Forms g_forms;

	std::atomic_bool g_dataReady{ false };
	std::atomic_bool g_inGame{ false };
	std::atomic_bool g_pollQueued{ false };
	bool             g_haveSkyrimNet{ false };

	// SkyrimNet dialogue activity, written from SkyrimNet's thread pool.
	std::atomic<std::int64_t> g_lastDialogueTicks{ 0 };

	// SeverActions (optional): marks the NPCs it leads or keeps on its companion roster.
	using SAGetBusyReasonsFn = std::uint32_t (*)(std::uint32_t);
	SAGetBusyReasonsFn g_saGetBusyReasons{ nullptr };
	constexpr std::uint32_t kSAFollowerLed = 1;
	constexpr std::uint32_t kSACompanion = 1024;

	struct Narration
	{
		std::string       text;
		bool              forceReaction;
		bool                    playerThought;
		int                     attempts{ 0 };
		Clock::time_point       notBefore{};
		RE::FormID              originator{ 0x14 };
		RE::FormID              target{ 0 };
		std::vector<RE::FormID> audience;  // empty = everyone nearby
	};

	struct WakeRequest
	{
		std::string text;
		RE::FormID  rescuer;
	};
	std::mutex                 g_wakeLock;
	std::optional<WakeRequest> g_wakePending;  // written by the memory callback, sent from the game thread
	std::optional<WakeRequest> g_arrivalPending;  // game thread only: the room sees the player brought in

	struct Companion
	{
		RE::FormID  formID;
		std::string name;
		std::string race;
		bool        creature;
	};

	struct RescueJob
	{
		RescueKind             kind;
		std::string            scenario;  // Frostfall's own message text
		std::string            fallLocation;
		std::string            arrivalLocation;
		std::string            weather;
		bool                   inWater;
		std::string            playerName;
		bool                   playerFemale{ false };
		std::vector<Companion> rescuers;
		bool                   memorySpeechOnly{ false };  // SkyrimNet: memories only for actors capable of speech
		bool                   actorSpeechOnly{ false };   // SkyrimNet: only actors capable of speech take part
		Settings               settings;
	};

	std::mutex             g_queueLock;
	std::vector<Narration> g_narrations;
	std::vector<RescueJob> g_rescues;

	// Game thread only.
	struct StateTrack
	{
		bool                             active{ false };
		bool                             pendingStart{ false };
		std::optional<Clock::time_point> lastNarrated;
		Clock::time_point                activeSince{};
		std::optional<Clock::time_point> endedAt;
	};
	std::array<StateTrack, kStateCount> g_track{};
	std::optional<Clock::time_point>    g_lastAnyNarration;
	std::optional<Clock::time_point>    g_lastCombat;
	std::optional<Clock::time_point>    g_rescueQuietUntil;    // no cold narration until then, after a rescue
	std::optional<Clock::time_point>    g_rescueBoxClosedAt;   // Frostfall's rescue message was dismissed
	int                                 g_lastLevel{ -1 };  // exposure level (capped at 5) on the previous poll
	bool                                g_primed{ false };

	// Rescue tracking (game thread / UI thread, which are the same in Skyrim's main loop).
	struct ColdSnapshot
	{
		Clock::time_point      at{};
		float                  gameHours{ 0.0f };
		std::string            location;
		std::string            weather;
		bool                   inWater{ false };
		std::vector<Companion> companions;
	};
	std::optional<ColdSnapshot> g_coldSnapshot;  // refreshed while exposure is Freezing to Death or worse
	std::optional<ColdSnapshot> g_armed;         // taken when exposure hit max in rescue mode
	std::optional<Clock::time_point> g_lastRescue;  // a rescue already handled at this time

	bool RescueHandled()
	{
		return g_lastRescue && Clock::now() - *g_lastRescue < kRescueWindow;
	}

	void InitializeLog()
	{
		auto path = logs::log_directory();
		if (!path) {
			SKSE::stl::report_and_fail("Failed to find standard logging directory"sv);
		}
		*path /= "Frostfall_Awareness.log"sv;
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S:%e] [%l] %v"s);
	}

	// --- helpers ---------------------------------------------------------------------------------------

	void ReplaceAll(std::string& a_text, std::string_view a_token, std::string_view a_value)
	{
		for (auto pos = a_text.find(a_token); pos != std::string::npos; pos = a_text.find(a_token, pos + a_value.size())) {
			a_text.replace(pos, a_token.size(), a_value);
		}
	}

	// Lower case, letters and digits only, single spaces: message box text may differ in whitespace.
	std::string Normalise(std::string_view a_text)
	{
		std::string out;
		bool        space = false;
		for (const unsigned char c : a_text) {
			if (std::isalnum(c)) {
				if (space && !out.empty()) {
					out.push_back(' ');
				}
				space = false;
				out.push_back(static_cast<char>(std::tolower(c)));
			} else {
				space = true;
			}
		}
		return out;
	}

	bool IsChildActor(RE::Actor* a_actor)
	{
		if (a_actor->IsChild()) {
			return true;
		}
		const auto race = a_actor->GetRace();
		const auto raceID = race ? race->GetFormEditorID() : nullptr;
		if (!raceID) {
			return false;
		}
		std::string editorID = raceID;
		std::ranges::transform(editorID, editorID.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return editorID.find("child") != std::string::npos;
	}

	float GlobalValue(RE::TESGlobal* a_global, float a_default)
	{
		return a_global ? a_global->value : a_default;
	}

	float GameHours()
	{
		const auto calendar = RE::Calendar::GetSingleton();
		return calendar ? calendar->GetHoursPassed() : 0.0f;
	}

	// The nearest map marker in the player's worldspace, for places the game only knows as the hold.
	std::string NearestMarkerName(RE::PlayerCharacter* a_player, float a_maxDistance)
	{
		const auto  world = a_player->GetWorldspace();
		const auto  pos = a_player->GetPosition();
		float       best = a_maxDistance;
		std::string name;
		for (auto& handle : a_player->GetPlayerRuntimeData().currentMapMarkers) {
			const auto ref = handle.get();
			if (!ref || ref->GetWorldspace() != world) {
				continue;
			}
			const auto extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
			const auto label = extra && extra->mapData ? extra->mapData->locationName.GetFullName() : nullptr;
			if (!label || !*label) {
				continue;
			}
			if (const float d = pos.GetDistance(ref->GetPosition()); d < best) {
				best = d;
				name = label;
			}
		}
		return name;
	}

	// SkyrimNet's "only actors capable of speech" switches aren't in its API; read them from its config files.
	bool SkyrimNetSwitch(const char* a_file, std::string_view a_key)
	{
		std::ifstream in(std::string("Data/SKSE/Plugins/SkyrimNet/config/") + a_file);
		for (std::string line; std::getline(in, line);) {
			if (line.starts_with(a_key) && line.size() > a_key.size() && line[a_key.size()] == ':') {
				return line.find("true", a_key.size()) != std::string::npos;
			}
		}
		return false;
	}

	std::string LocationName(RE::PlayerCharacter* a_player)
	{
		std::string name;
		const auto  loc = a_player->GetCurrentLocation();
		// Out in the wilds the current location is only the hold: name the nearest landmark too.
		const auto parentCell = a_player->GetParentCell();
		if ((!parentCell || !parentCell->IsInteriorCell()) && (!loc || loc->HasKeywordString("LocTypeHold"))) {
			if (auto marker = NearestMarkerName(a_player, 16000.0f); !marker.empty()) {
				const auto hold = loc ? loc->GetFullName() : nullptr;
				return hold && *hold ? "near " + marker + ", " + hold : "near " + marker;
			}
		}
		if (loc) {
			if (const auto full = loc->GetFullName(); full && *full) {
				name = full;
			}
			if (const auto parent = loc->parentLoc) {
				if (const auto full = parent->GetFullName(); full && *full && name != full) {
					name = name.empty() ? std::string(full) : name + ", " + full;
				}
			}
		}
		if (name.empty()) {
			if (const auto cell = a_player->GetParentCell()) {
				if (const auto full = cell->GetFullName(); full && *full) {
					name = full;
				}
			}
		}
		if (name.empty()) {
			if (const auto world = a_player->GetWorldspace()) {
				if (const auto full = world->GetFullName(); full && *full) {
					name = "the wilds of "s + full;
				}
			}
		}
		return name.empty() ? "the wilderness"s : name;
	}

	std::string WeatherName()
	{
		const auto sky = RE::Sky::GetSingleton();
		const auto weather = sky ? sky->currentWeather : nullptr;
		if (!weather) {
			return "unknown";
		}
		const auto flags = weather->data.flags;
		if (flags.any(RE::TESWeather::WeatherDataFlag::kSnow)) {
			return "snowing";
		}
		if (flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
			return "raining";
		}
		if (flags.any(RE::TESWeather::WeatherDataFlag::kCloudy)) {
			return "overcast";
		}
		return "clear";
	}

	// Followers who could have carried the player: player teammates, plus anyone SeverActions leads or keeps
	// on its companion roster. Children and the dead never count.
	std::vector<Companion> FindCompanions(RE::PlayerCharacter* a_player)
	{
		std::vector<Companion> out;
		const auto             lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return out;
		}
		for (auto& handle : lists->highActorHandles) {
			const auto ptr = handle.get();
			const auto actor = ptr.get();
			if (!actor || actor->IsDead() || IsChildActor(actor)) {
				continue;
			}
			bool follower = actor->IsPlayerTeammate();
			if (!follower && g_saGetBusyReasons) {
				follower = (g_saGetBusyReasons(actor->GetFormID()) & (kSAFollowerLed | kSACompanion)) != 0;
			}
			if (!follower || actor->GetDistance(a_player) > kCompanionRange) {
				continue;
			}
			const auto name = actor->GetDisplayFullName();
			const auto race = actor->GetRace();
			const auto raceName = race ? race->GetFullName() : nullptr;
			out.push_back({ actor->GetFormID(),
				name && *name ? name : "a companion",
				raceName && *raceName ? raceName : "",
				!(race && race->HasKeywordString("ActorTypeNPC")) });
		}
		return out;
	}

	ColdSnapshot TakeSnapshot(RE::PlayerCharacter* a_player)
	{
		ColdSnapshot snap;
		snap.at = Clock::now();
		snap.gameHours = GameHours();
		snap.location = LocationName(a_player);
		snap.weather = WeatherName();
		snap.inWater = a_player->AsActorState() && a_player->AsActorState()->IsSwimming();
		snap.companions = FindCompanions(a_player);
		return snap;
	}

	std::string ComposeText(RE::PlayerCharacter* a_player, const std::string& a_template)
	{
		const auto base = a_player->GetActorBase();
		const bool female = base && base->GetSex() == RE::SEX::kFemale;
		const auto fullName = a_player->GetDisplayFullName();

		std::string text = a_template;
		ReplaceAll(text, "{name}", fullName && *fullName ? fullName : "The traveller");
		ReplaceAll(text, "{their}", female ? "her" : "his");
		ReplaceAll(text, "{they}", female ? "she" : "he");
		ReplaceAll(text, "{them}", female ? "her" : "him");
		return text;
	}

	// SkyrimNetApi.PurgeDialogue(false): clears every queued NPC line and stops the one playing.
	void PurgeDialogue()
	{
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
		std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments(false) };
		if (vm->DispatchStaticCall("SkyrimNetApi", "PurgeDialogue", args.get(), result)) {
			logs::info("Rescue: SkyrimNet dialogue purged");
		} else {
			logs::warn("SkyrimNetApi.PurgeDialogue could not be called");
		}
	}

	// --- narration (game thread) -----------------------------------------------------------------------

	bool IsStateActive(std::size_t a_index, RE::PlayerCharacter* a_player, int a_level)
	{
		const auto& def = kStates[a_index];
		if (def.kind == Kind::kExposure) {
			// Only the current level is active; level 6 (max exposure) narrates as Freezing to Death.
			return (std::min)(a_level, 5) == def.level;
		}
		const auto keyword = g_forms.frostbite[a_index];
		return keyword && a_player->HasMagicEffectWithKeyword(keyword);
	}

	void UpdateNarration(RE::PlayerCharacter* a_player, const Settings& a_settings)
	{
		const auto now = Clock::now();
		const int  level = static_cast<int>(std::lround(GlobalValue(g_forms.exposureLevel, 0.0f)));
		const int  capped = (std::min)(level, 5);
		const bool coolingDown = capped > g_lastLevel;
		const bool rescueQuiet = g_rescueQuietUntil && now < *g_rescueQuietUntil;
		g_lastLevel = capped;

		for (std::size_t i = 0; i < kStateCount; ++i) {
			auto&      track = g_track[i];
			const bool active = IsStateActive(i, a_player, level);
			if (active && !track.active) {
				// Bouncing across a level boundary (a fire spell, a gust) continues the same spell of cold: no new
				// start narration, and the repeat timer carries on.
				if (track.endedAt && now - *track.endedAt < kRestartGrace) {
					track.active = true;
					track.endedAt.reset();
					if (a_settings.logAll) {
						logs::info("{} resumed", kStates[i].key);
					}
					continue;
				}
				track.active = true;
				track.endedAt.reset();
				track.activeSince = now;
				track.lastNarrated.reset();
				// Starts are only news when getting worse: a level reached by warming up is not announced, and
				// states already present after a load, or frostbite from a rescue, are recorded quietly.
				const bool frostbite = kStates[i].kind == Kind::kFrostbite;
				track.pendingStart = g_primed && !rescueQuiet && (frostbite || coolingDown);
				if (a_settings.logAll) {
					logs::info("{} started{}", kStates[i].key, track.pendingStart ? "" : " (quietly)");
				}
			} else if (!active && track.active) {
				// A start narration still waiting stays pending, so a short dip does not lose it; only active
				// states are ever narrated, and a fresh start recomputes it.
				track.active = false;
				track.endedAt = now;
				if (a_settings.logAll) {
					logs::info("{} ended", kStates[i].key);
				}
			}
		}

		if (!g_haveSkyrimNet || !a_settings.enabled || !g_primed || rescueQuiet) {
			return;
		}

		// Gates: combat (and a grace after it), recent SkyrimNet dialogue, and the minimum gap.
		if (a_player->IsInCombat()) {
			g_lastCombat = now;
			return;
		}
		if (g_lastCombat && now - *g_lastCombat < std::chrono::seconds(a_settings.combatGrace)) {
			return;
		}
		if (const auto ticks = g_lastDialogueTicks.load(); ticks != 0) {
			const Clock::time_point last{ Clock::duration(ticks) };
			if (now - last < std::chrono::seconds(a_settings.dialogueQuiet)) {
				return;
			}
		}
		if (g_lastAnyNarration && now - *g_lastAnyNarration < std::chrono::seconds(a_settings.minGap)) {
			return;
		}
		if (const auto ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
			return;
		}

		for (std::size_t i = 0; i < kStateCount; ++i) {
			auto& track = g_track[i];
			if (!track.active || !a_settings.narrate[i]) {
				continue;
			}
			bool due = track.pendingStart;
			if (!due && a_settings.repeat[i] > 0) {
				const auto since = track.lastNarrated ? *track.lastNarrated : track.activeSince;
				due = now - since >= std::chrono::seconds(a_settings.repeat[i]);
			}
			if (!due) {
				continue;
			}
			track.pendingStart = false;
			track.lastNarrated = now;
			g_lastAnyNarration = now;
			auto text = ComposeText(a_player, a_settings.texts[i]);
			logs::info("narrate {}: {}", kStates[i].key, text);
			std::scoped_lock lock(g_queueLock);
			g_narrations.push_back({ std::move(text), a_settings.forceReaction, a_settings.playerThought });
			return;  // one narration per poll; the minimum gap spaces the rest
		}
	}

	// --- rescue (game thread) --------------------------------------------------------------------------

	void QueueRescue(RescueKind a_kind, std::string a_scenario, RE::PlayerCharacter* a_player, const Settings& a_settings)
	{
		const auto now = Clock::now();
		g_lastRescue = now;
		g_rescueBoxClosedAt.reset();
		// Waking up starts a quiet spell: nothing narrated for a while, starts already suffered on the
		// mountain are not announced again, and repeats count from now.
		if (a_settings.rescueQuiet > 0) {
			g_rescueQuietUntil = now + std::chrono::seconds(a_settings.rescueQuiet);
		}
		for (auto& track : g_track) {
			track.pendingStart = false;
			if (track.active) {
				track.lastNarrated = now;
			}
		}
		// Lines queued on the mountain would play on in the inn; the player is unconscious, so stop them.
		if (a_settings.purgeDialogue) {
			PurgeDialogue();
		}

		const auto& fall = g_armed ? *g_armed : *g_coldSnapshot;
		// Frostfall's failsafe ("a distant tavern", used where it has no rescue spots, e.g. when its Solstheim
		// check misses) still moves followers within 2000 units along with the player, so whoever woke beside
		// the player carried them there.
		std::vector<Companion> present = fall.companions;
		if (a_kind == RescueKind::kFailsafe) {
			std::erase_if(present, [&](const Companion& c) {
				const auto actor = RE::TESForm::LookupByID<RE::Actor>(c.formID);
				return !actor || actor->GetDistance(a_player) > 2000.0f;
			});
		}
		if (a_kind == RescueKind::kUnknown || a_kind == RescueKind::kFailsafe) {
			// Frostfall picks the rescue the same way: followers first, then an animal companion.
			const bool people = std::ranges::any_of(present, [](const Companion& c) { return !c.creature; });
			const bool animal = std::ranges::any_of(present, [](const Companion& c) { return c.creature; });
			a_kind = people ? RescueKind::kCompanion : animal ? RescueKind::kAnimal : a_kind;
		}
		RescueJob   job;
		job.kind = a_kind;
		job.scenario = std::move(a_scenario);
		job.fallLocation = fall.location;
		job.arrivalLocation = LocationName(a_player);
		job.weather = fall.weather;
		job.inWater = fall.inWater;
		const auto playerName = a_player->GetDisplayFullName();
		job.playerName = playerName && *playerName ? playerName : "the traveller";
		const auto base = a_player->GetActorBase();
		job.playerFemale = base && base->GetSex() == RE::SEX::kFemale;
		// A companion rescue is everyone who came along, recruited creatures included; an animal rescue is the
		// creatures alone (Frostfall chose it because no follower was there). People are listed first.
		for (const bool creature : { false, true }) {
			for (const auto& c : present) {
				if (c.creature == creature && (a_kind == RescueKind::kCompanion || (a_kind == RescueKind::kAnimal && creature))) {
					job.rescuers.push_back(c);
				}
			}
		}
		job.memorySpeechOnly = SkyrimNetSwitch("MemoryFilter.yaml", "AllowOnlyActorsCapableOfSpeech");
		job.actorSpeechOnly = SkyrimNetSwitch("ActorFilter.yaml", "AllowOnlyActorsCapableOfSpeech");
		job.settings = a_settings;
		logs::info("Frostfall rescue confirmed ({}): {} -> {}, {} rescuer(s){}", RescueKindName(a_kind), job.fallLocation,
			job.arrivalLocation, job.rescuers.size(),
			job.memorySpeechOnly || job.actorSpeechOnly ? " (SkyrimNet: speech-capable actors only for"s + (job.memorySpeechOnly ? " memories"s : ""s) +
			                                                  (job.actorSpeechOnly ? " dialogue"s : ""s) + ")" :
			                                              ""s);

		// The room sees the player brought in. Not when the rescue stayed where the player fell (a companion's
		// campfire), and only for a companion or an animal, whose name is known.
		g_arrivalPending.reset();
		if (a_settings.arrivalLine && !job.rescuers.empty() && job.fallLocation != job.arrivalLocation) {
			const auto& r = job.rescuers.front();
			const auto  text = r.creature ? r.name + " drags a half-frozen " + job.playerName + " in, having saved " + (job.playerFemale ? "her"s : "him"s) + " from the cold." :
			                                r.name + " carries a half-frozen " + job.playerName + " in, having rescued " + (job.playerFemale ? "her"s : "him"s) + " from the cold.";
			g_arrivalPending = WakeRequest{ text, r.formID };
		}
		if (!a_settings.rescueMemories || !g_haveSkyrimNet) {
			return;
		}
		std::scoped_lock lock(g_queueLock);
		g_rescues.push_back(std::move(job));
	}

	// Frostfall's rescue: exposure reaches max (level 6) with Max Exposure set to rescue (2). It knocks the
	// player out, advances time two hours, sets exposure to 45, moves the party and shows one of its rescue
	// messages. The message is the proof; another mod that took the collapse over shows none of them.
	void UpdateRescue(RE::PlayerCharacter* a_player, const Settings& a_settings)
	{
		const auto now = Clock::now();
		const int  level = static_cast<int>(std::lround(GlobalValue(g_forms.exposureLevel, 0.0f)));
		const int  mode = static_cast<int>(std::lround(GlobalValue(g_forms.maxExposureMode, 0.0f)));

		if (level >= 5) {
			g_coldSnapshot = TakeSnapshot(a_player);
		}
		if (level >= 6 && mode == 2 && !g_armed) {
			g_armed = g_coldSnapshot ? g_coldSnapshot : TakeSnapshot(a_player);
			logs::info("Max exposure in rescue mode at {}: watching for Frostfall's rescue", g_armed->location);
		}
		if (!g_armed) {
			return;
		}
		if (now - g_armed->at > kRescueWindow) {
			if (!RescueHandled()) {
				logs::info("No Frostfall rescue followed the collapse: another mod may have handled it; no memories written");
			}
			g_armed.reset();
			return;
		}
		// Message not readable (or not shown): fall back to Frostfall's signature, two game hours later and
		// exposure reset to about 45. Only after the message has had time to appear.
		if (!RescueHandled() && now - g_armed->at > 30s) {
			const float hours = GameHours() - g_armed->gameHours;
			const float exposure = GlobalValue(g_forms.exposure, 0.0f);
			if (hours >= 1.5f && exposure > 0.0f && exposure <= 50.0f) {
				QueueRescue(RescueKind::kUnknown, "", a_player, a_settings);
			}
		}
	}

	void OnMessageBox(const std::string& a_text)
	{
		if (!g_dataReady || g_forms.rescueTexts.empty()) {
			return;
		}
		const auto normalised = Normalise(a_text);
		for (const auto& [text, kind] : g_forms.rescueTexts) {
			if (normalised.find(text) == std::string::npos) {
				continue;
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto settings = SnapshotSettings();
			if (!player) {
				return;
			}
			if (RescueHandled()) {
				return;
			}
			if (!g_armed && !(g_coldSnapshot && Clock::now() - g_coldSnapshot->at < kRescueWindow)) {
				logs::info("Frostfall rescue message seen without a recent collapse; no memories written");
				return;
			}
			QueueRescue(kind, a_text, player, settings);
			return;
		}
	}

	class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* GetSingleton()
		{
			static MenuSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->menuName == RE::MessageBoxMenu::MENU_NAME) {
				if (a_event->opening) {
					if (const auto data = RE::MessageBoxMenu::GetCurrentMessageBoxData(); data && data->bodyText.c_str()) {
						OnMessageBox(data->bodyText.c_str());
					}
				} else if (RescueHandled() && !g_rescueBoxClosedAt) {
					g_rescueBoxClosedAt = Clock::now();
					// Second purge as the player comes to: lines generated during the blackout (another mod's
					// narration, a reply to it) would otherwise play over the wake-up.
					if (SnapshotSettings().purgeDialogue) {
						PurgeDialogue();
					}
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// --- scene warmth (SexLab / OStim) ---------------------------------------------------------------------
	// While the player is in a SexLab or OStim scene (cuddles included), shared body heat warms them: every
	// 10 s Frostfall's own FrostUtil.ModPlayerExposure(-rate) runs, with no floor. SexLab: SexLabAnimatingFaction
	// rank 1 (2 = walking into position; it leaves actors at rank 0 after the scene, so membership alone is not
	// enough). OStim: its own OActor.IsInOStim(player), asked every 2 s through the Papyrus VM and cached.

	constexpr auto kSceneTick = 10s;

	RE::TESFaction*                  g_slAnimating{ nullptr };  // SexLab.esm SexLabAnimatingFaction
	bool                             g_haveOStim{ false };
	std::atomic_bool                 g_inOStim{ false };       // written by the Papyrus callback
	std::optional<Clock::time_point> g_lastOStimQuery;
	bool                             g_inScene{ false };
	const char*                      g_sceneFramework{ "a SexLab" };  // which one started the scene, for the exit log
	Clock::time_point                g_lastSceneTick{};

	class OStimResult final : public RE::BSScript::IStackCallbackFunctor
	{
	public:
		void operator()(RE::BSScript::Variable a_result) override { g_inOStim = a_result.IsBool() && a_result.GetBool(); }
		void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
	};

	void QueryOStim(RE::PlayerCharacter* a_player)
	{
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{ new OStimResult() };
		std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments(static_cast<RE::Actor*>(a_player)) };
		vm->DispatchStaticCall("OActor", "IsInOStim", args.get(), result);
	}

	void WarmPlayerBy(float a_amount)
	{
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
		std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments(-a_amount, -1.0f) };
		if (!vm->DispatchStaticCall("FrostUtil", "ModPlayerExposure", args.get(), result)) {
			logs::warn("FrostUtil.ModPlayerExposure could not be called");
		}
	}

	void UpdateSceneWarmth(RE::PlayerCharacter* a_player, const Settings& a_settings)
	{
		const auto now = Clock::now();
		if (g_haveOStim && (!g_lastOStimQuery || now - *g_lastOStimQuery >= 2s)) {
			g_lastOStimQuery = now;
			QueryOStim(a_player);
		}
		const bool sexlab = g_slAnimating && a_player->GetFactionRank(g_slAnimating, true) == 1;
		const bool inScene = sexlab || g_inOStim;
		if (inScene != g_inScene) {
			g_inScene = inScene;
			g_lastSceneTick = now;  // the first warming comes one tick into the scene
			if (inScene) {
				g_sceneFramework = sexlab ? "a SexLab" : "an OStim";
			}
			logs::info("Player {} {} scene", inScene ? "entered" : "left", g_sceneFramework);
		}
		if (!inScene || !a_settings.sceneWarmth || a_settings.sceneRate <= 0.0f || now - g_lastSceneTick < kSceneTick) {
			return;
		}
		g_lastSceneTick = now;
		WarmPlayerBy(a_settings.sceneRate);
		if (a_settings.logAll) {
			logs::info("Scene warmth: exposure -{:.1f}", a_settings.sceneRate);
		}
	}

	// --- share body heat (eligibility decorator for the YAML action) ------------------------------------
	// actions/FrostfallShareBodyHeat.yaml lets a nearby friendly person huddle with the player; SkyrimNet runs it
	// on Frostfall's own exposure quest (ModExposure -10, never below Chilly), so only fire or shelter truly warms.
	// This decorator is its eligibility rule: "" when the speaker can, else why not. SkyrimNet calls it on its
	// own threads, so it reads only what the game-thread poll caches here.

	constexpr auto  kHuddleDecorator = "frostfall_share_heat_block";
	constexpr float kHuddleRange = 700.0f;

	std::atomic_int                             g_huddleLevel{ -1 };
	std::atomic_bool                            g_huddleCombat{ false };
	std::mutex                                  g_huddleLock;
	std::unordered_map<RE::FormID, std::string> g_huddleCandidates;  // "" = can huddle, else why not
	std::unordered_map<RE::FormID, std::string> g_huddleLogged;      // last answer logged per actor

	void UpdateHuddleCandidates(RE::PlayerCharacter* a_player)
	{
		g_huddleLevel = static_cast<int>(std::lround(GlobalValue(g_forms.exposureLevel, 0.0f)));
		g_huddleCombat = a_player->IsInCombat();
		std::unordered_map<RE::FormID, std::string> candidates;
		if (const auto lists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor) {
					continue;
				}
				const auto  race = actor->GetRace();
				const float distance = actor->GetDistance(a_player);
				std::string why;
				if (actor->IsDead()) {
					why = "dead";
				} else if (IsChildActor(actor)) {
					why = "a child";
				} else if (actor->IsHostileToActor(a_player)) {
					why = "hostile";
				} else if (!race || !race->HasKeywordString("ActorTypeNPC")) {
					why = "not a person";
				} else if (distance > kHuddleRange) {
					why = fmt::format("too far ({:.0f} units)", distance);
				}
				candidates.emplace(actor->GetFormID(), std::move(why));
			}
		}
		std::scoped_lock lock(g_huddleLock);
		g_huddleCandidates = std::move(candidates);
	}

	// "" = eligible, else the reason it is not.
	const char* ColdImmunity(RE::Actor* a_actor, const Settings& a_settings);  // defined with the NPC cold estimate

	std::string HuddleBlocker(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return "no actor";
		}
		if (!g_inGame) {
			return "not in game";
		}
		const auto settings = SnapshotSettings();
		if (!settings.huddleEnabled) {
			return "action switched off";
		}
		// A vampire or undead who feels no cold has no body heat to share either.
		if (const auto immune = ColdImmunity(a_actor, settings)) {
			return fmt::format("{}, no body heat to share", immune);
		}
		if (g_huddleLevel < 2) {
			return fmt::format("player not cold enough (exposure level {})", g_huddleLevel.load());
		}
		if (g_huddleCombat) {
			return "player in combat";
		}
		std::scoped_lock lock(g_huddleLock);
		const auto       candidate = g_huddleCandidates.find(a_actor->GetFormID());
		if (candidate == g_huddleCandidates.end()) {
			return "not near the player (not in the loaded high-process list)";
		}
		return candidate->second;
	}

	std::string HuddleDecorator(RE::Actor* a_actor)
	{
		auto why = HuddleBlocker(a_actor);
		if (a_actor) {
			// One line whenever an actor's answer changes, so the log says why without flooding.
			std::scoped_lock lock(g_huddleLock);
			auto&            last = g_huddleLogged[a_actor->GetFormID()];
			const auto       now = why.empty() ? "yes"s : why;
			if (last != now) {
				last = now;
				const auto name = a_actor->GetName();
				logs::info("ShareBodyHeat {} ({:08X}): {}", name && *name ? name : "?", a_actor->GetFormID(), why.empty() ? "eligible" : "no, " + why);
			}
		}
		return why;
	}

	void RegisterHuddleDecorator()
	{
		if (!PublicRegisterDecorator) {
			logs::warn("SkyrimNet decorators unavailable: the share-body-heat action is never eligible");
			return;
		}
		const bool ok = PublicRegisterDecorator(kHuddleDecorator,
			"Frostfall Awareness: empty when this actor can share body heat with the player (player Chilly or worse, not in combat, "
			"actor a friendly adult close by), otherwise the reason they can't.",
			HuddleDecorator);
		logs::info("Share-body-heat decorator {}", ok ? "registered" : "NOT registered");
	}

	// The wake-up line waits until Frostfall's rescue message is dismissed and the player has got up (Frostfall
	// fades in and plays a woozy get-up), and until the room has been quiet briefly. Only the rescuer hears it,
	// so they take the turn; the rescue event already told everyone else.
	void UpdateWake()
	{
		// Arrival first: as soon as Frostfall's message is dismissed (the second purge has just run), so the
		// people in the room react to the player being brought in; the rescuer's own line follows later.
		if (g_arrivalPending) {
			const auto now = Clock::now();
			const bool ready = g_rescueBoxClosedAt || (g_lastRescue && now - *g_lastRescue >= 20s);
			const auto ui = RE::UI::GetSingleton();
			if (ready && !(ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)))) {
				Narration n{ g_arrivalPending->text, true, false };
				n.target = g_arrivalPending->rescuer;
				g_arrivalPending.reset();
				logs::info("Arrival narration sent: {}", n.text);
				std::scoped_lock lock(g_queueLock);
				g_narrations.push_back(std::move(n));
			}
		}

		std::optional<WakeRequest> wake;
		{
			std::scoped_lock lock(g_wakeLock);
			if (!g_wakePending) {
				return;
			}
			const auto now = Clock::now();
			// Without a dismissed message (the fallback confirmation), count from the rescue itself.
			const auto since = g_rescueBoxClosedAt ? *g_rescueBoxClosedAt : g_lastRescue.value_or(now);
			const auto waited = now - since;
			if (waited < (g_rescueBoxClosedAt ? 4s : 20s)) {
				return;
			}
			if (const auto ui = RE::UI::GetSingleton(); ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME))) {
				return;
			}
			const auto ticks = g_lastDialogueTicks.load();
			const bool talking = ticks != 0 && now - Clock::time_point{ Clock::duration(ticks) } < 4s;
			if (talking && waited < 45s) {
				return;
			}
			wake = std::move(g_wakePending);
			g_wakePending.reset();
		}
		Narration n{ wake->text, true, false };
		n.originator = wake->rescuer;
		n.target = 0x14;
		n.audience = { wake->rescuer };
		logs::info("Wake-up narration sent: {}", n.text);
		std::scoped_lock lock(g_queueLock);
		g_narrations.push_back(std::move(n));
	}

	// --- NPC frost shaders ---------------------------------------------------------------------------
	// Frostfall frosts only the player (its Freezing / Freezing to Death effects). NPCs outdoors get the same look
	// from the same estimate as the Cold Shiver animations: the air, revealing clothing (A.N.D.) and 50+ frost
	// resistance decide whether they are cold; then they sit at the player's level, one step colder in revealing
	// clothing, with Freezing (4) the lighter frost and Freezing to Death (5+) the full one. Each shader runs for
	// a short time and is renewed while it still applies, so it fades by itself when the NPC warms up or unloads.

	constexpr float kFrostShaderSeconds = 15.0f;
	constexpr auto  kFrostTick = 3s;

	Clock::time_point g_lastFrostTick{};

	// NPC-to-NPC warming (FrostfallWarmEachOther). Who feels cold and who could warm someone are worked out here
	// on the game thread; the decorator reads them from SkyrimNet's threads. A warmed NPC loses the frost look for
	// a while (game thread only).
	constexpr auto  kWarmedFor = 300s;
	constexpr float kWarmOtherRange = 1000.0f;
	constexpr auto  kWarmOtherDecorator = "frostfall_warm_other_block";

	std::mutex                                        g_warmOtherLock;
	std::unordered_map<RE::FormID, std::string>       g_warmOtherCandidates;  // "" = has someone cold to warm, else why not
	std::unordered_map<RE::FormID, std::string>       g_warmOtherLogged;
	std::unordered_map<RE::FormID, Clock::time_point> g_warmedUntil;

	bool RecentlyWarmed(RE::FormID a_id, Clock::time_point a_now)
	{
		const auto it = g_warmedUntil.find(a_id);
		return it != g_warmedUntil.end() && a_now < it->second;
	}

	bool IsRevealing(RE::Actor* a_actor)
	{
		return std::ranges::any_of(g_forms.andRevealing, [&](RE::TESFaction* a_faction) {
			return a_actor->GetFactionRank(a_faction, false) == 1;
		});
	}

	// The Cold Shiver OAR conditions, NPC branch.
	std::atomic_bool g_seranaCured{ false };  // from the game thread's poll; the decorator reads it elsewhere

	bool IsVampire(RE::Actor* a_actor, const std::function<bool(std::string_view)>& a_hasKeyword)
	{
		if (a_hasKeyword("Vampire")) {
			return true;
		}
		// Serana and Valerica are plain Nords in the records (Dawnguard tracks Serana's cure in a quest), and
		// follower mods often leave the keyword off but keep the vampire factions.
		const auto base = a_actor->GetActorBase();
		if (base && ((base == g_forms.serana && !g_seranaCured) || base == g_forms.valerica)) {
			return true;
		}
		return std::ranges::any_of(g_forms.vampireFactions, [&](RE::TESFaction* a_faction) { return a_actor->IsInFaction(a_faction); });
	}

	class SeranaCuredResult final : public RE::BSScript::IStackCallbackFunctor
	{
	public:
		void operator()(RE::BSScript::Variable a_result) override { g_seranaCured = a_result.IsBool() && a_result.GetBool(); }
		void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}
	};

	// DLC1SeranaCureSelfQuest stage 100 = cured. CommonLib has no stage-done check, so ask Papyrus, every 10 s.
	void QuerySeranaCured()
	{
		static Clock::time_point last{};
		const auto now = Clock::now();
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!g_forms.seranaCure || !vm || now - last < 10s) {
			return;
		}
		last = now;
		const auto policy = vm->GetObjectHandlePolicy();
		const auto handle = policy ? policy->GetHandleForObject(RE::TESQuest::FORMTYPE, g_forms.seranaCure) : 0;
		if (!handle) {
			return;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result{ new SeranaCuredResult() };
		std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments(static_cast<std::int32_t>(100)) };
		vm->DispatchMethodCall(handle, "Quest", "IsStageDone", args.get(), result);
	}

	// Vampires and the undead don't feel the cold (each switchable). The keyword is usually on the race.
	const char* ColdImmunity(RE::Actor* a_actor, const Settings& a_settings)
	{
		const auto race = a_actor->GetRace();
		const std::function<bool(std::string_view)> has = [&](std::string_view a_keyword) {
			return a_actor->HasKeywordString(a_keyword) || (race && race->HasKeywordString(a_keyword));
		};
		if (a_settings.vampiresImmune && IsVampire(a_actor, has)) {
			return "vampire";
		}
		if (a_settings.undeadImmune && has("ActorTypeUndead")) {
			return "undead";
		}
		return nullptr;
	}

	// The Cold Shiver OAR conditions, NPC branch, by air tier (Weathersense: 1 chilly < 6, 2 cold < 1,
	// 3 biting < -4, 4 bone-chilling < -9, 5 frigid < -14). Revealing clothing feels it from tier 2 (tier 1 in
	// rain), anyone from tier 4; 50+ frost resistance holds off revealing to 4 and everyone else to 5. The cold
	// shift setting moves those thresholds by whole tiers (the OAR animations can't read it).
	bool NpcFeelsCold(RE::Actor* a_actor, float a_air, bool a_rain, bool a_revealing, int a_shift)
	{
		const auto avs = a_actor->AsActorValueOwner();
		const bool resistant = avs && avs->GetActorValue(RE::ActorValue::kResistFrost) >= 50.0f;
		const int  tier = a_air < -14.0f ? 5 : a_air < -9.0f ? 4 : a_air < -4.0f ? 3 : a_air < 1.0f ? 2 : a_air < 6.0f ? 1 : 0;
		const int  felt = tier + (a_revealing && a_rain && !resistant ? 1 : 0);
		const int  threshold = a_revealing ? (resistant ? 4 : 2) : (resistant ? 5 : 4);
		return tier > 0 && felt >= threshold - a_shift;
	}

	bool IsRaining()
	{
		const auto sky = RE::Sky::GetSingleton();
		const auto weather = sky ? sky->currentWeather : nullptr;
		return weather && weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy);
	}

	void UpdateNpcFrost(const Settings& a_settings)
	{
		const auto now = Clock::now();
		if (now - g_lastFrostTick < kFrostTick) {
			return;
		}
		g_lastFrostTick = now;
		const auto lists = RE::ProcessLists::GetSingleton();
		if (!lists || !g_forms.frostShader || !g_forms.frostShaderLower) {
			return;
		}

		std::unordered_map<RE::FormID, std::pair<RE::Actor*, RE::TESEffectShader*>> wanted;
		std::vector<RE::Actor*> cold;     // feel the cold and haven't just been warmed
		std::vector<RE::Actor*> warmers;  // friendly adults close to the player
		const int level = static_cast<int>(std::lround(GlobalValue(g_forms.exposureLevel, 0.0f)));
		const bool running = GlobalValue(g_forms.running, 0.0f) > 1.5f;
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (running && level >= 2 && player) {
			const float air = GlobalValue(g_forms.temperature, 10.0f);
			const bool  rain = IsRaining();
			for (auto& handle : lists->highActorHandles) {
				const auto ptr = handle.get();
				const auto actor = ptr.get();
				if (!actor || actor->IsPlayerRef() || actor->IsDead() || !actor->Is3DLoaded() || IsChildActor(actor)) {
					continue;
				}
				const auto race = actor->GetRace();
				const auto cell = actor->GetParentCell();
				if (!race || !race->HasKeywordString("ActorTypeNPC") || (cell && cell->IsInteriorCell())) {
					continue;
				}
				if (!actor->IsHostileToActor(player) && actor->GetDistance(player) <= kHuddleRange) {
					warmers.push_back(actor);
				}
				const bool revealing = IsRevealing(actor);
				if (ColdImmunity(actor, a_settings) || !NpcFeelsCold(actor, air, rain, revealing, a_settings.coldShift) ||
					RecentlyWarmed(actor->GetFormID(), now)) {
					continue;
				}
				cold.push_back(actor);
				const int npcLevel = (std::min)(level + (revealing ? 1 : 0), 6);
				if (a_settings.npcFrost && level >= 3 && npcLevel >= 4) {
					wanted.emplace(actor->GetFormID(), std::pair{ actor, npcLevel >= 5 ? g_forms.frostShader : g_forms.frostShaderLower });
				}
			}
		}

		std::unordered_map<RE::FormID, std::string> candidates;
		for (const auto warmer : warmers) {
			const bool someone = std::ranges::any_of(cold, [&](RE::Actor* a) {
				return a != warmer && a->GetDistance(warmer) <= kWarmOtherRange;
			});
			candidates.emplace(warmer->GetFormID(), someone ? ""s : "no one cold close by"s);
		}
		{
			std::scoped_lock lock(g_warmOtherLock);
			g_warmOtherCandidates = std::move(candidates);
		}

		// Renew the ones still wanted; end the rest (Frostfall puts these shaders on the player only, so any
		// running on an NPC are ours).
		std::unordered_set<RE::FormID> running_on;
		lists->ForEachShaderEffect([&](RE::ShaderReferenceEffect* a_effect) {
			if (!a_effect || (a_effect->effectData != g_forms.frostShader && a_effect->effectData != g_forms.frostShaderLower)) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto target = a_effect->target.get();
			if (!target || target->IsPlayerRef()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto it = wanted.find(target->GetFormID());
			if (it != wanted.end() && it->second.second == a_effect->effectData && !a_effect->finished) {
				a_effect->age = 0.0f;
				running_on.insert(target->GetFormID());
			} else {
				a_effect->finished = true;
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		for (const auto& [id, entry] : wanted) {
			if (!running_on.contains(id)) {
				entry.first->ApplyEffectShader(entry.second, kFrostShaderSeconds);
				if (a_settings.logAll) {
					const auto name = entry.first->GetName();
					logs::info("Frost shader on {} ({})", name && *name ? name : "?", entry.second == g_forms.frostShader ? "full" : "lighter");
				}
			}
		}
	}

	// --- share body heat visuals --------------------------------------------------------------------------
	// SkyrimNet posts a custom_action event when the YAML action runs; the hug and the glow follow it on the
	// game thread. The warming itself never depends on them: a paired idle can refuse to start (sitting, moving).

	constexpr float kWarmGlowSeconds = 3.0f;
	constexpr float kHugReach = 200.0f;  // a paired idle snaps the pair together; beyond this it looks like a teleport
	constexpr int   kHugAttempts = 10;  // ~20 s: long enough for the rescuer to finish the line that chose the action

	const char* ActorName(RE::Actor* a_actor)
	{
		const auto name = a_actor ? a_actor->GetName() : nullptr;
		return name && *name ? name : "?";
	}

	bool IsSynced(RE::Actor* a_actor)
	{
		bool synced = false;
		a_actor->GetGraphVariableBool("bIsSynced", synced);
		return synced;
	}

	void RunLater(std::chrono::milliseconds a_delay, std::function<void()> a_task)
	{
		std::thread([a_delay, task = std::move(a_task)]() mutable {
			std::this_thread::sleep_for(a_delay);
			SKSE::GetTaskInterface()->AddTask(std::move(task));
		}).detach();
	}

	// PlayIdle only queues the request; the engine drops it while either actor is busy (the hugger is usually
	// still speaking the line that chose the action). A paired idle that actually runs sets bIsSynced on both,
	// so check after a moment and ask again. The one being held starts it with the hugger as partner, as
	// SkyrimNet SexLab's lone hug does; started from the hugger's side the engine accepts it but never runs it.
	void TryHug(RE::ActorHandle a_hugger, RE::ActorHandle a_held, std::function<void()> a_done, int a_attempt = 1)
	{
		const auto huggerRef = a_hugger.get();
		const auto heldRef = a_held.get();
		const auto hugger = huggerRef.get();
		const auto held = heldRef.get();
		if (!hugger || !held || hugger->IsDead() || held->IsDead() || !g_forms.hugIdle) {
			if (a_done) {
				a_done();
			}
			return;
		}
		const auto standing = [](RE::Actor* a) {
			const auto state = a->AsActorState();
			return !a->IsOnMount() && (!state || state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal);
		};
		// A paired idle with a drawn weapon desyncs the graph, so sheathe first and try on the next attempt.
		bool sheathing = false;
		for (const auto a : { hugger, held }) {
			if (const auto state = a->AsActorState(); state && state->IsWeaponDrawn()) {
				a->DrawWeaponMagicHands(false);
				sheathing = true;
			}
		}
		// A paired idle snaps both actors together, so only ask once the hugger is within reach (the action can
		// run well after it was chosen, by which time they may have wandered off), and never in water.
		const auto  swimming = [](RE::Actor* a) { const auto state = a->AsActorState(); return state && state->IsSwimming(); };
		const float distance = hugger->GetDistance(held);
		const char* waiting = sheathing                         ? "waits for weapons to be sheathed" :
		                      swimming(hugger) || swimming(held) ? "waits: in water" :
		                      distance > kHugReach               ? "waits: too far apart" :
		                                                           nullptr;
		const auto process = held->GetActorRuntimeData().currentProcess;
		const bool requested = !waiting && standing(hugger) && standing(held) && process && process->PlayIdle(held, g_forms.hugIdle, hugger);
		logs::info("Hug {} -> {}: attempt {} {} (distance {:.0f})", ActorName(hugger), ActorName(held), a_attempt,
			waiting ? waiting : requested ? "requested" : "refused", distance);
		RunLater(1500ms, [a_hugger, a_held, a_done, a_attempt] {
			const auto huggerRef = a_hugger.get();
			const auto heldRef = a_held.get();
			const auto hugger = huggerRef.get();
			const auto held = heldRef.get();
			if (!hugger || !held) {
				if (a_done) {
					a_done();
				}
				return;
			}
			if (IsSynced(hugger) && IsSynced(held)) {
				logs::info("Hug {} -> {}: running (attempt {})", ActorName(hugger), ActorName(held), a_attempt);
				if (a_done) {
					RunLater(8s, a_done);
				}
				return;
			}
			if (a_attempt < kHugAttempts) {
				RunLater(500ms, [a_hugger, a_held, a_done, a_attempt] { TryHug(a_hugger, a_held, a_done, a_attempt + 1); });
			} else {
				logs::info("Hug {} -> {}: never ran (synced: {}, {})", ActorName(hugger), ActorName(held), IsSynced(hugger), IsSynced(held));
				if (a_done) {
					a_done();
				}
			}
		});
	}

	void PlayHuddle(RE::FormID a_rescuer)
	{
		const auto settings = SnapshotSettings();
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto actor = RE::TESForm::LookupByID<RE::Actor>(a_rescuer);
		if (!player || !actor || actor->IsDead()) {
			return;
		}
		if (settings.huddleHug) {
			TryHug(actor->GetHandle(), player->GetHandle(), nullptr);
		}
		if (settings.huddleGlow && g_forms.warmGlow) {
			player->ApplyEffectShader(g_forms.warmGlow, kWarmGlowSeconds);
			actor->ApplyEffectShader(g_forms.warmGlow, kWarmGlowSeconds);
		}
	}

	// The YAML action borrows SkyrimNet's own skynet_MainController.SetActorDialogueTarget (the warmer turns to
	// the other and walks over, as for a conversation); release them the same way once the hug is over.
	void ClearDialogueTarget(RE::ActorHandle a_actor)
	{
		const auto ref = a_actor.get();
		const auto actor = ref.get();
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		const auto quest = dataHandler ? dataHandler->LookupForm<RE::TESQuest>(0x802, "SkyrimNet.esp") : nullptr;
		if (!actor || !vm || !quest) {
			return;
		}
		const auto policy = vm->GetObjectHandlePolicy();
		const auto handle = policy ? policy->GetHandleForObject(RE::TESQuest::FORMTYPE, quest) : 0;
		if (!handle) {
			return;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
		std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor)) };
		vm->DispatchMethodCall(handle, "skynet_MainController", "ClearActorDialogueTarget", args.get(), result);
	}

	RE::Actor* FindNearbyByName(std::string_view a_name, RE::Actor* a_exclude)
	{
		const auto wanted = Normalise(a_name);
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (player && Normalise(player->GetName()) == wanted) {
			return player;
		}
		const auto lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return nullptr;
		}
		RE::Actor* best = nullptr;
		float      bestDistance = 0.0f;
		for (auto& handle : lists->highActorHandles) {
			const auto ptr = handle.get();
			const auto actor = ptr.get();
			if (!actor || actor == a_exclude || Normalise(actor->GetName()) != wanted) {
				continue;
			}
			const float distance = a_exclude ? actor->GetDistance(a_exclude) : 0.0f;
			if (!best || distance < bestDistance) {
				best = actor;
				bestDistance = distance;
			}
		}
		return best;
	}

	void PlayWarmEachOther(RE::FormID a_warmer, RE::FormID a_targetId, std::string a_targetName)
	{
		const auto settings = SnapshotSettings();
		const auto warmer = RE::TESForm::LookupByID<RE::Actor>(a_warmer);
		if (!warmer || warmer->IsDead()) {
			return;
		}
		auto target = a_targetId ? RE::TESForm::LookupByID<RE::Actor>(a_targetId) : nullptr;
		if (!target && !a_targetName.empty()) {
			target = FindNearbyByName(a_targetName, warmer);
		}
		if (!target || target == warmer || target->IsPlayerRef()) {
			// The player has Share Body Heat, which really warms them; this one is for NPCs only.
			logs::info("WarmEachOther {}: no NPC to warm ({})", ActorName(warmer), target ? ActorName(target) : a_targetName.c_str());
			ClearDialogueTarget(warmer->GetHandle());
			return;
		}
		const auto until = Clock::now() + kWarmedFor;
		g_warmedUntil[warmer->GetFormID()] = until;
		g_warmedUntil[target->GetFormID()] = until;
		g_lastFrostTick = {};  // drop their frost on the next poll
		logs::info("WarmEachOther {} warms {}", ActorName(warmer), ActorName(target));
		const auto warmerHandle = warmer->GetHandle();
		if (settings.huddleHug) {
			TryHug(warmerHandle, target->GetHandle(), [warmerHandle] { ClearDialogueTarget(warmerHandle); });
		} else {
			RunLater(10s, [warmerHandle] { ClearDialogueTarget(warmerHandle); });
		}
		if (settings.huddleGlow && g_forms.warmGlow) {
			warmer->ApplyEffectShader(g_forms.warmGlow, kWarmGlowSeconds);
			target->ApplyEffectShader(g_forms.warmGlow, kWarmGlowSeconds);
		}
	}

	// "" = eligible, else the reason. The warmer needs someone cold close by; the AI picks who.
	std::string WarmOtherBlocker(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return "no actor";
		}
		if (!g_inGame) {
			return "not in game";
		}
		const auto settings = SnapshotSettings();
		if (!settings.warmOthers) {
			return "action switched off";
		}
		if (const auto immune = ColdImmunity(a_actor, settings)) {
			return fmt::format("{}, no body heat to share", immune);
		}
		if (g_huddleLevel < 2) {
			return fmt::format("not cold enough out (player exposure level {})", g_huddleLevel.load());
		}
		if (g_huddleCombat) {
			return "player in combat";
		}
		std::scoped_lock lock(g_warmOtherLock);
		const auto       candidate = g_warmOtherCandidates.find(a_actor->GetFormID());
		if (candidate == g_warmOtherCandidates.end()) {
			return "not a friendly adult near the player outdoors";
		}
		return candidate->second;
	}

	std::string WarmOtherDecorator(RE::Actor* a_actor)
	{
		auto why = WarmOtherBlocker(a_actor);
		if (a_actor) {
			std::scoped_lock lock(g_warmOtherLock);
			auto&            last = g_warmOtherLogged[a_actor->GetFormID()];
			const auto       now = why.empty() ? "yes"s : why;
			if (last != now) {
				last = now;
				logs::info("WarmEachOther {} ({:08X}): {}", ActorName(a_actor), a_actor->GetFormID(), why.empty() ? "eligible" : "no, " + why);
			}
		}
		return why;
	}

	// The 0413 prompt's NPC estimate reads the same switches through these, as prompts can't read plugin settings.
	void RegisterPromptDecorators()
	{
		if (!PublicRegisterDecorator) {
			return;
		}
		const bool shift = PublicRegisterDecorator("frostfall_npc_cold_shift",
			"Frostfall Awareness: the NPC cold shift setting in whole steps, -2 (hardier) to 2 (feel the cold sooner).",
			[](RE::Actor*) { return std::to_string(SnapshotSettings().coldShift); });
		const bool immune = PublicRegisterDecorator("frostfall_cold_immune",
			"Frostfall Awareness: \"vampire\" or \"undead\" when this actor doesn't feel the cold under the current settings, otherwise empty.",
			[](RE::Actor* a_actor) -> std::string {
				const auto why = a_actor ? ColdImmunity(a_actor, SnapshotSettings()) : nullptr;
				return why ? why : "";
			});
		logs::info("Prompt decorators {}", shift && immune ? "registered" : "NOT registered");
	}

	void RegisterWarmOtherDecorator()
	{
		if (!PublicRegisterDecorator) {
			return;
		}
		const bool ok = PublicRegisterDecorator(kWarmOtherDecorator,
			"Frostfall Awareness: empty when this actor could warm another cold NPC close by (outdoors, the player at least Chilly, "
			"not in combat), otherwise the reason they can't.",
			WarmOtherDecorator);
		logs::info("Warm-each-other decorator {}", ok ? "registered" : "NOT registered");
	}

	void OnCustomAction(const char* a_json)
	{
		const std::string_view raw = a_json ? a_json : "";
		const bool player = raw.find("sharing body heat") != std::string_view::npos || raw.find("FrostfallShareBodyHeat") != std::string_view::npos;
		const bool others = raw.find("sharing their warmth") != std::string_view::npos || raw.find("FrostfallWarmEachOther") != std::string_view::npos;
		if (!player && !others) {
			return;
		}
		RE::FormID  actor = 0;
		RE::FormID  target = 0;
		std::string content;
		try {
			const auto json = nlohmann::json::parse(raw);
			actor = json.value("originatingActorFormId", 0u);
			target = json.value("targetActorFormId", 0u);
			content = json.value("data", ""s);
		} catch (...) {
		}
		if (!actor) {
			logs::warn("Warming event without an actor: {}", raw);
			return;
		}
		if (player) {
			SKSE::GetTaskInterface()->AddTask([actor] { PlayHuddle(actor); });
			return;
		}
		// The event text: "<warmer> pulls <target> close, the two sharing their warmth against the cold".
		std::string targetName;
		const auto  from = content.find(" pulls ");
		const auto  to = content.find(" close, the two");
		if (from != std::string::npos && to != std::string::npos && to > from + 7) {
			targetName = content.substr(from + 7, to - from - 7);
		}
		if (SnapshotSettings().logAll) {
			logs::info("WarmEachOther event: {}", raw);
		}
		SKSE::GetTaskInterface()->AddTask([actor, target, targetName] { PlayWarmEachOther(actor, target, targetName); });
	}

	void LogHeatSource(RE::PlayerCharacter* a_player);  // defined with the heat-source lists below

	// --- poll ------------------------------------------------------------------------------------------

	void Poll()
	{
		g_pollQueued = false;
		if (!g_dataReady || !g_inGame) {
			return;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			return;
		}
		const auto settings = SnapshotSettings();
		if (GlobalValue(g_forms.running, 0.0f) < 1.5f) {
			// Frostfall stopped: forget everything so a restart begins quietly.
			g_track = {};
			g_primed = false;
			g_armed.reset();
			g_coldSnapshot.reset();
			g_lastLevel = -1;
			g_huddleLevel = -1;
			UpdateNpcFrost(settings);  // clears any frost left on NPCs
			return;
		}
		UpdateRescue(player, settings);
		UpdateWake();
		UpdateNarration(player, settings);
		UpdateHuddleCandidates(player);
		UpdateSceneWarmth(player, settings);
		QuerySeranaCured();
		UpdateNpcFrost(settings);
		if (settings.logAll) {
			LogHeatSource(player);
		}
		g_primed = true;
	}

	void ResetGameState()
	{
		g_track = {};
		g_lastAnyNarration.reset();
		g_lastCombat.reset();
		g_rescueQuietUntil.reset();
		g_rescueBoxClosedAt.reset();
		{
			std::scoped_lock wakeLock(g_wakeLock);
			g_wakePending.reset();
		}
		g_arrivalPending.reset();
		g_armed.reset();
		g_coldSnapshot.reset();
		g_lastRescue.reset();
		g_lastLevel = -1;
		g_primed = false;
		g_huddleLevel = -1;
		g_inOStim = false;
		g_inScene = false;
		g_lastOStimQuery.reset();
		std::scoped_lock lock(g_huddleLock);
		g_huddleCandidates.clear();
		g_warmedUntil.clear();
		std::scoped_lock warmLock(g_warmOtherLock);
		g_warmOtherCandidates.clear();
		g_warmOtherLogged.clear();
		g_huddleLogged.clear();
	}

	// --- SkyrimNet calls (worker thread) ---------------------------------------------------------------

	// The narration only reaches NPCs; a thought of the player's own, which SkyrimNet shows and voices, lets the
	// player feel it too. SkyrimNet builds it from the current context, the narration just stored included.
	void TriggerPlayerThought()
	{
		const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		if (!vm) {
			return;
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result;
		std::unique_ptr<RE::BSScript::IFunctionArguments>          args{ RE::MakeFunctionArguments() };
		if (!vm->DispatchStaticCall("SkyrimNetApi", "TriggerPlayerThought", args.get(), result)) {
			logs::warn("SkyrimNetApi.TriggerPlayerThought could not be called");
		}
	}

	void PostNarrations()
	{
		std::vector<Narration> pending;
		{
			std::scoped_lock lock(g_queueLock);
			pending.swap(g_narrations);
		}
		// SkyrimNet occasionally refuses an event without logging why; a refused narration is retried twice,
		// a few seconds apart, before it is dropped.
		constexpr int kMaxAttempts = 3;
		std::vector<Narration> retry;
		const auto             now = Clock::now();
		for (auto& n : pending) {
			if (now < n.notBefore) {
				retry.push_back(std::move(n));
				continue;
			}
			const auto type = n.forceReaction ? "direct_narration" : "persistent_generic";
			const auto audience = n.audience.empty() ? nullptr : n.audience.data();
			const auto count = static_cast<std::uint32_t>(n.audience.size());
			if (!PublicRegisterEvent || PublicRegisterEvent(type, n.text.c_str(), n.originator, n.target, audience, count) == 0) {
				if (PublicRegisterEvent && ++n.attempts < kMaxAttempts) {
					logs::info("SkyrimNet did not store the {} (attempt {}), retrying", type, n.attempts);
					n.notBefore = now + 5s;
					retry.push_back(std::move(n));
				} else {
					logs::warn("SkyrimNet did not store the {}: {}", type, n.text);
				}
				continue;
			}
			if (n.attempts > 0) {
				logs::info("{} stored on attempt {}", type, n.attempts + 1);
			}
			if (n.playerThought) {
				SKSE::GetTaskInterface()->AddTask([] { TriggerPlayerThought(); });
			}
		}
		if (!retry.empty()) {
			std::scoped_lock lock(g_queueLock);
			g_narrations.insert(g_narrations.begin(), std::make_move_iterator(retry.begin()), std::make_move_iterator(retry.end()));
		}
	}

	// The LLM answers with a JSON object; tolerate code fences or text around it.
	std::optional<nlohmann::json> ParseReply(std::string_view a_reply)
	{
		const auto open = a_reply.find('{');
		const auto close = a_reply.rfind('}');
		if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
			return std::nullopt;
		}
		try {
			return nlohmann::json::parse(a_reply.substr(open, close - open + 1));
		} catch (const std::exception& e) {
			logs::warn("Rescue memory reply is not valid JSON: {}", e.what());
			return std::nullopt;
		}
	}

	void StoreMemory(RE::FormID a_owner, const nlohmann::json& a_memory, const std::string& a_location, const std::string& a_related)
	{
		const auto content = a_memory.value("content", ""s);
		if (content.empty()) {
			return;
		}
		auto type = a_memory.value("type", "EXPERIENCE"s);
		static constexpr std::array kTypes{ "EXPERIENCE"sv, "RELATIONSHIP"sv, "KNOWLEDGE"sv, "LOCATION"sv, "SKILL"sv, "TRAUMA"sv, "JOY"sv };
		if (std::ranges::find(kTypes, type) == kTypes.end()) {
			type = "EXPERIENCE";
		}
		const auto  emotion = a_memory.value("emotion", ""s);
		const float importance = std::clamp(a_memory.value("importance_score", 0.7f), 0.0f, 1.0f);
		auto        tags = a_memory.contains("tags") && a_memory["tags"].is_array() ? a_memory["tags"] : nlohmann::json::array();
		tags.push_back("frostfall");
		tags.push_back("rescue");
		const auto tagsJson = tags.dump();
		const int  id = PublicAddMemory(a_owner, content.c_str(), importance, type.c_str(), emotion.c_str(), a_location.c_str(),
			tagsJson.c_str(), a_related.c_str());
		if (id > 0) {
			logs::info("Rescue memory {} for {:08X}: {}", id, a_owner, content);
		} else {
			logs::warn("SkyrimNet refused the rescue memory for {:08X}", a_owner);
		}
	}

	void HandleRescueReply(const RescueJob& a_job, std::string_view a_reply)
	{
		const auto reply = ParseReply(a_reply);
		if (!reply) {
			logs::warn("Rescue memory reply could not be read: {}", a_reply);
			return;
		}
		// Every rescuer's memory names the player and the other rescuers as related actors.
		auto ids = nlohmann::json::array({ 0x14 });
		for (const auto& r : a_job.rescuers) {
			ids.push_back(r.formID);
		}

		if (const auto it = reply->find("rescuers"); it != reply->end() && it->is_array()) {
			for (const auto& entry : *it) {
				const auto name = entry.value("name", ""s);
				const auto owner = std::ranges::find_if(a_job.rescuers, [&](const Companion& c) { return c.name == name; });
				if (owner == a_job.rescuers.end()) {
					logs::warn("Rescue memory names an unknown rescuer '{}'", name);
					continue;
				}
				auto related = nlohmann::json::array();
				for (const auto& id : ids) {
					if (id.get<RE::FormID>() != owner->formID) {
						related.push_back(id);
					}
				}
				StoreMemory(owner->formID, entry, a_job.fallLocation, related.dump());
			}
		}
		if (a_job.settings.playerMemory) {
			if (const auto it = reply->find("player"); it != reply->end() && it->is_object()) {
				auto related = nlohmann::json::array();
				for (const auto& r : a_job.rescuers) {
					related.push_back(r.formID);
				}
				StoreMemory(0x14, *it, a_job.fallLocation, related.dump());
			}
		}
		if (a_job.settings.rescueEvent && PublicRegisterEvent) {
			const auto summary = reply->value("event", ""s);
			if (!summary.empty() && PublicRegisterEvent("persistent_generic", summary.c_str(), 0x14, 0, nullptr, 0) == 0) {
				logs::warn("SkyrimNet did not store the rescue event");
			}
		}
		// The player waking is narrated with the first rescuer who may speak as originator, so they get the turn to
		// speak, with the memory of the rescue already theirs. People come first; a creature only when it wrote a
		// memory (SkyrimNet let it) and SkyrimNet doesn't limit dialogue to actors capable of speech.
		std::unordered_set<RE::FormID> remembered;
		if (const auto it = reply->find("rescuers"); it != reply->end() && it->is_array()) {
			for (const auto& entry : *it) {
				const auto name = entry.value("name", ""s);
				if (const auto c = std::ranges::find_if(a_job.rescuers, [&](const Companion& r) { return r.name == name; }); c != a_job.rescuers.end()) {
					remembered.insert(c->formID);
				}
			}
		}
		const auto rescuer = std::ranges::find_if(a_job.rescuers, [&](const Companion& c) {
			return !c.creature || (!a_job.actorSpeechOnly && remembered.contains(c.formID));
		});
		if (a_job.settings.wakeLine && PublicRegisterEvent && rescuer != a_job.rescuers.end()) {
			const std::string their = a_job.playerFemale ? "her" : "his";
			const std::string them = a_job.playerFemale ? "her" : "him";
			const std::string text = a_job.playerName + " stirs and opens " + their + " eyes at " + a_job.arrivalLocation +
			                         ", alive, after " + rescuer->name + " rescued " + them + " from the cold.";
			std::scoped_lock lock(g_wakeLock);
			g_wakePending = WakeRequest{ text, rescuer->formID };
			logs::info("Wake-up narration for {} waits until the player is up: {}", rescuer->name, text);
		}
	}

	void PostRescues()
	{
		std::vector<RescueJob> pending;
		{
			std::scoped_lock lock(g_queueLock);
			pending.swap(g_rescues);
		}
		for (auto& job : pending) {
			if (!PublicSendCustomPromptToLLM || !PublicAddMemory) {
				logs::warn("SkyrimNet has no custom prompt / memory API (needs API v8+): rescue memories skipped");
				continue;
			}
			nlohmann::json ctx;
			ctx["playerName"] = job.playerName;
			ctx["playerSubject"] = job.playerFemale ? "she" : "he";
			ctx["playerObject"] = job.playerFemale ? "her" : "him";
			ctx["playerPossessive"] = job.playerFemale ? "her" : "his";
			ctx["scenarioKind"] = RescueKindName(job.kind);
			ctx["scenario"] = job.scenario;
			ctx["fallLocation"] = job.fallLocation;
			ctx["arrivalLocation"] = job.arrivalLocation;
			ctx["weather"] = job.weather;
			ctx["inWater"] = job.inWater;
			ctx["hoursPassed"] = 2;
			ctx["playerMemory"] = job.settings.playerMemory;
			ctx["memorySpeechOnly"] = job.memorySpeechOnly;
			ctx["rescuers"] = nlohmann::json::array();
			for (const auto& r : job.rescuers) {
				ctx["rescuers"].push_back({ { "name", r.name }, { "race", r.race }, { "formId", r.formID }, { "creature", r.creature } });
			}
			const auto context = ctx.dump();
			auto       shared = std::make_shared<RescueJob>(std::move(job));
			const bool queued = PublicSendCustomPromptToLLM(kRescuePrompt, "", context.c_str(),
				[shared](const char* a_response, int a_success) {
					const std::string reply = a_response ? a_response : "";
					if (!a_success) {
						logs::warn("Rescue memory prompt failed: {}", reply);
						return;
					}
					HandleRescueReply(*shared, reply);
				});
			if (!queued) {
				logs::warn("SkyrimNet did not queue the rescue memory prompt");
			}
		}
	}

	// Pace from our own thread: one queued poll at a time, never a self-requeueing task.
	void WorkerLoop()
	{
		int polls = 0;
		for (;;) {
			std::this_thread::sleep_for(kPollInterval);
			if (++polls >= kReloadEveryPolls) {
				polls = 0;
				ReloadSettings();
			}
			PostNarrations();
			PostRescues();
			if (g_dataReady && g_inGame && !g_pollQueued.exchange(true)) {
				SKSE::GetTaskInterface()->AddTask([] { Poll(); });
			}
		}
	}

	// --- lifecycle -------------------------------------------------------------------------------------

	// Globals and keywords keep their EditorIDs in the game, so the lookup works for both the regular and the
	// ESL build of Frostfall 2026; the FormIDs are the regular build's fallback.
	template <class T>
	T* Resolve(const char* a_editorID, RE::FormID a_localID, std::string_view a_plugin)
	{
		if (auto form = RE::TESForm::LookupByEditorID<T>(a_editorID)) {
			return form;
		}
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		auto       form = dataHandler && a_localID ? dataHandler->LookupForm<T>(a_localID, a_plugin) : nullptr;
		if (!form) {
			logs::warn("{} not found", a_editorID);
		}
		return form;
	}

	void ResolveForms()
	{
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler || !dataHandler->LookupModByName(kFrostfallPlugin)) {
			logs::warn("{} is not loaded: Frostfall Awareness stays idle", kFrostfallPlugin);
			return;
		}
		g_forms.running = Resolve<RE::TESGlobal>("FrostfallRunning", 0x06DCFB, kFrostfallPlugin);
		// Update.esm's read-only mirrors keep the same FormIDs in both Frostfall builds.
		g_forms.exposureLevel = Resolve<RE::TESGlobal>("FrostfallExposureLevelReadOnly", 0xCC0E06, kUpdatePlugin);
		g_forms.exposure = Resolve<RE::TESGlobal>("FrostfallAttributeExposureReadOnly", 0xCC0E03, kUpdatePlugin);
		g_forms.maxExposureMode = Resolve<RE::TESGlobal>("_Frost_Setting_MaxExposureMode", 0x068115, kFrostfallPlugin);
		g_forms.temperature = Resolve<RE::TESGlobal>("FrostfallCurrentTemperatureReadOnly", 0xCC0E07, kUpdatePlugin);
		// Effect shaders and idles have no EditorIDs in the game, so these go by FormID only.
		g_forms.frostShader = dataHandler->LookupForm<RE::TESEffectShader>(0x006EC7, kFrostfallPlugin);
		g_forms.frostShaderLower = dataHandler->LookupForm<RE::TESEffectShader>(0x006EC8, kFrostfallPlugin);
		g_forms.hugIdle = dataHandler->LookupForm<RE::TESIdleForm>(0x0F4699, "Skyrim.esm");
		g_forms.warmGlow = dataHandler->LookupForm<RE::TESEffectShader>(0x10BEA9, "Skyrim.esm");
		if (dataHandler->LookupModByName(kANDPlugin)) {
			for (const auto id : { 0x831u, 0x832u, 0x833u, 0x82Fu, 0x834u, 0x830u, 0x82Eu, 0x835u }) {
				if (const auto faction = dataHandler->LookupForm<RE::TESFaction>(id, kANDPlugin)) {
					g_forms.andRevealing.push_back(faction);
				}
			}
		}
		if (const auto faction = dataHandler->LookupForm<RE::TESFaction>(0x027242, "Skyrim.esm")) {
			g_forms.vampireFactions.push_back(faction);
		}
		if (dataHandler->LookupModByName("Dawnguard.esm")) {
			if (const auto faction = dataHandler->LookupForm<RE::TESFaction>(0x003376, "Dawnguard.esm")) {
				g_forms.vampireFactions.push_back(faction);
			}
			g_forms.serana = dataHandler->LookupForm<RE::TESNPC>(0x002B6C, "Dawnguard.esm");
			g_forms.valerica = dataHandler->LookupForm<RE::TESNPC>(0x003B8B, "Dawnguard.esm");
			g_forms.seranaCure = dataHandler->LookupForm<RE::TESQuest>(0x005044, "Dawnguard.esm");
		}
		logs::info("Visuals: frost shaders {}/{}, A.N.D. factions {}, hug idle {}, warm glow {}", g_forms.frostShaderLower != nullptr,
			g_forms.frostShader != nullptr, g_forms.andRevealing.size(), g_forms.hugIdle != nullptr, g_forms.warmGlow != nullptr);
		// Scene warmth sources, both optional.
		g_slAnimating = dataHandler->LookupModByName("SexLab.esm") ? dataHandler->LookupForm<RE::TESFaction>(0x00E50F, "SexLab.esm") : nullptr;
		g_haveOStim = dataHandler->LookupModByName("OStim.esp") != nullptr;
		logs::info("Scene warmth: SexLab {}, OStim {}", g_slAnimating ? "found" : "not loaded", g_haveOStim ? "found" : "not loaded");
		for (std::size_t i = 0; i < kStateCount; ++i) {
			if (kStates[i].kind == Kind::kFrostbite) {
				g_forms.frostbite[i] = Resolve<RE::BGSKeyword>(kStates[i].keywordEditorID, kStates[i].keywordLocalID, kFrostfallPlugin);
			}
		}

		// Rescue messages: EditorIDs need po3 Tweaks for messages, so the regular build's FormIDs back them up.
		// The text comes from the loaded record, so a translated Frostfall still matches its own message box.
		for (const auto& def : kRescueMessages) {
			auto message = RE::TESForm::LookupByEditorID<RE::BGSMessage>(def.editorID);
			if (!message) {
				message = dataHandler->LookupForm<RE::BGSMessage>(def.localID, kFrostfallPlugin);
			}
			if (!message) {
				continue;
			}
			RE::BSString text;
			message->GetDescription(text, message);
			auto normalised = Normalise(text.c_str() ? text.c_str() : "");
			if (normalised.size() > 40) {
				g_forms.rescueTexts.emplace_back(std::move(normalised), def.kind);
			}
		}
		logs::info("Frostfall forms: running {}, exposure level {}, exposure {}, max exposure mode {}, {} rescue message(s)",
			g_forms.running != nullptr, g_forms.exposureLevel != nullptr, g_forms.exposure != nullptr,
			g_forms.maxExposureMode != nullptr, g_forms.rescueTexts.size());
		if (g_forms.rescueTexts.empty()) {
			logs::warn("Frostfall's rescue messages were not found: rescues are confirmed by Frostfall's time skip and exposure reset only");
		}
		g_dataReady = g_forms.running && g_forms.exposureLevel;
	}

	// Frostfall finds fires by base record (Campfire's _Camp_HeatSources lists). DynDOLOD's large-reference
	// workaround and some mods place copies of vanilla fires under new records with the same model, which it then
	// ignores (giant camps are the usual case). At startup, every object sharing a listed fire's model joins the
	// same lists. Only the lists' loaded entries change, nothing is saved, and DynDOLOD's new numbers after a
	// regeneration are picked up on the next start.
	RE::BGSListForm*               g_heatAll{ nullptr };
	std::array<RE::BGSListForm*, 5> g_heatLists{};  // fire, small, medium, large, other
	RE::TESGlobal*                  g_nearFire{ nullptr };

	std::string HeatListNames(RE::TESForm* a_form)
	{
		static constexpr std::array names{ "fire", "small", "medium", "large", "other" };
		std::string out;
		for (std::size_t i = 0; i < g_heatLists.size(); ++i) {
			if (g_heatLists[i] && g_heatLists[i]->HasForm(a_form)) {
				out += out.empty() ? names[i] : ", "s + names[i];
			}
		}
		return out.empty() ? "no size list" : out;
	}

	std::string ModelPath(RE::TESForm* a_form)
	{
		const auto model = a_form ? a_form->As<RE::TESModel>() : nullptr;
		const auto path = model ? model->GetModel() : nullptr;
		return path ? path : "";
	}

	// Debug (Log All Changes): the heat source Frostfall's own search would pick (closest base in
	// _Camp_HeatSources_All within 600 units), logged whenever it changes, with Frostfall's near-fire flag.
	void LogHeatSource(RE::PlayerCharacter* a_player)
	{
		static Clock::time_point last{};
		static RE::FormID        lastRef = 0;
		static int               lastFlag = -1;
		const auto               now = Clock::now();
		const auto               tes = RE::TES::GetSingleton();
		if (!g_heatAll || !tes || now - last < 5s) {
			return;
		}
		last = now;
		RE::TESObjectREFR* closest = nullptr;
		float              best = 0.0f;
		tes->ForEachReferenceInRange(a_player, 600.0f, [&](RE::TESObjectREFR* a_ref) {
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (base && g_heatAll->HasForm(base) && !a_ref->IsDisabled()) {
				const float d = a_ref->GetDistance(a_player);
				if (!closest || d < best) {
					closest = a_ref;
					best = d;
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		const int        flag = static_cast<int>(GlobalValue(g_nearFire, 0.0f));
		const RE::FormID id = closest ? closest->GetFormID() : 0;
		if (id == lastRef && flag == lastFlag) {
			return;
		}
		lastRef = id;
		lastFlag = flag;
		if (!closest) {
			logs::info("Heat source: none within 600 units (near-fire flag {})", flag);
			return;
		}
		const auto base = closest->GetBaseObject();
		logs::info("Heat source: ref {:08X}, base {:08X} '{}' ({}), {:.0f} units; lists: {}; near-fire flag {}", id, base->GetFormID(),
			ModelPath(base), RE::FormTypeToString(base->GetFormType()), best, HeatListNames(base), flag);
	}

	void AddHeatSourceCopies()
	{
		const auto dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler || !dataHandler->LookupModByName("Campfire.esm")) {
			return;
		}
		const auto list = [&](RE::FormID a_id) { return dataHandler->LookupForm<RE::BGSListForm>(a_id, "Campfire.esm"); };
		const auto all = list(0x028F06);
		const std::array sized{ list(0x02899F), list(0x028F02), list(0x028F03), list(0x028F04), list(0x028F05) };  // fire, small, medium, large, other
		g_heatAll = all;
		g_heatLists = sized;
		g_nearFire = dataHandler->LookupForm<RE::TESGlobal>(0x064AFD, kFrostfallPlugin);
		if (!all) {
			return;
		}
		const auto modelOf = [](RE::TESForm* a_form) -> std::string {
			const auto model = a_form ? a_form->As<RE::TESModel>() : nullptr;
			const auto path = model ? model->GetModel() : nullptr;
			if (!path || !*path) {
				return {};
			}
			std::string key = path;
			std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(c == '/' ? '\\' : std::tolower(c)); });
			if (key.starts_with("meshes\\")) {
				key.erase(0, 7);
			}
			return key;
		};
		// model -> the size lists its known fires are in
		std::unordered_map<std::string, std::unordered_set<RE::BGSListForm*>> known;
		for (const auto form : all->forms) {
			if (auto key = modelOf(form); !key.empty()) {
				auto& lists = known[key];
				for (const auto l : sized) {
					if (l && l->HasForm(form)) {
						lists.insert(l);
					}
				}
			}
		}
		int added = 0;
		for (const auto type : { RE::FormType::Static, RE::FormType::MovableStatic, RE::FormType::Activator, RE::FormType::Light, RE::FormType::Furniture }) {
			for (const auto form : dataHandler->GetFormArray(type)) {
				if (!form || all->HasForm(form)) {
					continue;
				}
				// Only real fires: an original in a fire or size list, never a marker (Update.esm's heat-source
				// override markers share MarkerX.nif with every XMarker in the world).
				const auto key = modelOf(form);
				const auto it = known.find(key);
				if (it == known.end() || it->second.empty() || key.find("marker") != std::string::npos) {
					continue;
				}
				all->forms.push_back(form);
				for (const auto l : it->second) {
					l->forms.push_back(form);
				}
				++added;
				logs::info("Heat source copy {:08X} '{}' ({}) -> {}", form->GetFormID(), ModelPath(form), RE::FormTypeToString(form->GetFormType()), HeatListNames(form));
			}
		}
		logs::info("Heat sources: {} same-model copies of known fires added to Campfire's lists", added);
	}

	void ConnectSkyrimNet()
	{
		if (!FindFunctions() || !PublicGetVersion) {
			logs::warn("SkyrimNet API not found: Frostfall Awareness posts nothing");
			return;
		}
		g_haveSkyrimNet = PublicRegisterEvent != nullptr;
		logs::info("SkyrimNet API v{}; events {}; settings {}; custom prompts {}; memories {}", PublicGetVersion(),
			g_haveSkyrimNet ? "yes" : "NO", PublicGetPluginConfigValue ? "yes" : "NO (defaults)",
			PublicSendCustomPromptToLLM ? "yes" : "NO", PublicAddMemory ? "yes" : "NO");

		if (PublicRegisterEventCallback) {
			for (const auto type : { "dialogue", "dialogue_player_text", "dialogue_player_stt" }) {
				PublicRegisterEventCallback(type, [](const char*) {
					g_lastDialogueTicks = Clock::now().time_since_epoch().count();
				});
			}
			PublicRegisterEventCallback("custom_action", OnCustomAction);
		} else {
			logs::warn("SkyrimNet event callbacks unavailable: narration does not wait for dialogue to settle");
		}
	}

	void ConnectSeverActions()
	{
		const auto module = GetModuleHandleW(L"SeverActionsNative.dll");
		if (!module) {
			return;
		}
		using GetVersionFn = int (*)();
		const auto getVersion = reinterpret_cast<GetVersionFn>(GetProcAddress(module, "SAAPI_GetVersion"));
		if (getVersion && getVersion() >= 1) {
			g_saGetBusyReasons = reinterpret_cast<SAGetBusyReasonsFn>(GetProcAddress(module, "SAAPI_GetBusyReasons"));
		}
		logs::info("SeverActions API {}", g_saGetBusyReasons ? "found: its companions count as rescuers" : "not usable");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			ResolveForms();
			ConnectSkyrimNet();
			ConnectSeverActions();
			RegisterHuddleDecorator();
			RegisterWarmOtherDecorator();
			RegisterPromptDecorators();
			ReloadSettings();
			if (SnapshotSettings().heatCopies) {
				AddHeatSourceCopies();
			}
			if (g_dataReady) {
				if (const auto ui = RE::UI::GetSingleton()) {
					ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuSink::GetSingleton());
				}
				std::thread(WorkerLoop).detach();
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			g_inGame = false;
			SKSE::GetTaskInterface()->AddTask([] { ResetGameState(); });
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			// The payload is the success bool cast into the pointer value.
			g_inGame = a_msg->data != nullptr;
			break;
		case SKSE::MessagingInterface::kNewGame:
			SKSE::GetTaskInterface()->AddTask([] { ResetGameState(); });
			g_inGame = true;
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	SKSE::Init(a_skse, false);
	logs::info("Frostfall Awareness {} loaded", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."));

	if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
		logs::critical("Could not register the SKSE message listener");
		return false;
	}
	return true;
}
